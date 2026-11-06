// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "base/dbus.h"

namespace settings {

// The dock's settings and the desktop's layout: read from the shell's file,
// changed through the dock and the status bar, which own that file.
class DockSettings
{
public:
    DockSettings();

    int iconSize() const { return m_iconSize; }
    void setIconSize(int size);

    // 0 left, 1 bottom, 2 right.
    int direction() const { return m_direction; }
    void setDirection(int direction);

    // 0 always shown, 1 always hidden, 2 smart hide.
    int visibility() const { return m_visibility; }
    void setVisibility(int visibility);

    // Shell/Theme: the material the dock and the top bar share.
    int theme() const { return m_theme; }
    void setTheme(int theme);

    bool minimizedArea() const { return m_minimizedArea; }
    void setMinimizedArea(bool enabled);

    // Shell/Layout: 0 the top bar with the dock, 1 the panel alone.
    int layout() const { return m_layout; }
    void setLayout(int layout);
    // The panel's: off the screen's edge while no window is open, its icon
    // size (32 or 36) and its glass.
    bool panelFloating() const { return m_panelFloating; }
    void setPanelFloating(bool floating);
    int panelIconSize() const { return m_panelIconSize; }
    void setPanelIconSize(int size);
    double panelGlassTint() const { return m_panelGlassTint; }
    void setPanelGlassTint(double tint);
    // The open applications as icons (false) or a button with each window's title.
    bool panelWindowTitles() const { return m_panelWindowTitles; }
    void setPanelWindowTitles(bool titles);

private:
    bayu::base::dbus::Proxy m_dock;
    bayu::base::dbus::Proxy m_statusbar;
    int m_layout = 0;
    bool m_panelFloating = true;
    int m_panelIconSize = 36;
    double m_panelGlassTint = 0.5;
    bool m_panelWindowTitles = false;
    int m_iconSize = 0;
    int m_direction = 0;
    int m_visibility = 0;
    int m_theme = 0;
    bool m_minimizedArea = true;
};

} // namespace settings
