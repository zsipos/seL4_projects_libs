/*
 * Copyright 2017, Data61
 * Commonwealth Scientific and Industrial Research Organisation (CSIRO)
 * ABN 41 687 119 230.
 *
 * This software may be distributed and modified according to the terms of
 * the GNU General Public License version 2. Note that NO WARRANTY is provided.
 * See "LICENSE_GPLv2.txt" for details.
 *
 * @TAG(DATA61_GPL)
 */

/* Guest specific booting to do with elf loading and bootinfo
 * manipulation */

#include <autoconf.h>
#include <sel4vm/gen_config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cpio/cpio.h>
#include <elf/elf.h>
#include <sel4utils/mapping.h>
#include <vka/capops.h>

#include <sel4vm/guest_vm.h>

#include "sel4vm/debug.h"
#include "sel4vm/processor/platfeature.h"
#include "sel4vm/platform/boot_guest.h"
#include "sel4vm/platform/guest_memory.h"
#include "sel4vm/platform/e820.h"
#include "sel4vm/platform/bootinfo.h"
#include "sel4vm/platform/guest_vspace.h"
#include "sel4vm/platform/elf_helper.h"
#include "sel4vm/platform/acpi.h"

#include <sel4/arch/bootinfo_types.h>

typedef struct boot_guest_cookie {
    vm_t *vm;
    FILE *file;
} boot_guest_cookie_t;

static int guest_elf_write_address(uintptr_t paddr, void *vaddr, size_t size, size_t offset, void *cookie)
{
    memcpy(vaddr, cookie + offset, size);
    return 0;
}

static int guest_elf_read_address(uintptr_t paddr, void *vaddr, size_t size, size_t offset, void *cookie)
{
    memcpy(cookie + offset, vaddr, size);
    return 0;
}

void vmm_plat_guest_elf_relocate(vm_t *vm, const char *relocs_filename) {
    guest_image_t *image = &vm->arch.guest_image;
    int delta = image->relocation_offset;
    if (delta == 0) {
        /* No relocation needed. */
        return;
    }

    uintptr_t load_addr = image->link_paddr;
    DPRINTF(1, "plat: relocating guest kernel from 0x%x --> 0x%x\n", (unsigned int)load_addr,
            (unsigned int)(load_addr + delta));

    /* Open the relocs file. */
    DPRINTF(2, "plat: opening relocs file %s\n", relocs_filename);

    size_t relocs_size = 0;
    FILE *file = fopen(relocs_filename, "r");
    if (!file) {
        printf(COLOUR_Y "ERROR: Guest OS kernel relocation is required, but corresponding"
               "%s was not found. This is most likely due to a Makefile"
               "error, or configuration error.\n", relocs_filename);
        panic("Relocation required but relocation data file not found.");
        return;
    }
    fseek(file, 0, SEEK_END);
    relocs_size = ftell(file);
    fseek(file, 0, SEEK_SET);

    /* The relocs file is the same relocs file format used by the Linux kernel decompressor to
     * relocate the Linux kernel:
     *
     *     0 - zero terminator for 64 bit relocations
     *     64 bit relocation repeated
     *     0 - zero terminator for 32 bit relocations
     *     32 bit relocation repeated
     *     <EOF>
     *
     * So we work backwards from the end of the file, and modify the guest kernel OS binary.
     * We only support 32-bit relocations, and ignore the 64-bit data.
     *
     * src: Linux kernel 3.5.3 arch/x86/boot/compressed/misc.c
     */
    uint32_t last_relocated_vaddr = 0xFFFFFFFF;
    uint32_t num_relocations = relocs_size / sizeof(uint32_t) - 1;
    for (int i = 0; ; i++) {
        uint32_t vaddr;
        /* Get the next relocation from the relocs file. */
        uint32_t offset = relocs_size - (sizeof(uint32_t) * (i + 1));
        fseek(file, offset, SEEK_SET);
        size_t result = fread(&vaddr, sizeof(uint32_t), 1, file);
        ZF_LOGF_IF(result != 1, "Read failed unexpectedly");
        if (!vaddr) {
            break;
        }
        assert(i * sizeof(uint32_t) < relocs_size);
        last_relocated_vaddr = vaddr;

        /* Calculate the corresponding guest-physical address at which we have already
           allocated and mapped the ELF contents into. */
        assert(vaddr >= (uint32_t)image->link_vaddr);
        uintptr_t guest_paddr = (uintptr_t)vaddr - (uintptr_t)image->link_vaddr +
                                (uintptr_t)(load_addr + delta);
//        assert(vmm_guest_mem_check_elf_segment(resource, guest_paddr, guest_paddr + 4));

        /* Perform the relocation. */
        DPRINTF(5, "   reloc vaddr 0x%x guest_addr 0x%x\n", (unsigned int)vaddr, (unsigned int)guest_paddr);
        uint32_t addr;
        vmm_guest_vspace_touch(&vm->mem.vm_vspace, guest_paddr, sizeof(int),
                guest_elf_read_address, &addr);
        addr += delta;
        vmm_guest_vspace_touch(&vm->mem.vm_vspace, guest_paddr, sizeof(int),
                guest_elf_write_address, &addr);

        if (i && i % 50000 == 0) {
            DPRINTF(2, "    %u relocs done.\n", i);
        }
    }
    DPRINTF(3, "plat: last relocated addr was %d\n", last_relocated_vaddr);
    DPRINTF(2, "plat: %d kernel relocations completed.\n", num_relocations);
    (void) last_relocated_vaddr;

    if (num_relocations == 0) {
        panic("Relocation required, but Kernel has not been build with CONFIG_RELOCATABLE.");
    }

    fclose(file);

}

