// SPDX-License-Identifier: BSD-3-Clause

#include <algorithm>
#include <spawn.h>
#include <memory>
#include <string>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

#include "base/dbus.h"
#include "base/event_loop.h"
#include "base/i18n.h"
#include "bayu/packagekit/packagekit.h"
#include "ui/cell_layout.h"
#include "ui/icon.h"
#include "ui/label.h"
#include "ui/progress_bar.h"
#include "ui/theme.h"
#include "widgets/card.h"
#include "widgets/listrow.h"
#include "widgets/metrics.h"
#include "widgets/sections.h"
#include "widgets/standardbutton.h"
#include "window/page.h"
#include "window/pagelist.h"
#include "window/settingswindow.h"

using namespace bayu::ui;
namespace dbus = bayu::base::dbus;
namespace packagekit = bayu::packagekit;
using bayu::i18n::format;
using bayu::i18n::tr;
using bayu::i18n::trn;

namespace settings {

namespace {

// The shell's update monitor, which tells the bar, the notifications and the
// sidebar's count.
dbus::Proxy updatesService()
{
    return dbus::Proxy(dbus::Connection::session(), "org.bayu.Updates", "/Updates", "org.bayu.Updates");
}

// At most this many packages are listed; the rest are counted.
constexpr size_t kListed = 60;

class UpdatesPage : public Page
{
public:
    explicit UpdatesPage(SettingsWindow& window)
        : Page(window, tr("System Update"))
        , m_service(updatesService())
    {
        // The status: an icon, what is going on, and what can be done.
        Card* status = content()->addChild<Card>();
        RowLayout* row = status->addChild<RowLayout>();
        row->setSpacing(metrics::l);
        m_icon = row->addChild<ThemeIcon>("system-software-update", 48);
        m_icon->setLayoutHints({.preferredWidth = 48, .preferredHeight = 48, .verticalAlignment = Alignment::Start});
        ColumnLayout* text = row->addChild<ColumnLayout>();
        text->setSpacing(metrics::xs);
        text->setLayoutHints({.fillWidth = true});
        m_title = text->addChild<Label>();
        m_title->setLayoutHints({.fillWidth = true});
        m_title->setFontSize(metrics::points(13));
        m_title->setWeight(600);
        m_detail = text->addChild<Label>();
        m_detail->setWrap(true);
        m_detail->setLayoutHints({.fillWidth = true});
        m_progress = text->addChild<ProgressBar>();
        m_progress->setLayoutHints({.fillWidth = true});
        m_progressText = text->addChild<Label>();
        m_progressText->setElide(Label::Elide::Middle);
        m_action = text->addChild<StandardButton>();
        LayoutHints actionHints {.horizontalAlignment = Alignment::Start};
        actionHints.margins.top = metrics::s;
        m_action->setLayoutHints(actionHints);
        m_action->onClicked = [this] { act(); };

        m_listSection = content()->addChild<Section>(tr("Updates"));
        m_list = m_listSection->addChild<Card>(Card::Style::List);

        Section* apps = content()->addChild<Section>(tr("Applications"));
        Card* appsCard = apps->addChild<Card>(Card::Style::List);
        m_apps = appsCard->addChild<NavigationItem>(tr("Application updates"));
        m_apps->onClicked = [] {
            // The Store's own page of updates.
            // Through a shell that leaves it running and ends at once, so
            // nothing is left to reap.
            pid_t pid = 0;
            char shell[] = "/bin/sh";
            char flag[] = "-c";
            char command[] = "bayu-store --updates >/dev/null 2>&1 &";
            char* arguments[] = {shell, flag, command, nullptr};
            if (posix_spawn(&pid, shell, nullptr, nullptr, arguments, environ) == 0)
                waitpid(pid, nullptr, 0);
        };

        Card* automatic = content()->addChild<Card>(Card::Style::List);
        m_automatic = automatic->addChild<SwitchRow>(tr("Download updates automatically"));
        m_automatic->setName("updates:automatic");
        m_automatic->onToggled = [this](bool checked) { m_service.send("SetAutoDownload", checked); };
        content()->addChild<Footnote>(
                tr("Updates are downloaded in the background and installed when you restart. You are always asked "
                   "before they are installed."));

        m_changed = m_service.onSignal("Changed", [this](const dbus::Message&) { readService(); });
        readService();
        list(false);
    }

    ~UpdatesPage() override
    {
        *m_alive = false;
        m_done.reset();
        m_progressChanged.reset();
    }

private:
    enum class State { Listing, UpToDate, Available, Downloading, Ready, Failed };

