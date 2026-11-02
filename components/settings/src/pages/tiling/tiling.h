// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <string>
#include <vector>

#include "base/dbus.h"
#include "base/signal.h"

namespace settings {

// The compositor's org.bayu.Tiling1 (#149): whether desks tile, the gap
// between tiles, and the applications whose windows always float.
class Tiling
{
public:
    Tiling();

    bool isAvailable() const { return m_available; }

    bool tilesByDefault() const;
    void setTilesByDefault(bool tiled);
    int gap() const;
    void setGap(int gap);
    std::vector<std::string> floatingApplications() const;
    void setFloatingApplications(const std::vector<std::string>& applications);

    bayu::base::Signal<> changed;

private:
    void reload();

    bayu::base::dbus::Proxy m_tiling;
    bayu::base::dbus::NameWatch m_watch;
    bayu::base::dbus::Match m_changedMatch;
    bayu::base::dbus::PendingCall m_reload;
    bayu::base::dbus::PendingCall m_call;
    bayu::base::dbus::VariantMap m_properties;
    bool m_available = false;
};

} // namespace settings
