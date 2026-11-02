// SPDX-License-Identifier: GPL-3.0-or-later

#include "android_install.h"

#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>

#include "base/paths.h"

namespace store::install {

bool readableByLapis(const std::string& path)
{
    std::error_code error;
    const std::string real = std::filesystem::canonical(path, error).string();
    if (error)
        return false;
    for (const char* temporary : {"/tmp/", "/var/tmp/", "/dev/shm/"}) {
        if (real.starts_with(temporary))
            return false;
    }
    struct stat st;
    return ::stat(real.c_str(), &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == ::getuid();
}

std::string stagingPath(const std::string& path)
{
    return bayu::base::paths::cacheHome() + "/bayu-store/apk/" + std::filesystem::path(path).filename().string();
}

std::string changedPackage(const std::map<std::string, int64_t>& before,
                           const std::map<std::string, int64_t>& after)
{
    std::string updated;
    for (const auto& [package, version] : after) {
        const auto old = before.find(package);
        if (old == before.end())
            return package;
        if (updated.empty() && old->second != version)
            updated = package;
    }
    return updated;
}

} // namespace store::install
