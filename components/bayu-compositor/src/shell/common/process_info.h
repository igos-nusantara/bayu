// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BAYU_SHELL_COMMON_PROCESS_INFO_H_
#define BAYU_SHELL_COMMON_PROCESS_INFO_H_

#include <cstdint>
#include <string>

namespace bayu::shell {

// The program a process runs, as /proc says it, for matching processes to
// their desktop entries. procfs is answered from memory, so these never wait
// on a disk.
struct ProcessCommand {
    std::string program;
    // The program's file name.
    std::string name;
    // A Flatpak's application id, from its sandbox's /.flatpak-info: its
    // program (/app/bin/sol) and window app id ("sol") are not its desktop
    // entry's (org.gnome.Aisleriot, which runs flatpak).
    std::string flatpakId;
};

// Empty when /proc cannot say. A script started through its shebang runs as
// the interpreter, which would match every application written in that
// language: the script stands for it instead, and inline code (-c, -e) or a
// module (-m) for nothing.
ProcessCommand processCommand(int64_t pid);

// The [Application] name of a .flatpak-info; empty for anything else.
std::string flatpakApplication(const std::string &flatpakInfo);

// The parent process, or 0.
int64_t parentProcess(int64_t pid);

} // namespace bayu::shell

#endif // BAYU_SHELL_COMMON_PROCESS_INFO_H_
