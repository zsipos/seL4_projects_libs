/*
 * Copyright 2019, Data61
 * Commonwealth Scientific and Industrial Research Organisation (CSIRO)
 * ABN 41 687 119 230.
 *
 * This software may be distributed and modified according to the terms of
 * the BSD 2-Clause license. Note that NO WARRANTY is provided.
 * See "LICENSE_BSD2.txt" for details.
 *
 * @TAG(DATA61_BSD)
 */

#pragma once

/***
 * @module device_utils.h
 * The device utils interface provides various helpers to establish different types devices for a given VM
 * instance.
 */

#include <sel4vm/guest_vm.h>
#include <sel4vmmplatsupport/device.h>

/***
 * @function vm_install_passthrough_device(vm, device)
 * Install a passthrough device into a VM
 * @param {vm_t *} vm                       A handle to the VM that the device should be install to
 * @param {const struct device *} device    A description of the device
 * @return                                  0 on success, -1 for error
 */
int vm_install_passthrough_device(vm_t *vm, const struct device *device);

/***
 * @function vm_install_ram_only_device(vm, device)
 * Install a device backed by ram into a VM
 * @param {vm_t *} vm                       A handle to the VM that the device should be install to
 * @param {const struct device *} device    A description of the device
 * @return                                  0 on success, -1 for error
 */
int vm_install_ram_only_device(vm_t *vm, const struct device *device);

/***
 * @function vm_install_listening_device(vm, device)
 * Install a passthrough device into a VM, but trap and print all access
 * @param {vm_t *} vm                       A handle to the VM that the device should be install to
 * @param {const struct device *} device    A description of the device
 * @return                                  0 on success, -1 for error
 */
int vm_install_listening_device(vm_t *vm, const struct device *device);
