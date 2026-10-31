// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

// The daemon under test never touches a VT or logind: every VT is taken at
// once and every switch succeeds. terminalCalls() records the checks and
// switches, in order.

#include "daemon/terminal.h"

#include <string>
#include <vector>

namespace dm {

std::vector<std::string> &terminalCalls()
{
    static std::vector<std::string> calls;
    return calls;
}

bool setGraphicsMode(int)
{
    return true;
}

void restoreTextMode(int)
{
}

bool isControlled(int)
{
    terminalCalls().push_back("controlled");
    return true;
}

int activeVt()
{
    return 0;
}

bool activateSession(const std::string &)
{
    terminalCalls().push_back("activate");
    return true;
}

} // namespace dm
