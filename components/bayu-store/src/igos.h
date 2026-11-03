// SPDX-License-Identifier: GPL-3.0-or-later
// IGOS Nusantara's own Flatpak remote, "igos" (github.com/igos-nusantara/flathub):
// its apps from the AppStream data Flatpak keeps for the remote. They are
// Flatpak apps like Flathub's and installed the same way, from this remote.
// Blocking: call from a worker thread.
#pragma once

#include <string>
#include <vector>

#include "model.h"

namespace store::igos {

inline constexpr const char* kRemote = "igos";
inline constexpr const char* kRepoFile = "/usr/share/bayu-store/igos.flatpakrepo";

// Adds the remote when missing, refreshes its AppStream and reads it.
std::vector<App> fetchCatalog(std::string* error);
// The apps of an AppStream document; icons from <iconDir>/icons/128x128.
std::vector<App> parseAppstream(const std::string& xml, const std::string& iconDir);

} // namespace store::igos
