/* SPDX-License-Identifier: MIT */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct intel_device_info;

struct anv_gpu_reclaim {
   uint64_t total;
   uint64_t available;
   uint64_t reclaim;
   uint64_t region_free;
   uint64_t region_size;
   uint64_t selected;
   const char *status;
};

bool anv_gpu_reclaim_parse(const char *text, size_t size,
                           struct anv_gpu_reclaim *sample);

void anv_gpu_reclaim_query(const struct intel_device_info *info, int fd,
                           bool enabled, bool diagnostic, uint64_t original,
                           struct anv_gpu_reclaim *sample);
