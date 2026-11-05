// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BAYU_SHELL_STATUSBAR_WIRELESS_STATUS_H_
#define BAYU_SHELL_STATUSBAR_WIRELESS_STATUS_H_

#include <memory>
#include <string>
#include <vector>

#include "base/signal.h"
#include "shell/statusbar/status_bar_panels.h"

namespace bayu::networkmanager {
class Client;
class Handler;
} // namespace bayu::networkmanager

namespace bayu::shell {

// What the bar's network icon shows, from NetworkManager on the system bus:
// the Wi-Fi switches and hotspot, the Wi-Fi network this machine is on, and
// the wired connection when one is up.
class WirelessStatus
{
public:
    WirelessStatus();
    ~WirelessStatus();

    WirelessStatus(const WirelessStatus &) = delete;
    WirelessStatus &operator=(const WirelessStatus &) = delete;

    // For NetworkManager requests, which share its hotspot tracking.
    networkmanager::Handler &handler() const { return *m_handler; }

    StatusBarIndicators::Network network() const;

    base::Signal<> changed;

private:
    bool ready() const;
    void updateActiveConnection();

    networkmanager::Client *m_client;
    std::unique_ptr<networkmanager::Handler> m_handler;
    std::string m_wirelessName;
    std::string m_wirelessIcon;
    bool m_wiredHwEnabled = false;
    bool m_wiredConnected = false;
    bool m_wiredPrimary = false;
    std::string m_wiredName;
    std::vector<base::Subscription> m_subscriptions;
};

} // namespace bayu::shell

#endif // BAYU_SHELL_STATUSBAR_WIRELESS_STATUS_H_