    void readService()
    {
        m_read = m_service.getAllAsync([this](const dbus::Reply& reply) {
            const auto properties = reply.value<dbus::VariantMap>();
            if (!properties)
                return;
            if (const auto it = properties->find("AppUpdates"); it != properties->end())
                m_appUpdates = int(it->second.value<uint32_t>());
            if (const auto it = properties->find("AutoDownload"); it != properties->end())
                m_automatic->setChecked(it->second.value<bool>());
            m_apps->setValue(m_appUpdates > 0 ? std::to_string(m_appUpdates) : tr("Up to date"));
        });
    }

    // Lists the updates; `refresh` reads the repositories again first.
    void list(bool refresh)
    {
        setState(State::Listing);
        m_transaction = std::make_unique<packagekit::Transaction>();
        if (refresh) {
            m_done = m_transaction->finished.connect([this](bool, const std::string&) {
                post([this] { list(false); });
            });
            // Asked for: every repository read again, as dnf check-update does,
            // not the metadata PackageKit still thinks fresh (issue #123).
            m_transaction->refreshCache(false, true);
            return;
        }
        m_done = m_transaction->finished.connect([this](bool success, const std::string& error) {
            if (!success) {
                m_error = error;
                setState(State::Failed);
                return;
            }
            m_packages = m_transaction->packages();
            std::stable_sort(m_packages.begin(), m_packages.end(), [](const auto& a, const auto& b) {
                return a.isSecurity() > b.isSecurity() || (a.isSecurity() == b.isSecurity() && a.name() < b.name());
            });
            fillList();
            packagekit::offline::prepared([this, alive = std::weak_ptr<bool>(m_alive)](bool prepared) {
                if (!alive.lock())
                    return;
                setState(m_packages.empty() ? State::UpToDate : prepared ? State::Ready : State::Available);
            });
        });
        m_transaction->getUpdates(false);
    }

    void download()
    {
        std::vector<std::string> ids;
        for (const packagekit::Package& package : m_packages)
            ids.push_back(package.id);
        setState(State::Downloading);
        m_transaction = std::make_unique<packagekit::Transaction>();
        m_progressChanged = m_transaction->progressChanged.connect([this] { showProgress(); });
        m_done = m_transaction->finished.connect([this](bool success, const std::string& error) {
            if (!success) {
                // Cancelled: back to what is waiting.
                m_error = error;
                setState(error.empty() ? State::Available : State::Failed);
                return;
            }
            setState(State::Ready);
            m_service.send("ReadPrepared");
        });
        m_transaction->downloadUpdates(ids, false);
    }

    void install()
    {
        packagekit::offline::trigger([this, alive = std::weak_ptr<bool>(m_alive)](bool armed, const std::string& error) {
            if (!alive.lock())
                return;
            if (!armed) {
                m_error = error;
                setState(State::Failed);
                return;
            }
            // The session asks first, as the user menu's Restart does.
            dbus::Proxy(dbus::Connection::session(), "org.bayu.Statusbar", "/Statusbar", "org.bayu.Statusbar")
                    .send("requestReboot");
        });
    }

    void act()
    {
        switch (m_state) {
        case State::UpToDate:
        case State::Failed:
            list(true);
            m_service.send("Check");
            break;
        case State::Available:
            download();
            break;
        case State::Downloading:
            if (m_transaction)
                m_transaction->cancel();
            break;
        case State::Ready:
            install();
            break;
        case State::Listing:
            break;
        }
    }

    void fillList()
    {
        while (!m_list->children().empty())
            m_list->takeChild(m_list->children().back().get());
        size_t shown = 0;
        for (const packagekit::Package& package : m_packages) {
            if (shown++ == kListed)
                break;
            const std::string name = package.isSecurity()
                    ? format(tr("{name} (security)"), {{"name", package.name()}})
                    : package.name();
            m_list->addChild<InfoRow>(name, package.version());
        }
        if (m_packages.size() > kListed)
            m_list->addChild<InfoRow>(
                    trn("And {n} more", "And {n} more", uint64_t(m_packages.size() - kListed)), std::string());
        m_listSection->setVisible(!m_packages.empty());
    }

