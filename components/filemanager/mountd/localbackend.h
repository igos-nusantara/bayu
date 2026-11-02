// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "backend.h"

#include <map>
#include <string>

// The local file system as this process sees it, under a root folder: what
// bayu-admin-mount serves as root, so the file manager reaches every file.
class LocalBackend : public Backend
{
public:
    explicit LocalBackend(std::string root = "/");
    ~LocalBackend() override;

    ConnectResult connect(const Credentials &credentials) override;

    int stat(const std::string &path, struct stat *st) override;
    int list(const std::string &path, std::vector<DirectoryEntry> *entries) override;
    int open(const std::string &path, int flags, mode_t mode, uint64_t *handle) override;
    int read(uint64_t handle, char *buffer, size_t size, off_t offset) override;
    int write(uint64_t handle, const char *buffer, size_t size, off_t offset) override;
    int flush(uint64_t handle) override;
    int release(uint64_t handle) override;
    int truncate(const std::string &path, off_t size, const uint64_t *handle) override;
    int remove(const std::string &path) override;
    int makeDirectory(const std::string &path, mode_t mode) override;
    int removeDirectory(const std::string &path) override;
    int rename(const std::string &from, const std::string &to, unsigned flags) override;
    int setMode(const std::string &path, mode_t mode) override;
    int setOwner(const std::string &path, uid_t uid, gid_t gid) override;
    int setTimes(const std::string &path, const struct timespec times[2]) override;
    int readLink(const std::string &path, std::string *target) override;
    int makeLink(const std::string &target, const std::string &path) override;
    int statfs(const std::string &path, struct statvfs *st) override;

private:
    std::string full(const std::string &path) const;

    std::string m_root;
};
