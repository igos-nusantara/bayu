// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace settings::admin {

// Runs bayu-settings-admin with `arguments` through pkexec, which asks for an
// administrator's password once in a while, on a thread of its own. `done`
// runs on this thread while `alive` is set: with nothing when it succeeded,
// otherwise with what went wrong; an empty text when the password dialog was
// dismissed.
void run(std::vector<std::string> arguments, std::weak_ptr<bool> alive,
         std::function<void(const std::optional<std::string>& error)> done);

} // namespace settings::admin
