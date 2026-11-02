// SPDX-License-Identifier: GPL-3.0-or-later
// What android-install decides around `lapis app install`, apart from its
// dialog, for tests.

#pragma once

#include <cstdint>
#include <map>
#include <string>

namespace store::install {

// Whether lapis' control service can read the APK where it is: a file the
// user owns, outside the temporary directories (the service has a private
// /tmp). Otherwise it goes through a copy in the user's cache.
bool readableByLapis(const std::string& path);
// Where that copy goes: $XDG_CACHE_HOME/bayu-store/apk/<file name>.
std::string stagingPath(const std::string& path);

// The package an install added or updated: the one new in `after`, or else
// the one whose version code changed. Empty when none did.
std::string changedPackage(const std::map<std::string, int64_t>& before,
                           const std::map<std::string, int64_t>& after);

} // namespace store::install
