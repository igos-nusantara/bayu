// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BAYU_SHELL_DOCK_DOCK_SETTINGS_H_
#define BAYU_SHELL_DOCK_DOCK_SETTINGS_H_

#include <string>

#include "base/signal.h"
#include "base/thread_pool.h"
#include "base/timer.h"
#include "shell/bar_theme.h"

namespace bayu::shell {

// The dock's [Dock] group of shell.conf: icon size, edge, visibility, glass
// tint and the minimized area. Missing keys are written with their
// defaults. Read on the file thread;
// the defaults stand until isReady().
class DockSettings
{
public:
    enum Direction {
        Left = 0,
        Bottom,
        Right,
    };

    enum Visibility {
        AlwaysShow = 0,
        AlwaysHide,
        IntellHide,
    };

    // The icon size the dock's metrics scale from.
    static constexpr int kBaseIconSize = 54;
    // A new user's: medium, and the dock hides when a window needs its room.
    static constexpr int kDefaultIconSize = 44;
    static constexpr Visibility kDefaultVisibility = IntellHide;

    // `path` is shell.conf.
    explicit DockSettings(std::string path);

    bool isReady() const { return m_ready; }

    int iconSize() const { return m_iconSize; }
    // Written once the size has stopped changing for a moment, as a slider
    // drags it.
    void setIconSize(int iconSize);
    Direction direction() const { return m_direction; }
    void setDirection(Direction direction);
    Visibility visibility() const { return m_visibility; }
    void setVisibility(Visibility visibility);
    int edgeMargins() const { return m_edgeMargins; }
    // Shell/Theme: the material the dock and the top bar share.
    BarTheme theme() const { return m_theme; }
    void setTheme(BarTheme theme);
    bool minimizedArea() const { return m_minimizedArea; }
    void setMinimizedArea(bool enabled);

    base::Signal<> ready;
    base::Signal<> iconSizeChanged;
    base::Signal<> directionChanged;
    base::Signal<> visibilityChanged;
    base::Signal<> themeChanged;
    base::Signal<> minimizedAreaChanged;

private:
    struct Values {
        int iconSize = kDefaultIconSize;
        Direction direction = Bottom;
        Visibility visibility = kDefaultVisibility;
        int edgeMargins = 10;
        BarTheme theme = BarTheme::Default;
        bool minimizedArea = true;
    };

    static Values load(const std::string &path);
    void write(const char *key, int value);

    std::string m_path;
    bool m_ready = false;
    int m_iconSize = kDefaultIconSize;
    Direction m_direction = Bottom;
    Visibility m_visibility = kDefaultVisibility;
    int m_edgeMargins = 10;
    BarTheme m_theme = BarTheme::Default;
    bool m_minimizedArea = true;
    base::Timer m_iconSizeSave;
    base::Job m_load;
};

} // namespace bayu::shell

#endif // BAYU_SHELL_DOCK_DOCK_SETTINGS_H_
