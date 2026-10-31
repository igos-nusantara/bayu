// SPDX-License-Identifier: BSD-3-Clause

#include "bayu/packagekit/packagekit.h"

#include <utility>

using namespace bayu::base::dbus;

namespace bayu::packagekit {

static constexpr const char* kService = "org.freedesktop.PackageKit";
static constexpr const char* kPath = "/org/freedesktop/PackageKit";
static constexpr const char* kTransaction = "org.freedesktop.PackageKit.Transaction";
static constexpr const char* kOffline = "org.freedesktop.PackageKit.Offline";

// pk-enum.h: bitfields are 1 << the enum's value.
static constexpr uint64_t kFilterNone = uint64_t(1) << 1;
static constexpr uint64_t kFlagOnlyTrusted = uint64_t(1) << 1;
static constexpr uint64_t kFlagOnlyDownload = uint64_t(1) << 3;
// PkExitEnum.
static constexpr uint32_t kExitSuccess = 1;
static constexpr uint32_t kExitCancelled = 3;
static constexpr uint32_t kExitNeedUntrusted = 8;
// PkInfoEnum values a transaction tells of a package it works on, not of one
// it lists.
static constexpr uint32_t kInfoFirstWorking = 10;
// Percentage when PackageKit cannot tell.
static constexpr uint32_t kPercentageUnknown = 101;

static std::string field(const std::string& id, int index)
{
    size_t start = 0;
    for (int i = 0; i < index; ++i) {
        start = id.find(';', start);
        if (start == std::string::npos)
            return {};
        ++start;
    }
    const size_t end = id.find(';', start);
    return id.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

std::string Package::name() const
{
    return field(id, 0);
}

std::string Package::version() const
{
    return field(id, 1);
}

std::string Package::repository() const
{
    return field(id, 3);
}

Transaction::Transaction(Connection* bus)
    : m_bus(bus)
{
}

Transaction::~Transaction()
{
    *m_alive = false;
}

void Transaction::refreshCache(bool background)
{
    refreshCache(background, false);
}

void Transaction::refreshCache(bool background, bool force)
{
    m_force = force;
    start(Kind::RefreshCache, {}, background);
}

void Transaction::getUpdates(bool background)
{
    start(Kind::GetUpdates, {}, background);
}

void Transaction::downloadUpdates(const std::vector<std::string>& packageIds, bool background)
{
    m_onlyTrusted = true;
    start(Kind::DownloadUpdates, packageIds, background);
}

void Transaction::cancel()
{
    m_cancelled = true;
    if (m_transaction)
        m_transaction->send("Cancel");
}

void Transaction::start(Kind kind, std::vector<std::string> ids, bool background)
{
    if (m_running || !m_bus) {
        if (!m_bus)
            end(false, "no system bus");
        return;
    }
    m_kind = kind;
    m_ids = std::move(ids);
    m_background = background;
    m_running = true;
    m_cancelled = false;
    m_percentage = -1;
    m_status = 0;
    m_downloadSizeRemaining = 0;
    m_currentPackage.clear();
    m_error.clear();
    m_packages.clear();
    const Proxy daemon(m_bus, kService, kPath, kService);
    m_call = daemon.callAsync("CreateTransaction", [this, alive = std::weak_ptr<bool>(m_alive)](const Reply& reply) {
        if (!alive.lock())
            return;
        ObjectPath path;
        if (!reply.read(path)) {
            end(false, reply.isError() ? reply.error().message : "PackageKit is not available");
            return;
        }
        run(path.path);
    });
}

void Transaction::run(const std::string& path)
{
    m_transaction = std::make_unique<Proxy>(m_bus, kService, path, kTransaction);
    // Asked by the user: polkit may ask for a password (unsigned packages).
    m_transaction->setInteractiveAuthorizationAllowed(!m_background);
    m_matches.clear();
    m_matches.push_back(m_transaction->onSignal("Package", [this](const Message& message) {
        Package item;
        if (!message.read(item.info, item.id, item.summary))
            return;
        item.info &= 0xFFFF;
        if (item.info >= kInfoFirstWorking) {
            m_currentPackage = item.id;
            progressChanged.emit();
            return;
        }
        m_packages.push_back(item);
        package.emit(m_packages.back());
    }));
    m_matches.push_back(m_transaction->onSignal("ItemProgress", [this](const Message& message) {
        std::string id;
        uint32_t status = 0;
        uint32_t percentage = 0;
        if (!message.read(id, status, percentage))
            return;
        m_currentPackage = id;
        progressChanged.emit();
    }));
    m_matches.push_back(m_transaction->onSignal("ErrorCode", [this](const Message& message) {
        uint32_t code = 0;
        std::string details;
        if (message.read(code, details))
            m_error = details;
    }));
    m_matches.push_back(m_transaction->onSignal("Finished", [this](const Message& message) {
        uint32_t exit = 0;
        uint32_t runtime = 0;
        message.read(exit, runtime);
        // A package from a repository without signatures: asked again
        // without only-trusted, which polkit authorizes on its own. In the
        // background nothing unsigned is downloaded unasked.
        if (exit == kExitNeedUntrusted && m_onlyTrusted && !m_background) {
            m_onlyTrusted = false;
            m_running = false;
            m_matches.clear();
            m_transaction.reset();
            start(m_kind, m_ids, m_background);
            return;
        }
        if (exit == kExitNeedUntrusted)
            m_error = "Some updates are not signed";
        end(exit == kExitSuccess, exit == kExitCancelled ? std::string() : m_error.empty() ? "failed" : m_error);
    }));
    m_matches.push_back(m_transaction->onPropertiesChanged(
            [this](const VariantMap& changed, const std::vector<std::string>&) {
                if (const auto it = changed.find("Percentage"); it != changed.end()) {
                    const uint32_t value = it->second.value<uint32_t>();
                    m_percentage = value >= kPercentageUnknown ? -1 : int(value);
                }
                if (const auto it = changed.find("Status"); it != changed.end())
                    m_status = it->second.value<uint32_t>();
                if (const auto it = changed.find("DownloadSizeRemaining"); it != changed.end())
                    m_downloadSizeRemaining = it->second.value<uint64_t>();
                progressChanged.emit();
            }));

    std::vector<std::string> hints {"interactive=" + std::string(m_background ? "false" : "true")};
    if (m_background)
        hints.push_back("background=true");
    m_transaction->send("SetHints", hints);
    const auto failed = [this, alive = std::weak_ptr<bool>(m_alive)](const Reply& reply) {
        // A refused call (polkit, a bad argument) gives no Finished.
        if (alive.lock() && reply.isError() && m_running)
            end(false, reply.error().message);
    };
    switch (m_kind) {
    case Kind::RefreshCache:
        m_call = m_transaction->callAsync("RefreshCache", failed, m_force);
        break;
    case Kind::GetUpdates:
        m_call = m_transaction->callAsync("GetUpdates", failed, kFilterNone);
        break;
    case Kind::DownloadUpdates:
        m_call = m_transaction->callAsync("UpdatePackages", failed,
                                          (m_onlyTrusted ? kFlagOnlyTrusted : 0) | kFlagOnlyDownload, m_ids);
        break;
    }
    if (m_cancelled)
        m_transaction->send("Cancel");
}

void Transaction::end(bool success, const std::string& error)
{
    if (!m_running && m_bus)
        return;
    m_running = false;
    m_matches.clear();
    m_transaction.reset();
    finished.emit(success, error);
}

namespace offline {

void prepared(std::function<void(bool)> done, Connection* bus)
{
    if (!bus) {
        done(false);
        return;
    }
    auto proxy = std::make_shared<Proxy>(bus, kService, kPath, kOffline);
    auto call = std::make_shared<PendingCall>();
    *call = proxy->getPropertyAsync("UpdatePrepared", [proxy, call, done](const Reply& reply) {
        const auto value = reply.value<Variant>();
        done(value && value->value<bool>());
    });
}

void trigger(std::function<void(bool, const std::string&)> done, Connection* bus)
{
    if (!bus) {
        done(false, "no system bus");
        return;
    }
    auto proxy = std::make_shared<Proxy>(bus, kService, kPath, kOffline);
    // The user chose it: polkit may ask for the password.
    proxy->setInteractiveAuthorizationAllowed(true);
    auto call = std::make_shared<PendingCall>();
    *call = proxy->callAsync(
            "Trigger",
            [proxy, call, done](const Reply& reply) {
                done(!reply.isError(), reply.isError() ? reply.error().message : std::string());
            },
            std::string("reboot"));
}

} // namespace offline

} // namespace bayu::packagekit
