// SPDX-License-Identifier: BSD-3-Clause

#include "pages/accounts/autologin.h"

#include "base/settings.h"

namespace settings {

std::string autologinUser(const std::string& configPath)
{
    return bayu::base::Settings(configPath).stringValue("Autologin/User");
}

} // namespace settings
