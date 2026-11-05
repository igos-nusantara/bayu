// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <cstdint>
#include <string>

namespace settings::memory {

// The memory compression Settings sets: zram-generator's zram0, which
// bayu-settings-admin writes to /etc/systemd/zram-generator.conf.d/bayu.conf
// over Bayu's default in /usr/lib.
enum class ZramMode { Off, Auto, Quarter, Half, Custom };

// From a zram-generator configuration: its [zram0] zram-size; Off without
// a [zram0].
ZramMode zramModeOf(const std::string& config);
// The configuration in effect: /etc's bayu.conf, else /usr/lib's.
ZramMode zramMode(const std::string& etc = "/etc/systemd/zram-generator.conf.d/bayu.conf",
                  const std::string& usr = "/usr/lib/systemd/zram-generator.conf.d/bayu.conf");
// The helper's argument for a mode: auto, 25, 50 or off.
std::string zramArgument(ZramMode mode);

// What zram0 holds now, from sysfs; all 0 while there is none.
struct ZramUsage
{
    uint64_t size = 0;       // the swap's size
    uint64_t stored = 0;     // what was swapped to it
    uint64_t memoryUsed = 0; // the memory that takes, compressed
};
ZramUsage zramUsageOf(const std::string& disksize, const std::string& mmStat);
ZramUsage zramUsage(const std::string& device = "/sys/block/zram0");

} // namespace settings::memory
