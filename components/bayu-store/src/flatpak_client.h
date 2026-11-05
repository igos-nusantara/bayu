// SPDX-License-Identifier: GPL-3.0-or-later
// Flathub apps through libflatpak, in the system installation (polkit lets
// administrators install without a password). Blocking: call from a worker
// thread.

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace store::flatpak {

struct Installed
{
    std::string id;
    std::string version;
    uint64_t size = 0;
    int64_t installed = 0;
};

struct Update
{
    std::string id;
    uint64_t downloadSize = 0;
};

struct Usage
{
    uint64_t apps = 0;
    uint64_t runtimes = 0;
};

// Adds Flathub when the installation lacks it.
// Adds a system remote from its .flatpakrepo file when it is missing.
std::string ensureRemote(const char* name, const char* repoFile);
std::string ensureFlathub();
std::vector<Installed> installed(std::string* error);
std::vector<Update> updates(std::string* error);
Usage usage();

// done bytes, total bytes (0 unknown), percent; returns false to stop.
using Progress = std::function<void(uint64_t done, uint64_t total, int percent)>;

enum class Operation {
    Install,
    Update,
    Remove,
};
// An error text, "stopped" when cancelled, empty on success.
// Installs from `remote`, Flathub when empty; updates and removals find theirs.
std::string run(Operation operation, const std::string& id, const std::string& remote, const Progress& progress,
                std::atomic<bool>& cancel);
std::string launch(const std::string& id);

} // namespace store::flatpak
