// SPDX-License-Identifier: GPL-3.0-or-later
// Turning Android apps on, as the store and the welcome do: android-setup as
// root through pkexec (lapis' control service, and Android's images
// downloaded once), then Android for this session. What `lapis init` prints
// tells how the download goes.

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "base/thread_pool.h"
#include "model.h"

namespace bayu::base {
class Process;
}

namespace store {

// What a setup that is busy does, in words: "Downloading Android",
// "620 MB of 1 GB · 61%".
void describeSetup(const AndroidSetup& setup, std::string* title, std::string* detail);

// One line of android-setup's output into `state`: "Downloading
// lineage-…-system.zip (1003 MiB)", "  620 / 1003 MiB (61%)", "Resuming at
// 120 MiB", "Extracting system.img". Others leave it as it is.
void readSetupLine(std::string_view line, AndroidSetup& state);

class AndroidSetupRun
{
public:
    AndroidSetupRun();
    // Closing stops the download, which lapis resumes the next time.
    ~AndroidSetupRun();

    AndroidSetupRun(const AndroidSetupRun&) = delete;
    AndroidSetupRun& operator=(const AndroidSetupRun&) = delete;

    // Sets lapis up when it is not, then starts Android. Nothing while it
    // runs already.
    void start();
    const AndroidSetup& state() const { return m_state; }
    bool isRunning() const { return m_state.busy(); }

    std::function<void()> onChanged;
    // With an error text when Android does not run.
    std::function<void(const std::string& error)> onFinished;

private:
    void setStage(AndroidSetup::Stage stage);
    void readOutput(std::string_view data);
    void setupFinished(int exitCode, bool crashed);
    void startSession();
    void finish(const std::string& error);

    AndroidSetup m_state;
    std::unique_ptr<bayu::base::Process> m_process;
    std::string m_line;
    std::string m_errors;
    bayu::base::Job m_job;
};

} // namespace store
