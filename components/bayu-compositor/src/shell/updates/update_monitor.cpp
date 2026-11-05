// SPDX-License-Identifier: BSD-3-Clause

#include "shell/updates/update_monitor.h"

#include <cstdio>
#include <utility>

#include "base/i18n.h"
#include "base/logging.h"
#include "base/settings.h"
#include "bayu/packagekit/packagekit.h"
#include "core/main_loop.h"
#include "shell/common/file_thread.h"

namespace bayu::shell {

namespace dbus = base::dbus;
using bayu::i18n::format;
using bayu::i18n::tr;
using bayu::i18n::trn;

static const base::LogCategory cfUpdates("bayu.shell.updates");

static constexpr const char *kNotifications = "org.freedesktop.Notifications";
static constexpr const char *kNotificationsPath = "/org/freedesktop/Notifications";
static constexpr std::chrono::minutes kFirstCheck {1};
static constexpr std::chrono::hours kInterval {6};

// The Flatpak applications with an update: `flatpak remote-ls --updates`, run
// on the file thread as it reaches the remotes.
static int flatpakAppUpdates()
{
    FILE *pipe = popen("flatpak remote-ls --updates --app --columns=application 2>/dev/null", "r");
    if (!pipe)
        return 0;
    int count = 0;
    char line[512];
    while (fgets(line, sizeof(line), pipe)) {
        if (line[0] != '\n' && line[0] != '\0')
            ++count;
    }
    pclose(pipe);
    return count;
}

UpdateMonitor::UpdateMonitor(dbus::Connection *bus, dbus::Connection *systemBus, std::string settingsPath)
    : m_bus(bus)
    , m_systemBus(systemBus)
    , m_settingsPath(std::move(settingsPath))
    , m_notifications(bus, kNotifications, kNotificationsPath, kNotifications)
{
    m_autoDownload = base::Settings(m_settingsPath).boolValue("Updates/AutoDownload", false);
    m_first.setSingleShot(true);
    m_first.setInterval(kFirstCheck);
    m_first.onTimeout = [this] { check(); };
    m_periodic.setInterval(kInterval);
    m_periodic.onTimeout = [this] { check(); };

    if (!m_bus)
        return;
    dbus::MatchRule rule;
    rule.sender = kNotifications;
    rule.path = kNotificationsPath;
    rule.interface = kNotifications;
    rule.member = "ActionInvoked";
    m_matches.push_back(m_bus->subscribe(rule, [this](const dbus::Message &message) {
        uint32_t id = 0;
        std::string action;
        if (!message.read(id, action) || id != m_notification || !m_invoked)
            return;
        const auto invoked = m_invoked;
        invoked(action);
    }));
    rule.member = "NotificationClosed";
    m_matches.push_back(m_bus->subscribe(rule, [this](const dbus::Message &message) {
        uint32_t id = 0;
        uint32_t reason = 0;
        if (message.read(id, reason) && id == m_notification) {
            m_notification = 0;
            m_invoked = nullptr;
        }
    }));
}

UpdateMonitor::~UpdateMonitor()
{
    *m_alive = false;
    if (m_object && m_bus)
        m_bus->releaseName("org.bayu.Updates");
}

bool UpdateMonitor::registerService()
{
    if (m_object || !m_bus)
        return m_object != nullptr;
    auto object = std::make_unique<dbus::Object>(m_bus, "/Updates", "org.bayu.Updates");
    object->addProperty("SystemUpdates", [this] { return uint32_t(systemUpdates()); });
    object->addProperty("SecurityUpdates", [this] { return uint32_t(m_security); });
    object->addProperty("AppUpdates", [this] { return uint32_t(m_apps); });
    object->addProperty("State", [this] {
        switch (m_state) {
        case State::Checking:
            return std::string("checking");
        case State::Downloading:
            return std::string("downloading");
        case State::Ready:
            return std::string("ready");
        case State::Idle:
            break;
        }
        return std::string("idle");
    });
    object->addProperty("AutoDownload", [this] { return m_autoDownload; });
    object->addSignal("Changed", "");
    object->addMethod("Check", [this] { check(); });
    // Settings downloaded the updates: reads whether they wait for the restart.
    object->addMethod("ReadPrepared", [this] { readPrepared(); });
    object->addMethod("SetAutoDownload", [this](bool enabled) { setAutoDownload(enabled); },
                      dbus::ArgumentNames {{"enabled"}, {}});
    if (!object->publish() || !m_bus->requestName("org.bayu.Updates", dbus::Connection::DoNotQueue)) {
        CF_WARNING(cfUpdates) << "Cannot own org.bayu.Updates";
        return false;
    }
    m_object = std::move(object);
    return true;
}

void UpdateMonitor::start()
{
    readPrepared();
    m_first.start();
    m_periodic.start();
}

void UpdateMonitor::check()
{
    if (m_state == State::Checking || m_state == State::Downloading)
        return;
    // Downloaded and waiting for the restart: a refresh of the repositories
    // would make PackageKit drop the prepared update.
    if (m_state == State::Ready) {
        readPrepared();
        return;
    }
    setState(State::Checking);
    m_systemDone = false;
    m_appsDone = false;
    checkSystem();
    checkApps();
}

void UpdateMonitor::checkSystem()
{
    m_transaction = std::make_unique<packagekit::Transaction>(m_systemBus);
    m_transactionDone = m_transaction->finished.connect([this](bool success, const std::string &error) {
        if (!success)
            CF_WARNING(cfUpdates) << "Refreshing the repositories failed:" << error;
        // A stale cache still lists what it knows. Not from inside the
        // transaction's own signal.
        core::mainLoop()->post([this, alive = std::weak_ptr<bool>(m_alive)] {
            if (alive.lock())
                listSystem();
        });
    });
    // Forced: PackageKit otherwise keeps metadata it thinks fresh for days, and
    // updates dnf already lists are missed (issue #123). Only repomd.xml and
    // what changed are downloaded.
    m_transaction->refreshCache(true, true);
}

void UpdateMonitor::listSystem()
{
    m_transaction = std::make_unique<packagekit::Transaction>(m_systemBus);
    m_transactionDone = m_transaction->finished.connect([this](bool success, const std::string &error) {
        if (success) {
            m_system.clear();
            m_security = 0;
            for (const packagekit::Package &package : m_transaction->packages()) {
                m_system.push_back(package.id);
                m_security += package.isSecurity();
            }
        } else {
            CF_WARNING(cfUpdates) << "Listing the system's updates failed:" << error;
        }
        m_systemDone = true;
        checked();
    });
    m_transaction->getUpdates(true);
}

void UpdateMonitor::checkApps()
{
    m_appsJob = runOnFileThread([] { return flatpakAppUpdates(); },
                                [this](int count) {
                                    m_apps = count;
                                    m_appsDone = true;
                                    checked();
                                });
}

void UpdateMonitor::checked()
{
    if (!m_systemDone || !m_appsDone)
        return;
    packagekit::offline::prepared(
            [this, alive = std::weak_ptr<bool>(m_alive)](bool prepared) {
                if (!alive.lock())
                    return;
                setState(prepared && !m_system.empty() ? State::Ready : State::Idle);
                updateBadge();
                publish();

                // Told once: new system updates, or more applications' than before.
                bool fresh = m_apps > m_toldApps;
                for (const std::string &id : m_system)
                    fresh = fresh || !m_told.count(id);
                m_told = std::set<std::string>(m_system.begin(), m_system.end());
                m_toldApps = m_apps;
                if (m_autoDownload && !m_system.empty() && m_state == State::Idle) {
                    download();
                    return;
                }
                if (!fresh || total() == 0 || m_state == State::Ready)
                    return;
                std::string body;
                if (!m_system.empty() && m_apps > 0)
                    body = format(tr("{system} system updates and {apps} application updates are available."),
                                  {{"system", std::to_string(m_system.size())}, {"apps", std::to_string(m_apps)}});
                else if (!m_system.empty())
                    body = trn("{n} system update is available.", "{n} system updates are available.",
                               m_system.size());
                else
                    body = trn("{n} application update is available.", "{n} application updates are available.",
                               uint64_t(m_apps));
                const bool system = !m_system.empty();
                notify(tr("Updates available"), body, {"default", "", "view", tr("View")},
                       [this, system](const std::string &) {
                           if (system && openSystemUpdates)
                               openSystemUpdates();
                           else if (!system && openAppUpdates)
                               openAppUpdates();
                       });
            },
            m_systemBus);
}

void UpdateMonitor::download()
{
    setState(State::Downloading);
    m_transaction = std::make_unique<packagekit::Transaction>(m_systemBus);
    m_transactionDone = m_transaction->finished.connect([this](bool success, const std::string &error) {
        if (!success) {
            CF_WARNING(cfUpdates) << "Downloading the updates failed:" << error;
            setState(State::Idle);
            publish();
            return;
        }
        setState(State::Ready);
        publish();
        // Nothing is installed before the user says so.
        notify(tr("Updates ready to install"), tr("The updates were downloaded. Restart to install them."),
               {"default", "", "restart", tr("Restart and Install"), "later", tr("Later")},
               [this](const std::string &action) {
                   if (action == "later")
                       return;
                   if (action == "default") {
                       if (openSystemUpdates)
                           openSystemUpdates();
                       return;
                   }
                   packagekit::offline::trigger(
                           [this, alive = std::weak_ptr<bool>(m_alive)](bool armed, const std::string &error) {
                               if (!alive.lock())
                                   return;
                               if (!armed) {
                                   CF_WARNING(cfUpdates) << "Arming the offline update failed:" << error;
                                   return;
                               }
                               if (restart)
                                   restart();
                           },
                           m_systemBus);
               });
    });
    m_transaction->downloadUpdates(m_system, true);
}

void UpdateMonitor::readPrepared()
{
    packagekit::offline::prepared(
            [this, alive = std::weak_ptr<bool>(m_alive)](bool prepared) {
                if (!alive.lock())
                    return;
                if (prepared)
                    setState(State::Ready);
                else if (m_state == State::Ready)
                    setState(State::Idle);
                publish();
            },
            m_systemBus);
}

void UpdateMonitor::setAutoDownload(bool enabled)
{
    if (m_autoDownload == enabled)
        return;
    m_autoDownload = enabled;
    writeSettings(m_settingsPath, [enabled](base::Settings &settings) {
        settings.setValue("Updates/AutoDownload", enabled);
    });
    publish();
    if (enabled && !m_system.empty() && m_state == State::Idle)
        download();
}

void UpdateMonitor::setState(State state)
{
    if (m_state == state)
        return;
    m_state = state;
    publish();
}

void UpdateMonitor::notify(const std::string &title, const std::string &body,
                           const std::vector<std::string> &actions,
                           std::function<void(const std::string &action)> invoked)
{
    if (!m_bus)
        return;
    m_invoked = std::move(invoked);
    dbus::VariantMap hints;
    hints["desktop-entry"] = dbus::Variant(std::string("bayu-settings"));
    m_calls.track([&](std::function<void()> finished) {
        return m_notifications.callAsync(
                "Notify",
                [this, finished](const dbus::Reply &reply) {
                    uint32_t id = 0;
                    if (reply.read(id))
                        m_notification = id;
                    finished();
                },
                std::string(), m_notification, std::string("system-software-update"), title, body, actions, hints,
                int32_t(-1));
    });
}

void UpdateMonitor::updateBadge()
{
    // The count on the Settings icon, as an application puts one on its own.
    if (!m_bus)
        return;
    dbus::VariantMap properties;
    properties["count"] = dbus::Variant(int64_t(systemUpdates()));
    properties["count-visible"] = dbus::Variant(systemUpdates() > 0);
    m_bus->emitSignal("/org/bayu/Updates", "com.canonical.Unity.LauncherEntry", "Update",
                      std::string("application://bayu-settings.desktop"), properties);
}

void UpdateMonitor::publish()
{
    if (m_object) {
        m_object->emitPropertiesChanged({"SystemUpdates", "SecurityUpdates", "AppUpdates", "State", "AutoDownload"});
        m_object->emitSignal("Changed");
    }
    changed();
}

} // namespace bayu::shell
