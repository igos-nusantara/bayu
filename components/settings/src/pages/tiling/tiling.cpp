// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "pages/tiling/tiling.h"

#include <cstdint>
#include <cstdio>
#include <sstream>

using namespace bayu::base;

namespace settings {

static constexpr const char* kService = "org.bayu.Tiling1";
static constexpr const char* kPath = "/org/bayu/Tiling1";

Tiling::Tiling()
    : m_tiling(dbus::Connection::session(), kService, kPath, kService)
{
    dbus::Connection* bus = m_tiling.connection();
    if (!bus)
        return;
    // Read at once, so the page has the values for its first frame.
    if (const dbus::Reply reply = m_tiling.getAll(); !reply.isError()) {
        m_properties = reply.value<dbus::VariantMap>().value_or(dbus::VariantMap());
        m_available = true;
    }
    m_changedMatch = m_tiling.onSignal("changed", [this](const dbus::Message&) { reload(); });
    m_watch = bus->watchName(kService, [this](const std::string& owner) {
        if (!owner.empty()) {
            reload();
        } else if (m_available) {
            m_available = false;
            m_properties.clear();
            changed.emit();
        }
    });
}

void Tiling::reload()
{
    m_reload = m_tiling.getAllAsync([this](const dbus::Reply& reply) {
        if (reply.isError())
            return;
        m_properties = reply.value<dbus::VariantMap>().value_or(dbus::VariantMap());
        m_available = true;
        changed.emit();
    });
}

bool Tiling::tilesByDefault() const
{
    const auto it = m_properties.find("tilesByDefault");
    return it != m_properties.end() && it->second.value<bool>(false);
}

int Tiling::gap() const
{
    const auto it = m_properties.find("gap");
    return it == m_properties.end() ? 8 : it->second.value<int32_t>(8);
}

std::vector<std::string> Tiling::floatingApplications() const
{
    std::vector<std::string> applications;
    const auto it = m_properties.find("floatingApplications");
    if (it == m_properties.end())
        return applications;
    std::istringstream lines(it->second.value<std::string>());
    for (std::string line; std::getline(lines, line);) {
        if (!line.empty())
            applications.push_back(line);
    }
    return applications;
}

static void report(const dbus::Reply& reply)
{
    if (reply.isError())
        fprintf(stderr, "Cannot update tiling: %s\n", reply.error().message.c_str());
}

void Tiling::setTilesByDefault(bool tiled)
{
    if (m_available)
        m_call = m_tiling.callAsync("setTilesByDefault", report, tiled);
}

void Tiling::setGap(int gap)
{
    if (m_available)
        m_call = m_tiling.callAsync("setGap", report, int32_t(gap));
}

void Tiling::setFloatingApplications(const std::vector<std::string>& applications)
{
    if (!m_available)
        return;
    std::string text;
    for (const std::string& application : applications)
        text += application + "\n";
    m_call = m_tiling.callAsync("setFloatingApplications", report, text);
}

} // namespace settings
