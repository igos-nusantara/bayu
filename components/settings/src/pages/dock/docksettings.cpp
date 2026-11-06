// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "pages/dock/docksettings.h"

#include <algorithm>
#include <cstdint>

#include "base/paths.h"
#include "base/settings.h"

using namespace bayu::base;

namespace settings {

DockSettings::DockSettings()
    : m_dock(dbus::Connection::session(), "org.bayu.Dock", "/Dock", "org.bayu.Dock")
    , m_statusbar(dbus::Connection::session(), "org.bayu.Statusbar", "/Statusbar", "org.bayu.Statusbar")
{
    const Settings shell(paths::bayuConfig("shell"));
    m_iconSize = int(shell.intValue("Dock/IconSize"));
    m_direction = int(shell.intValue("Dock/Direction"));
    m_visibility = int(shell.intValue("Dock/Visibility"));
    m_theme = int(shell.intValue("Shell/Theme", 0));
    if (m_theme < 0 || m_theme > 2)
        m_theme = 0;
    m_minimizedArea = shell.boolValue("Dock/MinimizedArea", true);
    m_layout = shell.intValue("Shell/Layout", 0) == 1 ? 1 : 0;
    m_panelFloating = shell.boolValue("Panel/Floating", true);
    m_panelIconSize = shell.intValue("Panel/IconSize", 36) == 32 ? 32 : 36;
    m_panelGlassTint = std::clamp(shell.doubleValue("Panel/GlassTint", 0.5), 0.0, 1.0);
    m_panelWindowTitles = shell.boolValue("Panel/WindowTitles", false);
}

// Called on every slider move; none of these waits for the dock.
void DockSettings::setIconSize(int size)
{
    if (size == m_iconSize)
        return;
    m_dock.send("setIconSize", int32_t(size));
    m_iconSize = size;
}

void DockSettings::setDirection(int direction)
{
    if (direction == m_direction)
        return;
    m_dock.send("setDirection", int32_t(direction));
    m_direction = direction;
}

void DockSettings::setVisibility(int visibility)
{
    if (visibility == m_visibility)
        return;
    m_dock.send("setVisibility", int32_t(visibility));
    m_visibility = visibility;
}

void DockSettings::setTheme(int theme)
{
    theme = std::clamp(theme, 0, 2);
    if (theme == m_theme)
        return;
    m_dock.send("setTheme", int32_t(theme));
    m_statusbar.send("setTheme", int32_t(theme));
    m_theme = theme;
}

void DockSettings::setMinimizedArea(bool enabled)
{
    if (enabled == m_minimizedArea)
        return;
    m_dock.send("setMinimizedArea", enabled);
    m_minimizedArea = enabled;
}

void DockSettings::setLayout(int layout)
{
    if (layout == m_layout)
        return;
    m_statusbar.send("setLayout", int32_t(layout));
    m_layout = layout;
}

void DockSettings::setPanelFloating(bool floating)
{
    if (floating == m_panelFloating)
        return;
    m_statusbar.send("setPanelFloating", floating);
    m_panelFloating = floating;
}

void DockSettings::setPanelIconSize(int size)
{
    if (size == m_panelIconSize)
        return;
    m_statusbar.send("setPanelIconSize", int32_t(size));
    m_panelIconSize = size;
}

void DockSettings::setPanelGlassTint(double tint)
{
    tint = std::clamp(tint, 0.0, 1.0);
    if (tint == m_panelGlassTint)
        return;
    m_statusbar.send("setPanelGlassTint", tint);
    m_panelGlassTint = tint;
}

void DockSettings::setPanelWindowTitles(bool titles)
{
    if (titles == m_panelWindowTitles)
        return;
    m_statusbar.send("setPanelWindowTitles", titles);
    m_panelWindowTitles = titles;
}

} // namespace settings
