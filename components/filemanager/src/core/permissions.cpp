// SPDX-License-Identifier: BSD-3-Clause

#include "core/permissions.h"

#include <dirent.h>
#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>

#include <algorithm>
#include <cerrno>

namespace fm::permissions {

static int shiftOf(Who who)
{
    switch (who) {
    case Who::Owner:
        return 6;
    case Who::Group:
        return 3;
    case Who::Others:
        break;
    }
    return 0;
}

Access access(mode_t mode, Who who)
{
    const int shift = shiftOf(who);
    if (mode & (S_IWOTH << shift))
        return Access::ReadWrite;
    return mode & (S_IROTH << shift) ? Access::Read : Access::None;
}

mode_t withAccess(mode_t mode, Who who, Access access, bool directory)
{
    const int shift = shiftOf(who);
    const mode_t read = S_IROTH << shift;
    const mode_t write = S_IWOTH << shift;
    const mode_t enter = S_IXOTH << shift;
    mode &= ~(read | write | (directory ? enter : 0));
    if (access != Access::None)
        mode |= read | (directory ? enter : 0);
    if (access == Access::ReadWrite)
        mode |= write;
    return mode;
}

bool isExecutable(mode_t mode)
{
    return mode & (S_IXUSR | S_IXGRP | S_IXOTH);
}

mode_t withExecutable(mode_t mode, bool executable)
{
    mode &= ~mode_t(S_IXUSR | S_IXGRP | S_IXOTH);
    if (executable)
        mode |= (mode & (S_IRUSR | S_IRGRP | S_IROTH)) >> 2;
    return mode;
}

static mode_t withAll(mode_t mode, const std::array<Access, 3>& access, bool directory)
{
    mode = withAccess(mode, Who::Owner, access[0], directory);
    mode = withAccess(mode, Who::Group, access[1], directory);
    return withAccess(mode, Who::Others, access[2], directory);
}

int applyToContents(const std::string& path, const std::array<Access, 3>& access)
{
    DIR* directory = ::opendir(path.c_str());
    if (!directory)
        return errno;
    int failure = 0;
    while (dirent* entry = ::readdir(directory)) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..")
            continue;
        const std::string child = path + '/' + name;
        struct stat st;
        if (::lstat(child.c_str(), &st) != 0 || S_ISLNK(st.st_mode))
            continue;
        const bool folder = S_ISDIR(st.st_mode);
        if (::chmod(child.c_str(), withAll(st.st_mode & 07777, access, folder)) != 0 && !failure)
            failure = errno;
        if (folder) {
            const int inner = applyToContents(child, access);
            if (inner && !failure)
                failure = inner;
        }
    }
    ::closedir(directory);
    return failure;
}

std::string userName(uid_t uid)
{
    const passwd* entry = ::getpwuid(uid);
    return entry ? entry->pw_name : std::to_string(uid);
}

std::string groupName(gid_t gid)
{
    const group* entry = ::getgrgid(gid);
    return entry ? entry->gr_name : std::to_string(gid);
}

std::vector<std::pair<uid_t, std::string>> users()
{
    std::vector<std::pair<uid_t, std::string>> out;
    ::setpwent();
    while (const passwd* entry = ::getpwent()) {
        if (entry->pw_uid == 0 || (entry->pw_uid >= 1000 && entry->pw_uid < 60000))
            out.emplace_back(entry->pw_uid, entry->pw_name);
    }
    ::endpwent();
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
    return out;
}

std::vector<std::pair<gid_t, std::string>> groups()
{
    std::vector<std::pair<gid_t, std::string>> out;
    ::setgrent();
    while (const group* entry = ::getgrent())
        out.emplace_back(entry->gr_gid, entry->gr_name);
    ::endgrent();
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
    return out;
}

} // namespace fm::permissions
