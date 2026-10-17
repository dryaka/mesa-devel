/* SPDX-License-Identifier: MIT */

#include "../anv_gpu_reclaim.h"
#include "dev/intel_device_info.h"
#include "util/u_debug.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int __wrap_open(const char *path, int flags, ...);
int __wrap_open64(const char *path, int flags, ...);
ssize_t __wrap_read(int fd, void *buf, size_t count);
int __wrap_close(int fd);
bool __wrap_intel_device_info_xe_query_regions(int fd,
                                              struct intel_device_info *info,
                                              bool update);

static const char populated[] =
   "MemTotal: 1000 kB\nMemAvailable: 200 kB\nGPUReclaim: 400 kB\n";
static const char *snapshot = populated;
static ssize_t read_size;
static bool open_ok = true, query_ok = true;
static unsigned opens, reads, closes, queries;
static uint64_t region_free = 800 * 1024, region_size = 1000 * 1024;

int
__wrap_open(const char *path, int flags, ...)
{
   assert(!strcmp(path, "/proc/meminfo"));
   assert(flags == (O_RDONLY | O_CLOEXEC));
   opens++;
   return open_ok ? 123 : -1;
}

ssize_t
__wrap_read(int fd, void *buf, size_t count)
{
   assert(fd == 123);
   reads++;
   if (read_size)
      return read_size;
   size_t size = strlen(snapshot);
   assert(size < count);
   memcpy(buf, snapshot, size);
   return size;
}

int
__wrap_open64(const char *path, int flags, ...)
{
   return __wrap_open(path, flags);
}

int
__wrap_close(int fd)
{
   assert(fd == 123);
   closes++;
   return 0;
}

bool
__wrap_intel_device_info_xe_query_regions(int fd,
                                         struct intel_device_info *info,
                                         bool update)
{
   assert(fd == 456 && update);
   queries++;
   info->mem.sram.mappable.free = region_free;
   info->mem.sram.mappable.size = region_size;
   return query_ok;
}

static void
test_parser(void)
{
   const char *const invalid[] = {
      "", "MemTotal: 1000 kB\n", "MemAvailable: 200 kB\n",
      "MemTotal: 0 kB\nMemAvailable: 0 kB\n",
      "MemTotal: 1 kB\nMemAvailable: 2 kB\n",
      "MemTotal: 1 kB\nMemAvailable: 0 kB\nGPUReclaim: 2 kB\n",
      "MemTotal: 1000 kB\nMemAvailable: 200 kB",
      "MemTotal: 1000 kB\nMemAvailable: -1 kB\n",
      "MemTotal: 1000 kB\nMemAvailable: +1 kB\n",
      "MemTotal: 1000 kB\nMemAvailable: 1.5 kB\n",
      "MemTotal: 1000 kB\nMemAvailable: 2 MB\n",
      "MemTotal: 1000 kB\nMemAvailable: 2kB\n",
      "MemTotal: 1000 kB\nMemAvailable: 2 kB junk\n",
      "MemTotal: 1000 kB\nMemAvailable: 2 kB\nMemTotal: 1000 kB\n",
      "MemTotal: 1000 kB\nMemAvailable: 2 kB\nMemAvailable: 2 kB\n",
      "MemTotal: 1000 kB\nMemAvailable: 2 kB\nGPUReclaim: 1 kB\nGPUReclaim: 1 kB\n",
      "MemTotal: 18446744073709551616 kB\nMemAvailable: 1 kB\n",
      "MemTotal: 18014398509481984 kB\nMemAvailable: 1 kB\n",
      "MemTotal: 18014398509481983 kB\nMemAvailable: 18014398509481983 kB\n"
      "GPUReclaim: 1 kB\n",
      "MemTotal: 1000 kB\nMemAvailable: 200 kB\nGPUReclaim: -1 kB\n",
      "MemTotal: 1000 kB\nMemAvailable: 200 kB\nGPUReclaim: 18446744073709551616 kB\n",
   };
   for (unsigned i = 0; i < ARRAY_SIZE(invalid); i++) {
      struct anv_gpu_reclaim sample = {.total = 77, .selected = 99};
      assert(!anv_gpu_reclaim_parse(invalid[i], strlen(invalid[i]), &sample));
      assert(sample.total == 77 && sample.selected == 99);
   }

   struct anv_gpu_reclaim sample = {0};
   assert(anv_gpu_reclaim_parse(populated, strlen(populated), &sample));
   assert(sample.total == 1000 * 1024);
   assert(sample.available == 200 * 1024 && sample.reclaim == 400 * 1024);

   const char reordered[] =
      "GPUActive: 999999 kB\nGPUReclaim: 400 kB\n"
      "MemAvailable:\t200\t kB \t\nMemTotal: 1000 kB\n";
   assert(anv_gpu_reclaim_parse(reordered, sizeof(reordered) - 1, &sample));
   assert(sample.available == 200 * 1024 && sample.reclaim == 400 * 1024);
   const char absent[] = "MemTotal: 1000 kB\nMemAvailable: 200 kB\n";
   assert(anv_gpu_reclaim_parse(absent, sizeof(absent) - 1, &sample));
   assert(sample.reclaim == 0);
   const char nul[] = "MemTotal: 1000 kB\n\0MemAvailable: 200 kB\n";
   assert(!anv_gpu_reclaim_parse(nul, sizeof(nul) - 1, &sample));
}

