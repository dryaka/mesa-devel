/* SPDX-License-Identifier: MIT */

#include "anv_gpu_reclaim.h"

#include <ctype.h>
#include <string.h>

#include "dev/intel_device_info.h"
#include "dev/xe/intel_device_info.h"

#ifdef __linux__
#include <fcntl.h>
#include <unistd.h>
#endif

static bool
parse_counter(const char *p, const char *end, uint64_t *bytes)
{
   while (p < end && isspace((unsigned char)*p))
      p++;
   if (p == end || *p < '0' || *p > '9')
      return false;

   uint64_t value = 0;
   while (p < end && *p >= '0' && *p <= '9') {
      unsigned digit = *p++ - '0';
      if (value > (UINT64_MAX - digit) / 10)
         return false;
      value = value * 10 + digit;
   }
   if (p == end || !isspace((unsigned char)*p))
      return false;
   while (p < end && isspace((unsigned char)*p))
      p++;
   if (end - p < 2 || memcmp(p, "kB", 2))
      return false;
   p += 2;
   while (p < end && isspace((unsigned char)*p))
      p++;
   if (p != end || value > UINT64_MAX / 1024)
      return false;
   *bytes = value * 1024;
   return true;
}

bool
anv_gpu_reclaim_parse(const char *text, size_t size,
                      struct anv_gpu_reclaim *sample)
{
   static const char *const names[] = {
      "MemTotal:", "MemAvailable:", "GPUReclaim:",
   };
   uint64_t values[3] = {0};
   unsigned found = 0;
   const char *end = text + size;

   if (memchr(text, '\0', size))
      return false;
   for (const char *p = text; p < end;) {
      const char *newline = memchr(p, '\n', end - p);
      if (!newline)
         return false;
      for (unsigned i = 0; i < 3; i++) {
         size_t length = strlen(names[i]);
         if ((size_t)(newline - p) < length || memcmp(p, names[i], length))
            continue;
         if ((found & (1u << i)) ||
             !parse_counter(p + length, newline, &values[i]))
            return false;
         found |= 1u << i;
      }
      p = newline + 1;
   }
   if ((found & 3) != 3 || values[0] == 0 ||
       values[1] > values[0] || values[2] > values[0] ||
       values[1] > UINT64_MAX - values[2])
      return false;

   sample->total = values[0];
   sample->available = values[1];
   sample->reclaim = values[2];
   return true;
}

void
anv_gpu_reclaim_query(const struct intel_device_info *info, int fd,
                      bool enabled, bool diagnostic, uint64_t original,
                      struct anv_gpu_reclaim *sample)
{
   *sample = (struct anv_gpu_reclaim) {
      .selected = original,
      .status = "disabled",
   };
   if (!enabled && !diagnostic)
      return;

   sample->status = "unsupported";
#ifdef __linux__
   if (info->has_local_mem || info->kmd_type != INTEL_KMD_TYPE_XE)
      return;

   sample->status = "read-failed";
   char text[16384];
   int memfd = open("/proc/meminfo", O_RDONLY | O_CLOEXEC);
   if (memfd < 0)
      return;
   ssize_t size = read(memfd, text, sizeof(text));
   close(memfd);
   if (size <= 0 || size == (ssize_t)sizeof(text))
      return;

   sample->status = "invalid-counters";
   if (!anv_gpu_reclaim_parse(text, size, sample))
      return;
   sample->status = "empty-pool";
   if (!sample->reclaim)
      return;

   sample->status = "query-failed";
   struct intel_device_info raw = *info;
   if (!intel_device_info_xe_query_regions(fd, &raw, true))
      return;
   sample->region_free = raw.mem.sram.mappable.free;
   sample->region_size = raw.mem.sram.mappable.size;
   sample->status = "invalid-region";
   if (sample->region_free > sample->region_size)
      return;

   sample->status = enabled ? "applied" : "disabled";
   if (enabled) {
      sample->selected = MIN3(sample->available + sample->reclaim,
                              sample->total, sample->region_free);
      sample->selected = MIN2(sample->selected, sample->region_size);
   }
#else
   (void)info;
   (void)fd;
#endif
}
