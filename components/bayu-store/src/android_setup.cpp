// SPDX-License-Identifier: GPL-3.0-or-later

#include "android_setup.h"

#include <cctype>
#include <optional>

#include "base/i18n.h"
#include "base/process.h"
#include "lapis.h"
#include "ui/style.h"

using bayu::i18n::tr;

namespace store {

void describeSetup(const AndroidSetup& setup, std::string* title, std::string* detail)
{
    using bayu::i18n::format;
    switch (setup.stage) {
    case AndroidSetup::Stage::Authorizing:
        *title = tr("Waiting for your password");
        *detail = tr("Setting Android up needs administrator rights, once.");
        break;
    case AndroidSetup::Stage::Downloading:
        *title = setup.image == "vendor" ? tr("Downloading Android's vendor image") : tr("Downloading Android");
        *detail = setup.bytesTotal ? format(tr("{done} of {total}"), {{"done", style::formatSize(setup.bytesDone)},
                                                                     {"total", style::formatSize(setup.bytesTotal)}})
                                   : style::formatSize(setup.bytesDone);
        if (setup.progress >= 0)
            *detail += " · " + std::to_string(int(setup.progress * 100)) + "%";
        break;
    case AndroidSetup::Stage::Extracting:
        *title = tr("Unpacking Android");
        *detail = tr("Almost there.");
        break;
    case AndroidSetup::Stage::Starting:
        *title = tr("Starting Android");
        *detail = tr("The first start takes a minute or two.");
        break;
    default:
        *title = tr("Getting Android ready");
        *detail = tr("Android is downloaded once, about 1.3 GB. You can keep using the store meanwhile.");
        break;
    }
}

namespace {

constexpr uint64_t kMiB = uint64_t(1) << 20;

std::string_view trimmed(std::string_view text)
{
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
        text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
        text.remove_suffix(1);
    return text;
}

// The number at the start of `text`, moving past it; nullopt when none.
std::optional<uint64_t> number(std::string_view& text)
{
    size_t end = 0;
    uint64_t value = 0;
    while (end < text.size() && std::isdigit(static_cast<unsigned char>(text[end])))
        value = value * 10 + uint64_t(text[end++] - '0');
    if (end == 0)
        return std::nullopt;
    text.remove_prefix(end);
    return value;
}

bool consume(std::string_view& text, std::string_view prefix)
{
    if (!text.starts_with(prefix))
        return false;
    text.remove_prefix(prefix.size());
    return true;
}

} // namespace

void readSetupLine(std::string_view line, AndroidSetup& state)
{
    std::string_view text = trimmed(line);
    if (text.empty())
        return;
    // Anything from lapis: the password was given.
    if (state.stage == AndroidSetup::Stage::Authorizing)
        state.stage = AndroidSetup::Stage::Preparing;

    if (consume(text, "Downloading ")) {
        // "lineage-20.0-…-VANILLA-x86_64-system.zip (1003 MiB)"
        const size_t open = text.rfind('(');
        const std::string_view file = text.substr(0, open);
        state.stage = AndroidSetup::Stage::Downloading;
        state.image = file.find("vendor") != std::string_view::npos ? "vendor" : "system";
        state.bytesDone = 0;
        state.bytesTotal = 0;
        state.progress = 0;
        if (open != std::string_view::npos) {
            std::string_view size = text.substr(open + 1);
            if (const std::optional<uint64_t> mib = number(size))
                state.bytesTotal = *mib * kMiB;
        }
        return;
    }
    if (consume(text, "Resuming at ")) {
        if (const std::optional<uint64_t> mib = number(text))
            state.bytesDone = *mib * kMiB;
        return;
    }
    if (consume(text, "Extracting ")) {
        // "system.img"
        state.stage = AndroidSetup::Stage::Extracting;
        state.image = std::string(text.substr(0, text.find('.')));
        state.progress = -1;
        return;
    }
    if (state.stage != AndroidSetup::Stage::Downloading)
        return;
    // "620 / 1003 MiB (61%)", "620 MiB" without a size, "1003 MiB done".
    const std::optional<uint64_t> done = number(text);
    if (!done)
        return;
    state.bytesDone = *done * kMiB;
    if (consume(text, " / ")) {
        if (const std::optional<uint64_t> total = number(text))
            state.bytesTotal = *total * kMiB;
    }
    if (trimmed(text).ends_with("done"))
        state.bytesDone = state.bytesTotal ? state.bytesTotal : state.bytesDone;
    state.progress = state.bytesTotal ? double(state.bytesDone) / double(state.bytesTotal) : -1;
}

AndroidSetupRun::AndroidSetupRun() = default;

AndroidSetupRun::~AndroidSetupRun()
{
    // android-setup stops at the end of its input; ~Process then waits for it.
    if (m_process)
        m_process->closeStandardInput();
}

void AndroidSetupRun::start()
{
    if (m_state.busy())
        return;
    m_state = {};
    setStage(AndroidSetup::Stage::Preparing);
    m_job = bayu::base::runInThread([] { return lapis::state(); }, [this](lapis::State state) {
        if (state != lapis::State::Missing) {
            startSession();
            return;
        }
        m_line.clear();
        m_errors.clear();
        m_process = std::make_unique<bayu::base::Process>();
        m_process->onStandardOutput = [this](std::string_view data) { readOutput(data); };
        m_process->onStandardError = [this](std::string_view data) { m_errors.append(data); };
        m_process->onFinished = [this](int exitCode, bool crashed) { setupFinished(exitCode, crashed); };
        // pkexec goes on as root, where this process cannot kill it: an
        // input left open, and closed with this process, stops it.
        m_process->setStandardInputOpen(true);
        if (!m_process->start("pkexec", {"/usr/libexec/bayu-store/android-setup"})) {
            finish(tr("Android could not be set up: pkexec is missing."));
            return;
        }
        setStage(AndroidSetup::Stage::Authorizing);
    });
}

void AndroidSetupRun::setStage(AndroidSetup::Stage stage)
{
    m_state.stage = stage;
    if (stage != AndroidSetup::Stage::Downloading)
        m_state.progress = -1;
    if (onChanged)
        onChanged();
}

void AndroidSetupRun::readOutput(std::string_view data)
{
    m_line.append(data);
    size_t end;
    bool read = false;
    while ((end = m_line.find('\n')) != std::string::npos) {
        readSetupLine(std::string_view(m_line).substr(0, end), m_state);
        m_line.erase(0, end + 1);
        read = true;
    }
    if (read && onChanged)
        onChanged();
}

void AndroidSetupRun::setupFinished(int exitCode, bool crashed)
{
    m_process.reset();
    if (crashed || exitCode != 0) {
        // pkexec: 126 when the prompt was closed, 127 when not authorised.
        if (!crashed && (exitCode == 126 || exitCode == 127)) {
            finish(tr("Android was not set up: the password was not given."));
            return;
        }
        // lapis says why on its last line.
        std::string_view errors = trimmed(m_errors);
        const size_t last = errors.rfind('\n');
        const std::string why(last == std::string_view::npos ? errors : trimmed(errors.substr(last + 1)));
        finish(why.empty() ? std::string(tr("Android could not be set up (lapis init failed).")) : why);
        return;
    }
    startSession();
}

void AndroidSetupRun::startSession()
{
    setStage(AndroidSetup::Stage::Starting);
    m_job = bayu::base::runInThread([] { return lapis::startSession(std::chrono::minutes(3)); },
                                    [this](std::string error) { finish(error); });
}

void AndroidSetupRun::finish(const std::string& error)
{
    m_state.error = error;
    setStage(error.empty() ? AndroidSetup::Stage::Idle : AndroidSetup::Stage::Failed);
    if (onFinished)
        onFinished(error);
}

} // namespace store
