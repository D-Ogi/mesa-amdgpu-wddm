/* SPDX-License-Identifier: MIT
 * Private BC250 bootstrap contract, not a registered Vulkan extension.
 * Both endpoints are built together against the same WDK and x64 ABI.
 * Version 1 supports adapter enumeration/paging only, never rendering.
 */
#ifndef BC250_HOST_BOOTSTRAP_H
#define BC250_HOST_BOOTSTRAP_H
#include <stdint.h>
#define BC250_HOST_STYPE 0x42434831u
#define BC250_HOST_VERSION 1u
#define BC250_HOST_CREATE_PAGING 1u
#define BC250_HOST_DESTROY_PAGING 2u
struct bc250_host_paging {
   uint32_t queue, sync;
   void *cpu_address;
};
struct bc250_host {
   uint32_t sType;
   const void *pNext;
   uint32_t version, size;
   uint64_t adapter_luid;
   void *identity;
   void *userdata;
   int32_t (*dispatch)(void *userdata, uint32_t operation, void *argument);
};
#endif
