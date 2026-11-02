// SPDX-License-Identifier: GPL-3.0-or-later
// android-install: installs an APK into Android (lapis), as opening one in
// the file manager does (#145). A dialog asks first, turns Android on when it
// is not (setting lapis up, as the store does, when it never was), installs,
// then offers to open the app.
//   android-install <file.apk>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>

#include "android_install.h"
#include "android_setup.h"
#include "base/desktop_entry.h"
#include "base/i18n.h"
#include "base/paths.h"
#include "base/process.h"
#include "base/thread_pool.h"
#include "lapis.h"
#include "ui/application.h"
#include "ui/dialog_window.h"
#include "ui/label.h"
#include "ui/layout.h"
#include "ui/progress_bar.h"
#include "ui/style.h"
#include "ui/widgets.h"

using namespace bayu;
using namespace bayu::ui;
using bayu::i18n::format;
using bayu::i18n::tr;
using store::style::Role;

namespace {

// The app's name, from the menu entry lapis writes for it; the package
// before lapis has written it.
std::string appName(const std::string& package)
{
    const std::string path = base::paths::dataHome() + "/applications/lapis." + package + ".desktop";
    std::ifstream in(path);
    std::stringstream contents;
    contents << in.rdbuf();
    const std::optional<base::DesktopEntry> entry =
            base::DesktopEntry::parse("lapis." + package + ".desktop", path, contents.str());
    return entry && !entry->name.empty() ? entry->name : package;
}

struct Result
{
    std::string error;
    std::string package;
};

// On a worker: the copy lapis can read when it needs one, then the install,
// and which package it was.
Result installNow(const std::string& apk)
{
    std::string path = apk;
    if (!store::install::readableByLapis(apk)) {
        path = store::install::stagingPath(apk);
        std::error_code error;
        std::filesystem::create_directories(std::filesystem::path(path).parent_path(), error);
        std::filesystem::copy_file(apk, path, std::filesystem::copy_options::overwrite_existing, error);
        if (error)
            return {format(tr("The APK could not be read: {error}"), {{"error", error.message()}}), {}};
    }
    std::string failure;
    const std::map<std::string, int64_t> before = store::lapis::packages(&failure);
    if (!failure.empty())
        return {failure, {}};
    Result result;
    result.error = store::lapis::install(path);
    if (path != apk) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }
    if (result.error.empty())
        result.package = store::install::changedPackage(before, store::lapis::packages(nullptr));
    return result;
}

class Installer
{
public:
    explicit Installer(std::string apk)
        : m_apk(std::move(apk))
        , m_name(std::filesystem::path(m_apk).filename().string())
        , m_dialog(nullptr, tr("Install on Android"), 420)
    {
        auto content = std::make_unique<Column>();
        content->setSpacing(16);
        content->setMargins({24, 4, 24, 20});
        Row* row = content->addChild<Row>();
        row->setSpacing(14);
        row->setLayoutHints({.fillWidth = true});
        m_spinner = row->addChild<BusyIndicator>();
        m_spinner->setVisible(false);
        Column* texts = row->addChild<Column>();
        texts->setSpacing(4);
        texts->setLayoutHints({.fillWidth = true});
        m_text = store::addText(texts, {}, 14, 500);
        m_text->setWrap(true);
        m_text->setLayoutHints({.fillWidth = true});
        m_detail = store::addText(texts, {}, 13, 400, Role::SecondaryText);
        m_detail->setWrap(true);
        m_detail->setLayoutHints({.fillWidth = true});
        m_progress = content->addChild<ProgressBar>();
        m_progress->setLayoutHints({.fillWidth = true});
        m_progress->setVisible(false);
        Row* actions = content->addChild<Row>();
        actions->setSpacing(8);
        actions->setLayoutHints({.alignment = Alignment::End});
        m_cancel = actions->addChild<DialogButton>(tr("Cancel"));
        m_cancel->onClicked = [this] { m_dialog.close(); };
        m_confirm = actions->addChild<DialogButton>(tr("Install"), true);
        m_confirm->onClicked = [this] { confirmed(); };
        m_dialog.setContentView(std::move(content));
        m_dialog.onClosed = [this] { Application::instance()->quit(m_done ? 0 : 1); };

        m_setup.onChanged = [this] { showSetup(); };
        m_setup.onFinished = [this](const std::string& error) {
            if (!error.empty())
                failed(error);
            else
                install();
        };
    }