static int vmm_guest_load_boot_module_continued(uintptr_t paddr, void *addr, size_t size, size_t offset, void *cookie)
{
    boot_guest_cookie_t *pass = (boot_guest_cookie_t *) cookie;
    fseek(pass->file, offset, SEEK_SET);
    size_t result = fread(addr, size, 1, pass->file);
    ZF_LOGF_IF(result != 1, "Read failed unexpectedly");

    return 0;
}

int vmm_guest_load_boot_module(vm_t *vm, const char *name) {
    uintptr_t load_addr = guest_ram_largest_free_region_start(&vm->mem);
    printf("Loading boot module \"%s\" at 0x%x\n", name, (unsigned int)load_addr);

    size_t initrd_size = 0;
    FILE *file = fopen(name, "r");
    if (!file) {
        ZF_LOGE("Boot module \"%s\" not found.", name);
        return -1;
    }
    fseek(file, 0, SEEK_END);
    initrd_size = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (!initrd_size) {
        ZF_LOGE("Boot module has zero size. This is probably not what you want.");
        return -1;
    }

    vm->arch.guest_image.boot_module_paddr = load_addr;
    vm->arch.guest_image.boot_module_size = initrd_size;

    guest_ram_mark_allocated(&vm->mem, load_addr, initrd_size);
    boot_guest_cookie_t pass = { .vm = vm, .file = file};
    vmm_guest_vspace_touch(&vm->mem.vm_vspace, load_addr, initrd_size, vmm_guest_load_boot_module_continued, &pass);

    printf("Guest memory after loading initrd:\n");
    print_guest_ram_regions(&vm->mem);

    fclose(file);

    return 0;
}

static inline uint32_t vmm_plat_vesa_fbuffer_size(seL4_VBEModeInfoBlock_t *block)
{
    assert(block);
    return ALIGN_UP(block->vbe_common.bytesPerScanLine * block->vbe12_part1.yRes, 65536);
}

static int make_guest_cmd_line_continued(uintptr_t phys, void *vaddr, size_t size, size_t offset, void *cookie)
{
    /* Copy the string to this area. */
    const char *cmdline = (const char *)cookie;
    memcpy(vaddr, cmdline + offset, size);
    return 0;
}

static int make_guest_cmd_line(vm_t *vm, const char *cmdline) {
    /* Allocate command line from guest ram */
    int len = strlen(cmdline);
    uintptr_t cmd_addr = guest_ram_allocate(&vm->mem, len + 1);
    if (cmd_addr == 0) {
        ZF_LOGE("Failed to allocate guest cmdline (length %d)", len);
        return -1;
    }
    printf("Constructing guest cmdline at 0x%x of size %d\n", (unsigned int)cmd_addr, len);
    vm->arch.guest_image.cmd_line = cmd_addr;
    vm->arch.guest_image.cmd_line_len = len;
    return vmm_guest_vspace_touch(&vm->mem.vm_vspace, cmd_addr, len + 1, make_guest_cmd_line_continued, (void*)cmdline);
}

