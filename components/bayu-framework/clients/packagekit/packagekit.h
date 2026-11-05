// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "base/dbus.h"
#include "base/signal.h"

namespace bayu::packagekit {

// A package as PackageKit names it: "name;version;arch;repository".
struct Package
{
    std::string id;
    std::string summary;
    // PackageKit's PkInfoEnum: 8 is a security update.
    uint32_t info = 0;

    std::string name() const;
    std::string version() const;
    std::string repository() const;
    bool isSecurity() const { return info == 8; }
};

// One PackageKit transaction on the system bus: made on start, its
// packages, progress and end told by signals. Destroying it cancels
// nothing; cancel() asks PackageKit to.
class Transaction
{
public:
    enum class Kind { RefreshCache, GetUpdates, DownloadUpdates };

    explicit Transaction(base::dbus::Connection* bus = base::dbus::Connection::system());
    ~Transaction();

    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    // `background` lets PackageKit run it at low priority, without asking
    // the user anything (polkit's no-interaction). `force` reads every
    // repository again, though its metadata has not expired, as a check the
    // user asks for must (issue #123). Two functions, so that what was built
    // against the first keeps linking.
    void refreshCache(bool background = false);
    void refreshCache(bool background, bool force);
    void getUpdates(bool background = false);
    // Downloads the updates only (PackageKit's only-download flag), for
    // the offline update to install at the next restart.
    void downloadUpdates(const std::vector<std::string>& packageIds, bool background = false);
    void cancel();

    bool isRunning() const { return m_running; }
    // 0-100, or -1 while PackageKit cannot tell.
    int percentage() const { return m_percentage; }
    // PackageKit's PkStatusEnum: 8 downloading, 9 installing, ...
    uint32_t status() const { return m_status; }
    uint64_t downloadSizeRemaining() const { return m_downloadSizeRemaining; }
    // The package being worked on, as its id.
    const std::string& currentPackage() const { return m_currentPackage; }
    const std::vector<Package>& packages() const { return m_packages; }

    base::Signal<const Package&> package;
    base::Signal<> progressChanged;
    // Success, or the error PackageKit gave (empty when it was cancelled).
    base::Signal<bool, const std::string&> finished;

private:
    void start(Kind kind, std::vector<std::string> ids, bool background);
    bool m_force = false;
    void run(const std::string& path);
    void end(bool success, const std::string& error);

    base::dbus::Connection* m_bus;
    Kind m_kind = Kind::GetUpdates;
    std::vector<std::string> m_ids;
    bool m_background = false;
    // Asked first with only-trusted; again without it when PackageKit needs.
    bool m_onlyTrusted = true;
    std::unique_ptr<base::dbus::Proxy> m_transaction;
    std::vector<base::dbus::Match> m_matches;
    base::dbus::PendingCall m_call;
    bool m_running = false;
    bool m_cancelled = false;
    int m_percentage = -1;
    uint32_t m_status = 0;
    uint64_t m_downloadSizeRemaining = 0;
    std::string m_currentPackage;
    std::string m_error;
    std::vector<Package> m_packages;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};

// The offline update: what was downloaded is installed by
// packagekit-offline-update.service at the next restart.
namespace offline {

// Whether a downloaded update waits to be installed.
void prepared(std::function<void(bool)> done, base::dbus::Connection* bus = base::dbus::Connection::system());
// Arms it for the next restart; false when PackageKit refused.
void trigger(std::function<void(bool, const std::string&)> done,
             base::dbus::Connection* bus = base::dbus::Connection::system());

} // namespace offline

} // namespace bayu::packagekit
