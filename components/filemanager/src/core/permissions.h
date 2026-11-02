// SPDX-License-Identifier: BSD-3-Clause
// Files' permissions as the Properties dialog shows them (#146): who may read
// and write, whether a file runs as a program, its owner and group.

#pragma once

#include <sys/types.h>

#include <array>
#include <string>
#include <utility>
#include <vector>

namespace fm::permissions {

enum class Access {
    None,
    Read,
    ReadWrite,
};

enum class Who {
    Owner,
    Group,
    Others,
};

// The access `who` has: its read and write bits.
Access access(mode_t mode, Who who);
// `mode` with `who`'s access set. On a folder, reading comes with entering it
// (x), as listing is no use without; a file's x stays as it was.
mode_t withAccess(mode_t mode, Who who, Access access, bool directory);

// A file runs as a program when anyone may run it.
bool isExecutable(mode_t mode);
// Running for whoever may read it, or for no one.
mode_t withExecutable(mode_t mode, bool executable);

// The same access, for owner, group and others, to everything under the
// folder `path`, folders as folders and files as files, links left alone.
// 0, or the errno of the first change that failed.
int applyToContents(const std::string& path, const std::array<Access, 3>& access);

std::string userName(uid_t uid);
std::string groupName(gid_t gid);
// The ones to give a file to: root and the people's accounts (uid 1000 up),
// and every group.
std::vector<std::pair<uid_t, std::string>> users();
std::vector<std::pair<gid_t, std::string>> groups();

} // namespace fm::permissions