static void make_guest_screen_info(vm_t *vm, struct screen_info *info) {
    /* VESA information */
    seL4_X86_BootInfo_VBE vbeinfo;
    ssize_t result;
    int error;
    result = simple_get_extended_bootinfo(vm->simple, SEL4_BOOTINFO_HEADER_X86_VBE, &vbeinfo, sizeof(seL4_X86_BootInfo_VBE));
    uintptr_t base = 0;
    size_t fbuffer_size;
    if (config_set(CONFIG_VMM_VESA_FRAMEBUFFER) && result != -1) {
        /* map the protected mode interface at the same location we are told about it at.
         * this is just to guarantee that it ends up within the segment addressable range */
        uintptr_t pm_base = (vbeinfo.vbeInterfaceSeg << 4) + vbeinfo.vbeInterfaceOff;
        error = 0;
        if (pm_base > 0xc000) {
            /* construct a page size and aligned region to map */
            uintptr_t aligned_pm = ROUND_DOWN(pm_base, PAGE_SIZE_4K);
            int size = vbeinfo.vbeInterfaceLen + (pm_base - aligned_pm);
            size = ROUND_UP(size, PAGE_SIZE_4K);
            error = vmm_map_guest_device_at(vm, aligned_pm, aligned_pm, size);
        }
        if (error) {
            ZF_LOGE("Failed to map vbe protected mode interface for VESA frame buffer. Disabling");
        } else {
            fbuffer_size = vmm_plat_vesa_fbuffer_size(&vbeinfo.vbeModeInfoBlock);
            base = vmm_map_guest_device(vm, vbeinfo.vbeModeInfoBlock.vbe20.physBasePtr, fbuffer_size, PAGE_SIZE_4K);
            if (!base) {
                ZF_LOGE("Failed to map base pointer for VESA frame buffer. Disabling");
            }
        }
    }
    if (base) {
        info->orig_video_isVGA = 0x23; // Tell Linux it's a VESA mode
        info->lfb_width = vbeinfo.vbeModeInfoBlock.vbe12_part1.xRes;
        info->lfb_height = vbeinfo.vbeModeInfoBlock.vbe12_part1.yRes;
        info->lfb_depth = vbeinfo.vbeModeInfoBlock.vbe12_part1.bitsPerPixel;

        info->lfb_base = base;
        info->lfb_size = fbuffer_size >> 16;
        info->lfb_linelength = vbeinfo.vbeModeInfoBlock.vbe_common.bytesPerScanLine;

        info->red_size = vbeinfo.vbeModeInfoBlock.vbe12_part2.redLen;
        info->red_pos = vbeinfo.vbeModeInfoBlock.vbe12_part2.redOff;
        info->green_size = vbeinfo.vbeModeInfoBlock.vbe12_part2.greenLen;
        info->green_pos = vbeinfo.vbeModeInfoBlock.vbe12_part2.greenOff;
        info->blue_size = vbeinfo.vbeModeInfoBlock.vbe12_part2.blueLen;
        info->blue_pos = vbeinfo.vbeModeInfoBlock.vbe12_part2.blueOff;
        info->rsvd_size = vbeinfo.vbeModeInfoBlock.vbe12_part2.rsvdLen;
        info->rsvd_pos = vbeinfo.vbeModeInfoBlock.vbe12_part2.rsvdOff;
        info->vesapm_seg = vbeinfo.vbeInterfaceSeg;
        info->vesapm_off = vbeinfo.vbeInterfaceOff;
        info->pages = vbeinfo.vbeModeInfoBlock.vbe12_part1.planes;
    } else {
        memset(info, 0, sizeof(*info));
    }
}