    void showProgress()
    {
        if (!m_transaction)
            return;
        const int percentage = m_transaction->percentage();
        m_progress->setIndeterminate(percentage < 0);
        if (percentage >= 0)
            m_progress->setValue(percentage / 100.0);
        const std::string package = packagekit::Package {m_transaction->currentPackage(), "", 0}.name();
        m_progressText->setText(package.empty()
                                        ? tr("Preparing…")
                                        : format(tr("Downloading {package}…"), {{"package", package}}));
    }

    void setState(State state)
    {
        m_state = state;
        const int count = int(m_packages.size());
        const int security = int(std::count_if(m_packages.begin(), m_packages.end(),
                                               [](const auto& package) { return package.isSecurity(); }));
        const bool downloading = state == State::Downloading;
        m_progress->setVisible(downloading);
        m_progressText->setVisible(downloading);
        m_action->setVisible(state != State::Listing);
        m_action->setBackgroundColor(std::nullopt);
        switch (state) {
        case State::Listing:
            m_title->setText(tr("Checking for updates…"));
            m_detail->setText(std::string());
            break;
        case State::UpToDate:
            m_title->setText(tr("Your system is up to date"));
            m_detail->setText(tr("There are no updates for the system."));
            m_action->setText(tr("Check for Updates"));
            break;
        case State::Available:
            m_title->setText(trn("{n} update available", "{n} updates available", uint64_t(count)));
            m_detail->setText(security > 0 ? trn("Including {n} security update.", "Including {n} security updates.",
                                                 uint64_t(security))
                                           : tr("They are downloaded now and installed when you restart."));
            m_action->setText(tr("Download"));
            m_action->setBackgroundColor(Theme::current().highlight);
            break;
        case State::Downloading:
            m_title->setText(tr("Downloading updates…"));
            m_detail->setText(trn("{n} update", "{n} updates", uint64_t(count)));
            m_action->setText(tr("Cancel"));
            showProgress();
            break;
        case State::Ready:
            m_title->setText(tr("Updates ready to install"));
            m_detail->setText(tr("Restart to install the downloaded updates. Save your work first."));
            m_action->setText(tr("Restart and Install"));
            m_action->setBackgroundColor(Theme::current().highlight);
            break;
        case State::Failed:
            m_title->setText(tr("Could not check for updates"));
            m_detail->setText(m_error.empty() ? tr("Check your internet connection and try again.") : m_error);
            m_action->setText(tr("Try Again"));
            break;
        }
        requestLayout();
    }

    template<typename F>
    void post(F f)
    {
        // Out of the transaction's own signal, which the new one replaces.
        bayu::base::EventLoop::current()->post([f = std::move(f), alive = std::weak_ptr<bool>(m_alive)] {
            if (alive.lock())
                f();
        });
    }

    dbus::Proxy m_service;
    dbus::Match m_changed;
    dbus::PendingCall m_read;
    std::unique_ptr<packagekit::Transaction> m_transaction;
    bayu::base::Subscription m_done;
    bayu::base::Subscription m_progressChanged;
    std::vector<packagekit::Package> m_packages;
    State m_state = State::Listing;
    std::string m_error;
    int m_appUpdates = 0;
    ThemeIcon* m_icon = nullptr;
    Label* m_title = nullptr;
    Label* m_detail = nullptr;
    ProgressBar* m_progress = nullptr;
    Label* m_progressText = nullptr;
    StandardButton* m_action = nullptr;
    Section* m_listSection = nullptr;
    Card* m_list = nullptr;
    NavigationItem* m_apps = nullptr;
    SwitchRow* m_automatic = nullptr;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};

} // namespace

// Always there; it keeps the sidebar's count of the system's updates.
class UpdatesBadge : public AvailabilityWatch
{
public:
    UpdatesBadge()
        : m_service(updatesService())
    {
        m_changed = m_service.onSignal("Changed", [this](const dbus::Message&) { read(); });
        read();
    }

private:
    void read()
    {
        m_read = m_service.getPropertyAsync("SystemUpdates", [](const dbus::Reply& reply) {
            const auto value = reply.value<dbus::Variant>();
            setPageBadge("updates", value ? int(value->value<uint32_t>()) : 0);
        });
    }

    dbus::Proxy m_service;
    dbus::Match m_changed;
    dbus::PendingCall m_read;
};

static const PageRegistration registration(
        "updates", [](SettingsWindow& window) -> std::unique_ptr<Page> { return std::make_unique<UpdatesPage>(window); },
        [](SettingsWindow&, AvailabilityCallback callback) -> std::unique_ptr<AvailabilityWatch> {
            callback(true);
            return std::make_unique<UpdatesBadge>();
        });

} // namespace settings
