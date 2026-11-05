// SPDX-License-Identifier: BSD-3-Clause

#include "localbackend.h"

#include <cerrno>
#include <cstdio>
#include <dirent.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>

// Every call returns 0, or a count, and -errno on failure.
static int result(long value)
{
    return value < 0 ? -errno : int(value);
}

LocalBackend::LocalBackend(std::string root)
    : m_root(root == "/" ? std::string() : std::move(root))
{
}

LocalBackend::~LocalBackend() = default;

ConnectResult LocalBackend::connect(const Credentials &)
{
    ConnectResult connected;
    connected.status = ConnectResult::Connected;
    return connected;
}

std::string LocalBackend::full(const std::string &path) const
{
    return m_root + (path.empty() ? "/" : path);
}

int LocalBackend::stat(const std::string &path, struct stat *st)
{
    return result(::lstat(full(path).c_str(), st));
}

int LocalBackend::list(const std::string &path, std::vector<DirectoryEntry> *entries)
{
    DIR *dir = ::opendir(full(path).c_str());
    if (!dir)
        return -errno;
    const int fd = ::dirfd(dir);
    while (const struct dirent *entry = ::readdir(dir)) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..")
            continue;
        DirectoryEntry item;
        item.name = name;
        ::fstatat(fd, entry->d_name, &item.st, AT_SYMLINK_NOFOLLOW);
        entries->push_back(std::move(item));
    }
    ::closedir(dir);
    return 0;
}

int LocalBackend::open(const std::string &path, int flags, mode_t mode, uint64_t *handle)
{
    const int fd = ::open(full(path).c_str(), flags, mode);
    if (fd < 0)
        return -errno;
    *handle = uint64_t(fd);
    return 0;
}

int LocalBackend::read(uint64_t handle, char *buffer, size_t size, off_t offset)
{
    return result(::pread(int(handle), buffer, size, offset));
}

int LocalBackend::write(uint64_t handle, const char *buffer, size_t size, off_t offset)
{
    return result(::pwrite(int(handle), buffer, size, offset));
}

int LocalBackend::flush(uint64_t handle)
{
    return result(::fsync(int(handle)));
}

int LocalBackend::release(uint64_t handle)
{
    return result(::close(int(handle)));
}

int LocalBackend::truncate(const std::string &path, off_t size, const uint64_t *handle)
{
    return handle ? result(::ftruncate(int(*handle), size)) : result(::truncate(full(path).c_str(), size));
}

int LocalBackend::remove(const std::string &path)
{
    return result(::unlink(full(path).c_str()));
}

int LocalBackend::makeDirectory(const std::string &path, mode_t mode)
{
    return result(::mkdir(full(path).c_str(), mode));
}

int LocalBackend::removeDirectory(const std::string &path)
{
    return result(::rmdir(full(path).c_str()));
}

int LocalBackend::rename(const std::string &from, const std::string &to, unsigned flags)
{
    return result(::syscall(SYS_renameat2, AT_FDCWD, full(from).c_str(), AT_FDCWD, full(to).c_str(), flags));
}

int LocalBackend::setMode(const std::string &path, mode_t mode)
{
    return result(::chmod(full(path).c_str(), mode));
}

int LocalBackend::setOwner(const std::string &path, uid_t uid, gid_t gid)
{
    return result(::lchown(full(path).c_str(), uid, gid));
}

int LocalBackend::setTimes(const std::string &path, const struct timespec times[2])
{
    return result(::utimensat(AT_FDCWD, full(path).c_str(), times, AT_SYMLINK_NOFOLLOW));
}

int LocalBackend::readLink(const std::string &path, std::string *target)
{
    char buffer[4096];
    const ssize_t length = ::readlink(full(path).c_str(), buffer, sizeof buffer);
    if (length < 0)
        return -errno;
    target->assign(buffer, size_t(length));
    return 0;
}

int LocalBackend::makeLink(const std::string &target, const std::string &path)
{
    return result(::symlink(target.c_str(), full(path).c_str()));
}

int LocalBackend::statfs(const std::string &path, struct statvfs *st)
{
    return result(::statvfs(full(path).c_str(), st));
}