static int make_guest_e820_map(struct e820entry *e820, vm_mem_t *guest_memory) {
    int i;
    int entry = 0;
    printf("Constructing e820 memory map for guest with:\n");
    print_guest_ram_regions(guest_memory);
    /* Create an initial entry at 0 that is reserved */
    e820[entry].addr = 0;
    e820[entry].size = 0;
    e820[entry].type = E820_RESERVED;
    assert(guest_memory->num_ram_regions > 0);
    for (i = 0; i < guest_memory->num_ram_regions; i++) {
        /* Check for discontinuity. We need this check since we can have multiple
         * regions that are contiguous if they have different allocation flags.
         * However we are reporting ALL of this memory to the guest */
        if (e820[entry].addr + e820[entry].size != guest_memory->ram_regions[i].start) {
            /* Finish region. Unless it was zero sized */
            if (e820[entry].size != 0) {
                entry++;
                assert(entry < E820MAX);
                e820[entry].addr = e820[entry - 1].addr + e820[entry - 1].size;
                e820[entry].type = E820_RESERVED;
            }
            /* Pad region */
            e820[entry].size = guest_memory->ram_regions[i].start - e820[entry].addr;
            /* Now start a new RAM region */
            entry++;
            assert(entry < E820MAX);
            e820[entry].addr = guest_memory->ram_regions[i].start;
            e820[entry].type = E820_RAM;
        }
        /* Increase region to size */
        e820[entry].size = guest_memory->ram_regions[i].start - e820[entry].addr + guest_memory->ram_regions[i].size;
    }
    /* Create empty region at the end */
    entry++;
    assert(entry < E820MAX);
    e820[entry].addr = e820[entry - 1].addr + e820[entry - 1].size;
    e820[entry].size = 0x100000000ull - e820[entry].addr;
    e820[entry].type = E820_RESERVED;
    printf("Final e820 map is:\n");
    for (i = 0; i <= entry; i++) {
        printf("\t0x%x - 0x%x type %d\n", (unsigned int)e820[i].addr, (unsigned int)(e820[i].addr + e820[i].size),
               e820[i].type);
        assert(e820[i].addr < e820[i].addr + e820[i].size);
    }
    return entry + 1;
}

static int make_guest_boot_info(vm_t *vm) {
    /* TODO: Bootinfo struct needs to be allocated in location accessable by real mode? */
    uintptr_t addr = guest_ram_allocate(&vm->mem, sizeof(struct boot_params));
    if (addr == 0) {
        ZF_LOGE("Failed to allocate %zu bytes for guest boot info struct", sizeof(struct boot_params));
        return -1;
    }
    printf("Guest boot info allocated at %p. Populating...\n", (void*)addr);
    vm->arch.guest_image.boot_info = addr;

    /* Map in BIOS boot info structure. */
    struct boot_params boot_info;
    memset(&boot_info, 0, sizeof(struct boot_params));

    /* Initialise basic bootinfo structure. Src: Linux kernel Documentation/x86/boot.txt */
    boot_info.hdr.header = 0x53726448; /* Magic number 'HdrS' */
    boot_info.hdr.boot_flag = 0xAA55; /* Magic number for Linux. */
    boot_info.hdr.type_of_loader = 0xFF; /* Undefined loeader type. */
    boot_info.hdr.code32_start = vm->arch.guest_image.load_paddr;
    boot_info.hdr.kernel_alignment = vm->arch.guest_image.alignment;
    boot_info.hdr.relocatable_kernel = true;

    /* Set up screen information. */
    /* Tell Guest OS about VESA mode. */
    make_guest_screen_info(vm, &boot_info.screen_info);

    /* Create e820 memory map */
    boot_info.e820_entries = make_guest_e820_map(boot_info.e820_map, &vm->mem);

    /* Pass in the command line string. */
    boot_info.hdr.cmd_line_ptr = vm->arch.guest_image.cmd_line;
    boot_info.hdr.cmdline_size = vm->arch.guest_image.cmd_line_len;

    /* These are not needed to be precise, because Linux uses these values
     * only to raise an error when the decompression code cannot find good
     * space. ref: GRUB2 source code loader/i386/linux.c */
    boot_info.alt_mem_k = 0;//((32 * 0x100000) >> 10);

    /* Pass in initramfs. */
    if (vm->arch.guest_image.boot_module_paddr) {
        boot_info.hdr.ramdisk_image = (uint32_t) vm->arch.guest_image.boot_module_paddr;
        boot_info.hdr.ramdisk_size = vm->arch.guest_image.boot_module_size;
        boot_info.hdr.root_dev = 0x0100;
        boot_info.hdr.version = 0x0204; /* Report version 2.04 in order to report ramdisk_image. */
    } else {
        boot_info.hdr.version = 0x0202;
    }
    return vmm_guest_vspace_touch(&vm->mem.vm_vspace, addr, sizeof(boot_info), guest_elf_write_address, &boot_info);
}

