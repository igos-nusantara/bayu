// SPDX-License-Identifier: BSD-3-Clause

#include "pages/memory/zram.h"

#include <sstream>

#include "base/file.h"
#include "base/strings.h"

using namespace bayu;

namespace settings::memory {

ZramMode zramModeOf(const std::string& config)
{
    bool inZram0 = false;
    bool hasZram0 = false;
    std::string size;
    for (const std::string_view raw : base::strings::splitViews(config, '\n')) {
        const std::string line(base::strings::trimmed(raw));
        if (line.empty() || line.front() == '#' || line.front() == ';')
            continue;
        if (line.front() == '[') {
            inZram0 = line == "[zram0]";
            hasZram0 = hasZram0 || inZram0;
            continue;
        }
        const size_t equals = line.find('=');
        if (inZram0 && equals != std::string::npos
            && base::strings::trimmed(std::string_view(line).substr(0, equals)) == "zram-size")
            size = base::strings::trimmed(std::string_view(line).substr(equals + 1));
    }
    if (!hasZram0)
        return ZramMode::Off;
    // zram-generator's default size, when the section names none.
    if (size.empty() || size == "min(ram / 2, 8192)" || size == "min(ram / 2, 4096)")
        return ZramMode::Auto;
    if (size == "ram / 4")
        return ZramMode::Quarter;
    if (size == "ram / 2")
        return ZramMode::Half;
    return ZramMode::Custom;
}

ZramMode zramMode(const std::string& etc, const std::string& usr)
{
    if (const auto config = base::file::read(etc))
        return zramModeOf(*config);
    if (const auto config = base::file::read(usr))
        return zramModeOf(*config);
    return ZramMode::Off;
}

std::string zramArgument(ZramMode mode)
{
    switch (mode) {
    case ZramMode::Off: return "off";
    case ZramMode::Quarter: return "25";
    case ZramMode::Half: return "50";
    case ZramMode::Auto:
    case ZramMode::Custom: break;
    }
    return "auto";
}

ZramUsage zramUsageOf(const std::string& disksize, const std::string& mmStat)
{
    ZramUsage usage;
    std::istringstream(disksize) >> usage.size;
    // orig_data_size compr_data_size mem_used_total ...
    std::istringstream stat(mmStat);
    uint64_t compressed = 0;
    stat >> usage.stored >> compressed >> usage.memoryUsed;
    return usage;
}

ZramUsage zramUsage(const std::string& device)
{
    return zramUsageOf(base::file::read(device + "/disksize").value_or(""),
                       base::file::read(device + "/mm_stat").value_or(""));
}

} // namespace settings::memory
