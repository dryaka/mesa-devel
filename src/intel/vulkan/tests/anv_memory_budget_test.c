/* SPDX-License-Identifier: MIT */

#include "../anv_private.h"
#include "../anv_gpu_reclaim.h"

#include <assert.h>
#include <string.h>

#define MIB (1ull << 20)

bool __wrap_intel_device_info_update_memory_info(struct intel_device_info *info,
                                                int fd);
void __wrap_anv_gpu_reclaim_query(const struct intel_device_info *info, int fd,
                                 bool enabled, bool diagnostic,
                                 uint64_t original,
                                 struct anv_gpu_reclaim *sample);

static bool refresh_ok = true;
static unsigned reclaim_queries, diagnostic_queries;
static uint64_t ordinary = 64 * MIB, selected = 192 * MIB;

bool
__wrap_intel_device_info_update_memory_info(struct intel_device_info *info,
                                           int fd)
{
   assert(fd == 456);
   if (refresh_ok)
      info->mem.sram.mappable.free = ordinary;
   return refresh_ok;
}

void
__wrap_anv_gpu_reclaim_query(const struct intel_device_info *info, int fd,
                            bool enabled, bool diagnostic, uint64_t original,
                            struct anv_gpu_reclaim *sample)
{
   assert(fd == 456 && original == ordinary);
   assert(info->mem.sram.mappable.free == ordinary);
   reclaim_queries++;
   diagnostic_queries += diagnostic;
   *sample = (struct anv_gpu_reclaim) {
      .selected = enabled ? selected : original,
      .status = enabled ? "applied" : "disabled",
   };
}

static void
query(struct anv_physical_device *device, uint64_t first, uint64_t second)
{
   VkPhysicalDeviceMemoryBudgetPropertiesEXT budget = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT,
   };
   memset(budget.heapUsage, 0xff, sizeof(budget.heapUsage));
   memset(budget.heapBudget, 0xff, sizeof(budget.heapBudget));
   VkPhysicalDeviceMemoryProperties2 props = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2,
      .pNext = &budget,
   };
   anv_GetPhysicalDeviceMemoryProperties2(anv_physical_device_to_handle(device),
                                         &props);
   assert(budget.heapBudget[0] == first && budget.heapBudget[1] == second);
   assert(props.memoryProperties.memoryHeapCount == 2);
   for (unsigned i = 0; i < 2; i++) {
      assert(budget.heapUsage[i] == device->memory.heaps_budget->used[i]);
      assert(props.memoryProperties.memoryHeaps[i].size ==
             device->memory.heaps[i].size);
      assert(budget.heapBudget[i] &&
             budget.heapBudget[i] <= device->memory.heaps[i].size);
   }
   for (unsigned i = 2; i < VK_MAX_MEMORY_HEAPS; i++)
      assert(!budget.heapUsage[i] && !budget.heapBudget[i]);
}

int
main(void)
{
   struct anv_memory_budget usage = {.used = {16 * MIB, 8 * MIB}};
   struct anv_physical_device device = {
      .vk.base.type = VK_OBJECT_TYPE_PHYSICAL_DEVICE,
      .vk.supported_extensions.EXT_memory_budget = true,
      .info.kmd_type = INTEL_KMD_TYPE_XE,
      .local_fd = 456,
      .memory.heap_count = 2,
      .memory.heaps = {{.size = 256 * MIB}, {.size = 128 * MIB}},
      .memory.heaps_budget = &usage,
   };

   query(&device, 54 * MIB, 27 * MIB);
   assert(!reclaim_queries);
   device.gpu_reclaim = true;
   query(&device, 131 * MIB, 65 * MIB);
   selected = 512 * MIB;
   query(&device, 246 * MIB, 123 * MIB);
   usage.used[0] = 255 * MIB;
   usage.used[1] = 127 * MIB;
   query(&device, 256 * MIB, 128 * MIB);
   usage.used[0] = 16 * MIB;
   usage.used[1] = 8 * MIB;

   refresh_ok = false;
   unsigned before = reclaim_queries;
   query(&device, 54 * MIB, 27 * MIB);
   assert(device.sys.available == ordinary && reclaim_queries == before);
   refresh_ok = true;
   selected = 192 * MIB;
   query(&device, 131 * MIB, 65 * MIB);

   device.gpu_reclaim = false;
   anv_debug = ANV_DEBUG_MEMORY_BUDGET;
   query(&device, 54 * MIB, 27 * MIB);
   query(&device, 54 * MIB, 27 * MIB);
   assert(diagnostic_queries == 1);
   anv_debug = 0;

   device.gpu_reclaim = true;
   selected = 0;
   usage.used[0] = usage.used[1] = 0;
   query(&device, MIB, MIB);
   return 0;
}