/* Init the guest page directory, cmd line args and boot info structures. */
void vmm_plat_init_guest_boot_structure(vm_t *vm, const char *cmdline) {
    int UNUSED err;

    err = make_guest_cmd_line(vm, cmdline);
    assert(!err);

    err = make_guest_boot_info(vm);
    assert(!err);

    err = make_guest_acpi_tables(vm);
    assert(!err);
}

void vmm_init_guest_thread_state(vm_vcpu_t *vcpu) {
    vmm_set_user_context(&vcpu->vcpu_arch.guest_state, USER_CONTEXT_EAX, 0);
    vmm_set_user_context(&vcpu->vcpu_arch.guest_state, USER_CONTEXT_EBX, 0);
    vmm_set_user_context(&vcpu->vcpu_arch.guest_state, USER_CONTEXT_ECX, 0);
    vmm_set_user_context(&vcpu->vcpu_arch.guest_state, USER_CONTEXT_EDX, 0);

    /* Entry point. */
    printf("Initializing guest to start running at 0x%x\n", (unsigned int)vcpu->vm->arch.guest_image.entry);
    vmm_guest_state_set_eip(&vcpu->vcpu_arch.guest_state, vcpu->vm->arch.guest_image.entry);
    /* The boot_param structure. */
    vmm_set_user_context(&vcpu->vcpu_arch.guest_state, USER_CONTEXT_ESI, vcpu->vm->arch.guest_image.boot_info);
}

/* TODO: Refactor and stop rewriting fucking elf loading code */
static int vmm_load_guest_segment(vm_t *vm, seL4_Word source_offset,
        seL4_Word dest_addr, unsigned int segment_size, unsigned int file_size, FILE *file) {

    int ret;
    unsigned int page_size = vm->mem.page_size;
    assert(file_size <= segment_size);

    /* Allocate a cslot for duplicating frame caps */
    cspacepath_t dup_slot;
    ret = vka_cspace_alloc_path(vm->vka, &dup_slot);
    if (ret) {
        ZF_LOGE("Failed to allocate slot");
        return ret;
    }

    size_t current = 0;
    size_t remain = file_size;
    while (current < segment_size) {
        /* Retrieve the mapping */
        seL4_CPtr cap;
        cap = vspace_get_cap(&vm->mem.vm_vspace, (void*)dest_addr);
        if (!cap) {
            ZF_LOGE("Failed to find frame cap while loading elf segment at %p", (void *)dest_addr);
            return -1;
        }
        cspacepath_t cap_path;
        vka_cspace_make_path(vm->vka, cap, &cap_path);

        /* Copy cap and map into our vspace */
        vka_cnode_copy(&dup_slot, &cap_path, seL4_AllRights);
        void *map_vaddr = vspace_map_pages(&vm->mem.vmm_vspace, &dup_slot.capPtr, NULL, seL4_AllRights, 1, page_size, 1);
        if (!map_vaddr) {
            ZF_LOGE("Failed to map page into host vspace");
            return -1;
        }

        /* Copy the contents of page from ELF into the mapped frame. */
        size_t offset = dest_addr & ((BIT(page_size)) - 1);

        void *copy_vaddr = map_vaddr + offset;
        size_t copy_len = (BIT(page_size)) - offset;

        if (remain > 0) {
            if (copy_len > remain) {
                /* Don't copy past end of data. */
                copy_len = remain;
            }

            DPRINTF(5, "load page src %zu dest %p remain %zu offset %zu copy vaddr %p "
                    "copy len %zu\n", source_offset, (void *)dest_addr, remain, offset, copy_vaddr, copy_len);

            fseek(file, source_offset, SEEK_SET);
            size_t result = fread(copy_vaddr, copy_len, 1, file);
            ZF_LOGF_IF(result != 1, "Read failed unexpectedly");
            source_offset += copy_len;
            remain -= copy_len;
        } else {
            memset(copy_vaddr, 0, copy_len);
        }

        dest_addr += copy_len;

        current += copy_len;

        /* Unamp the page and delete the temporary cap */
        vspace_unmap_pages(&vm->mem.vmm_vspace, map_vaddr, 1, page_size, NULL);
        vka_cnode_delete(&dup_slot);
    }

    return 0;
}