static void
check_query(struct intel_device_info *info, bool enabled, bool diagnostic,
            const char *status, uint64_t selected)
{
   struct intel_device_info before = *info;
   struct anv_gpu_reclaim sample;
   anv_gpu_reclaim_query(info, 456, enabled, diagnostic, 1234, &sample);
   assert(!strcmp(sample.status, status));
   assert(sample.selected == selected);
   assert(!memcmp(info, &before, sizeof(before)));
}

static void
test_query(void)
{
   struct intel_device_info info = {.kmd_type = INTEL_KMD_TYPE_XE};
   check_query(&info, false, false, "disabled", 1234);
   assert(!opens && !queries);
   info.has_local_mem = true;
   check_query(&info, true, true, "unsupported", 1234);
   info.has_local_mem = false;
   info.kmd_type = INTEL_KMD_TYPE_I915;
   check_query(&info, true, true, "unsupported", 1234);
   assert(!opens && !queries);
   info.kmd_type = INTEL_KMD_TYPE_XE;

   check_query(&info, true, false, "applied", 600 * 1024);
   assert(opens == 1 && reads == 1 && closes == 1 && queries == 1);
   check_query(&info, false, true, "disabled", 1234);
   assert(opens == 2 && queries == 2);
   region_free = 300 * 1024;
   check_query(&info, true, false, "applied", 300 * 1024);
   region_size = region_free = 250 * 1024;
   check_query(&info, true, false, "applied", 250 * 1024);
   region_free = 0;
   check_query(&info, true, false, "applied", 0);
   region_free = region_size + 1;
   check_query(&info, true, false, "invalid-region", 1234);

   region_free = region_size = 2000 * 1024;
   snapshot = "MemTotal: 1000 kB\nMemAvailable: 800 kB\nGPUReclaim: 400 kB\n";
   check_query(&info, true, false, "applied", 1000 * 1024);
   snapshot = "MemTotal: 1000 kB\nMemAvailable: 200 kB\nGPUReclaim: 10 kB\n";
   check_query(&info, true, false, "applied", 210 * 1024);
   snapshot = "MemTotal: 1000 kB\nMemAvailable: -1 kB\n";
   check_query(&info, true, false, "invalid-counters", 1234);
   snapshot = populated;
   check_query(&info, true, false, "applied", 600 * 1024);

   unsigned previous_queries = queries;
   snapshot = "MemTotal: 1000 kB\nMemAvailable: 200 kB\n";
   check_query(&info, true, false, "empty-pool", 1234);
   snapshot = "MemTotal: 1000 kB\nMemAvailable: 200 kB\nGPUReclaim: 0 kB\n";
   check_query(&info, true, false, "empty-pool", 1234);
   assert(queries == previous_queries);
   snapshot = populated;

   query_ok = false;
   check_query(&info, true, false, "query-failed", 1234);
   query_ok = true;
   read_size = -1;
   check_query(&info, true, false, "read-failed", 1234);
   read_size = 16384;
   check_query(&info, true, false, "read-failed", 1234);
   read_size = 0;
   open_ok = false;
   unsigned previous_reads = reads, previous_closes = closes;
   check_query(&info, true, false, "read-failed", 1234);
   assert(reads == previous_reads && closes == previous_closes);
   open_ok = true;
   check_query(&info, true, false, "applied", 600 * 1024);
}

int
main(void)
{
   const char *const enabled[] = {"1", "true", "TRUE", "y", "yes", "t"};
   const char *const disabled[] = {NULL, "", "0", "false", "no", "invalid"};
   for (unsigned i = 0; i < ARRAY_SIZE(enabled); i++)
      assert(debug_parse_bool_option(enabled[i], false));
   for (unsigned i = 0; i < ARRAY_SIZE(disabled); i++)
      assert(!debug_parse_bool_option(disabled[i], false));

   test_parser();
   test_query();
   return 0;
}