    void show()
    {
        m_text->setText(format(tr("Install {name} on Android?"), {{"name", m_name}}));
        m_detail->setText(tr("Install only apps you trust: an app from outside F-Droid can read what you give it."));
        m_dialog.show();
    }

private:
    enum class Step { Ask, Working, Installed, Failed };

    void confirmed()
    {
        if (m_step == Step::Installed) {
            open();
            return;
        }
        if (m_step != Step::Ask)
            return;
        setWorking(format(tr("Installing {name}"), {{"name", m_name}}), {});
        m_job = base::runInThread([] { return store::lapis::state(); }, [this](store::lapis::State state) {
            if (state == store::lapis::State::Running || state == store::lapis::State::Frozen)
                install();
            else
                m_setup.start();
        });
    }

    void showSetup()
    {
        std::string title;
        std::string detail;
        store::describeSetup(m_setup.state(), &title, &detail);
        setWorking(title, detail);
        const double progress = m_setup.state().progress;
        m_progress->setVisible(progress >= 0);
        if (progress >= 0)
            m_progress->setValue(progress);
    }

    void install()
    {
        setWorking(format(tr("Installing {name}"), {{"name", m_name}}), tr("Android checks and installs the app."));
        m_progress->setVisible(false);
        m_job = base::runInThread([apk = m_apk] { return installNow(apk); }, [this](Result result) {
            if (!result.error.empty()) {
                failed(result.error);
                return;
            }
            m_package = result.package;
            m_done = true;
            m_step = Step::Installed;
            m_spinner->setRunning(false);
            m_spinner->setVisible(false);
            m_dialog.setTitle(tr("Installed"));
            const std::string name = m_package.empty() ? m_name : appName(m_package);
            m_text->setText(format(tr("{name} is installed on Android."), {{"name", name}}));
            m_detail->setText(tr("It is in the app menu with your other apps."));
            m_cancel->setText(tr("Close"));
            m_cancel->setVisible(true);
            m_confirm->setText(tr("Open"));
            m_confirm->setVisible(!m_package.empty());
            m_confirm->setEnabled(true);
        });
    }

    void open()
    {
        // As the app menu's lapis entries do; it outlives this dialog.
        base::Process::startDetached("lapis", {"app", "launch", m_package});
        m_dialog.close();
    }

    void setWorking(const std::string& text, const std::string& detail)
    {
        m_step = Step::Working;
        m_spinner->setVisible(true);
        m_spinner->setRunning(true);
        m_text->setText(text);
        m_detail->setText(detail);
        m_detail->setColor(store::style::color(Role::SecondaryText));
        m_confirm->setEnabled(false);
        // Turning Android on goes on without the dialog; installing does not stop.
        m_cancel->setVisible(false);
    }

    void failed(const std::string& error)
    {
        m_step = Step::Failed;
        m_dialog.setTitle(tr("Not installed"));
        m_spinner->setRunning(false);
        m_spinner->setVisible(false);
        m_progress->setVisible(false);
        m_text->setText(format(tr("{name} could not be installed."), {{"name", m_name}}));
        m_detail->setText(error);
        m_detail->setColor(store::style::color(Role::Danger));
        m_confirm->setVisible(false);
        m_cancel->setText(tr("Close"));
        m_cancel->setVisible(true);
    }

    std::string m_apk;
    std::string m_name;
    std::string m_package;
    Step m_step = Step::Ask;
    bool m_done = false;
    DialogWindow m_dialog;
    BusyIndicator* m_spinner;
    Label* m_text;
    Label* m_detail;
    ProgressBar* m_progress;
    DialogButton* m_cancel;
    DialogButton* m_confirm;
    store::AndroidSetupRun m_setup;
    base::Job m_job;
};

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2 || argv[1][0] == '-') {
        fprintf(stderr, "Usage: %s <file.apk>\nInstalls an APK into Android (lapis), asking first.\n", argv[0]);
        return 2;
    }
    Application application(argc, argv);
    application.setApplicationId("bayu-store");
    bayu::i18n::setDomain("bayu-store");
    store::style::install();
    Installer installer(argv[1]);
    installer.show();
    return application.run();
}