/* Load the actual ELF file contents into pre-allocated frames.
   Used for both host and guest threads.

 @param resource    the thread structure for resource
 @param elf_name    the name of elf file

*/
/* TODO: refactor yet more elf loading code */
int vmm_load_guest_elf(vm_t *vm, const char *elfname, size_t alignment) {
    int ret;
    char elf_file[256];
    elf_t elf;

    DPRINTF(4, "Loading guest elf %s\n", elfname);
    FILE *file = fopen(elfname, "r");
    if (!file) {
        ZF_LOGE("Guest elf \"%s\" not found.", elfname);
        return -1;
    }

    ret = vmm_read_elf_headers(elf_file, vm, file, sizeof(elf_file), &elf);
    if(ret < 0) {
        ZF_LOGE("Guest elf \"%s\" invalid.", elfname);
        return -1;
    }

    unsigned int n_headers = elf_getNumProgramHeaders(&elf);

    /* Find the largest guest ram region and use that for loading */
    uintptr_t load_addr = guest_ram_largest_free_region_start(&vm->mem);
    /* Round up by the alignemnt. We just hope its still in the memory region.
     * if it isn't we will just fail when we try and get the frame */
    load_addr = ROUND_UP(load_addr, alignment);
    /* Calculate relocation offset. */
    uintptr_t guest_kernel_addr = 0xFFFFFFFF;
    uintptr_t guest_kernel_vaddr = 0xFFFFFFFF;
    for (int i = 0; i < n_headers; i++) {
        if (elf_getProgramHeaderType(&elf, i) != PT_LOAD) {
            continue;
        }
        uint32_t addr = elf_getProgramHeaderPaddr(&elf, i);
        if (addr < guest_kernel_addr) {
            guest_kernel_addr = addr;
        }
        uint32_t vaddr = elf_getProgramHeaderVaddr(&elf, i);
        if (vaddr < guest_kernel_vaddr) {
            guest_kernel_vaddr = vaddr;
        }
    }

    printf("Guest kernel is compiled to be located at paddr 0x%x vaddr 0x%x\n",
           (unsigned int)guest_kernel_addr, (unsigned int)guest_kernel_vaddr);
    printf("Guest kernel allocated 1:1 start is at paddr = 0x%x\n", (unsigned int)load_addr);
    int guest_relocation_offset = (int)((int64_t)load_addr - (int64_t)guest_kernel_addr);
    printf("Therefore relocation offset is %d (%s0x%x)\n",
           guest_relocation_offset,
           guest_relocation_offset < 0 ? "-" : "",
           abs(guest_relocation_offset));

    for (int i = 0; i < n_headers; i++) {
        seL4_Word source_offset, dest_addr;
        unsigned int file_size, segment_size;

        /* Skip unloadable program headers. */
        if (elf_getProgramHeaderType(&elf, i) != PT_LOAD) {
            continue;
        }

        /* Fetch information about this segment. */
        source_offset = elf_getProgramHeaderOffset(&elf, i);
        file_size = elf_getProgramHeaderFileSize(&elf, i);
        segment_size = elf_getProgramHeaderMemorySize(&elf, i);

        dest_addr = elf_getProgramHeaderPaddr(&elf, i);
        dest_addr += guest_relocation_offset;

        if (!segment_size) {
            /* Zero sized segment, ignore. */
            continue;
        }

        /* Load this ELf segment. */
        ret = vmm_load_guest_segment(vm, source_offset, dest_addr, segment_size, file_size, file);
        if (ret) {
            return ret;
        }

        /* Record it as allocated */
        guest_ram_mark_allocated(&vm->mem, dest_addr, segment_size);
    }

    /* Record the entry point. */
    vm->arch.guest_image.entry = elf_getEntryPoint(&elf);
    vm->arch.guest_image.entry += guest_relocation_offset;

    /* Remember where we started loading the kernel to fix up relocations in future */
    vm->arch.guest_image.load_paddr = load_addr;
    vm->arch.guest_image.link_paddr = guest_kernel_addr;
    vm->arch.guest_image.link_vaddr = guest_kernel_vaddr;
    vm->arch.guest_image.relocation_offset = guest_relocation_offset;
    vm->arch.guest_image.alignment = alignment;

    printf("Guest memory layout after loading elf\n");
    print_guest_ram_regions(&vm->mem);

    fclose(file);

    return 0;
}