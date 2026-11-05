// SPDX-License-Identifier: BSD-3-Clause

#include "window/admin.h"

#include <cctype>
#include <cerrno>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <thread>

#include "base/event_loop.h"
#include "base/i18n.h"

namespace settings::admin {

namespace {

constexpr char kHelper[] = SETTINGS_ADMIN_HELPER;

std::optional<std::string> execute(const std::vector<std::string>& arguments)
{
    // pkexec right from this process, not through a shell: polkit remembers
    // a password given for the process that runs pkexec.
    std::vector<std::string> command{"pkexec", kHelper};
    command.insert(command.end(), arguments.begin(), arguments.end());
    std::vector<char*> argv;
    for (std::string& argument : command)
        argv.push_back(argument.data());
    argv.push_back(nullptr);

    int output[2];
    if (pipe2(output, O_CLOEXEC) != 0)
        return std::string("Cannot run pkexec");
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, output[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, output[1], STDERR_FILENO);
    pid_t pid = 0;
    const int spawned = posix_spawnp(&pid, "pkexec", &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(output[1]);
    if (spawned != 0) {
        close(output[0]);
        return std::string("Cannot run pkexec");
    }
    std::string text;
    char buffer[512];
    ssize_t size;
    while ((size = read(output[0], buffer, sizeof buffer)) > 0 || (size < 0 && errno == EINTR)) {
        if (size > 0)
            text.append(buffer, size_t(size));
    }
    close(output[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    if (status == 0)
        return std::nullopt;
    // pkexec: 126, the password dialog was dismissed; 127, not allowed.
    if (WIFEXITED(status) && WEXITSTATUS(status) == 126)
        return std::string();
    if (WIFEXITED(status) && WEXITSTATUS(status) == 127)
        return bayu::i18n::tr("Only an administrator can change this.");
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
        text.pop_back();
    return text.empty() ? std::string("bayu-settings-admin failed") : text;
}

} // namespace

void run(std::vector<std::string> arguments, std::weak_ptr<bool> alive,
         std::function<void(const std::optional<std::string>& error)> done)
{
    std::thread([loop = bayu::base::EventLoop::current()->handle(), arguments = std::move(arguments),
                 alive = std::move(alive), done = std::move(done)] {
        const std::optional<std::string> error = execute(arguments);
        loop.post([alive, done, error] {
            if (const auto page = alive.lock(); page && *page)
                done(error);
        });
    }).detach();
}

} // namespace settings::admin
