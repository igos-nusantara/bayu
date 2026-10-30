// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <string>

namespace settings {

// The account bayu-dm logs in at boot without a password ([Autologin] User=
// of its configuration); empty for none.
std::string autologinUser(const std::string& configPath = "/etc/bayu-dm.conf");

} // namespace settings
