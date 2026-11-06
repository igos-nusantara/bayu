// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shell/statusbar/wireless_status.h"

#include "base/dbus.h"
#include "bayu/networkmanager/client.h"
#include "bayu/networkmanager/handler.h"
#include "bayu/networkmanager/wifi_utils.h"

namespace bayu::shell {

namespace dbus = base::dbus;
namespace nm = networkmanager;

WirelessStatus::WirelessStatus()
    : m_client(nm::Client::instance())
    , m_handler(std::make_unique<nm::Handler>())
{
    m_subscriptions.push_back(m_client->changed.connect([this] {
        updateActiveConnection();
        changed();
    }));
    m_subscriptions.push_back(m_client->readyChanged.connect([this] {
        updateActiveConnection();
        changed();
    }));
    m_subscriptions.push_back(m_handler->hotspotActiveChanged.connect([this] { changed(); }));
    updateActiveConnection();
}

WirelessStatus::~WirelessStatus()
{
    m_subscriptions.clear();
}

bool WirelessStatus::ready() const
{
    return m_client->ready();
}

StatusBarIndicators::Network WirelessStatus::network() const
{
    StatusBarIndicators::Network network;
    network.wirelessHwEnabled = ready() && m_client->manager("WirelessHardwareEnabled").value<bool>(false);
    network.wirelessEnabled = ready() && m_client->manager("WirelessEnabled").value<bool>(false);
    network.hotspotActive = m_handler->hotspotActive();
    network.wirelessName = m_wirelessName;
    network.wirelessIcon = m_wirelessIcon;
    network.wiredHwEnabled = m_wiredHwEnabled;
    network.wiredConnected = m_wiredConnected;
    network.wiredPrimary = m_wiredPrimary;
    network.wiredName = m_wiredName;
    network.wiredIcon = m_wiredConnected ? "network-wired"
            : m_wiredHwEnabled        ? "network-wired-unavailable"
                                      : std::string();
    return network;
}

// NM.ActiveConnection: the wired connection that is up, and the Wi-Fi network
// this machine is on, the primary one if there are several; a hotspot is not
// a network this machine is on.
void WirelessStatus::updateActiveConnection()
{
    std::string name;
    std::string icon;
    bool wiredHwEnabled = false;
    bool wiredConnected = false;
    bool wiredPrimary = false;
    std::string wiredName;
    if (ready()) {
        for (const std::string &device : m_client->devices()) {
            if (m_client->deviceType(device) == nm::DeviceEthernet) {
                wiredHwEnabled = true;
                break;
            }
        }

        const auto primary = m_client->manager("PrimaryConnection").get<dbus::ObjectPath>();
        std::string chosen;
        std::string device;
        for (const std::string &active : m_client->activeConnections()) {
            const auto devices = m_client->property(active, nm::activeInterface, "Devices")
                                         .value<std::vector<dbus::ObjectPath>>();
            const std::string type =
                    m_client->property(active, nm::activeInterface, "Type").value<std::string>();
            if (m_client->property(active, nm::activeInterface, "State").value<uint32_t>()
                        != nm::ActiveActivated
                || devices.empty())
                continue;
            const bool isPrimary = primary && active == primary->path;

            if (type == "802-3-ethernet") {
                wiredConnected = true;
                if (wiredName.empty() || isPrimary) {
                    wiredPrimary = isPrimary;
                    wiredName = m_client->property(active, nm::activeInterface, "Id").value<std::string>();
                    if (wiredName.empty())
                        wiredName = m_client->property(devices.front().path, nm::deviceInterface, "Interface")
                                            .value<std::string>();
                }
                continue;
            }

            if (type != "802-11-wireless"
                || m_client->property(devices.front().path, nm::wirelessInterface, "Mode").value<uint32_t>()
                        == nm::ModeAp)
                continue;
            if (chosen.empty() || isPrimary) {
                chosen = active;
                device = devices.front().path;
            }
        }
        if (!chosen.empty()) {
            const auto accessPoint =
                    m_client->property(device, nm::wirelessInterface, "ActiveAccessPoint").get<dbus::ObjectPath>();
            const std::string path = accessPoint ? accessPoint->path : std::string();
            const auto bytes = m_client->property(path, nm::accessPointInterface, "Ssid")
                                       .value<std::vector<uint8_t>>();
            const std::string ssid(bytes.begin(), bytes.end());
            // A hidden network's access point has no name of its own; its profile does.
            name = nm::wifi::ssidHidden(ssid)
                    ? m_client->property(chosen, nm::activeInterface, "Id").value<std::string>()
                    : nm::wifi::ssidLabel(ssid);
            icon = nm::wifi::strengthIcon(
                    int(m_client->property(path, nm::accessPointInterface, "Strength").value<uint8_t>()));
        }
    }
    m_wirelessName = name;
    m_wirelessIcon = icon;
    m_wiredHwEnabled = wiredHwEnabled;
    m_wiredConnected = wiredConnected;
    m_wiredPrimary = wiredPrimary;
    m_wiredName = wiredName;
}

} // namespace bayu::shell
