// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BAYU_SHELL_STATUSBAR_STATUS_BAR_H_
#define BAYU_SHELL_STATUSBAR_STATUS_BAR_H_

#include <memory>
#include <string>

#include "base/dbus.h"
#include "base/signal.h"
#include "base/thread_pool.h"
#include "shell/bar_theme.h"
#include "shell/statusbar/app_menu_registrar.h"
#include "shell/statusbar/battery.h"
#include "shell/statusbar/brightness.h"
#include "shell/statusbar/mpris.h"
#include "shell/statusbar/power_actions.h"
#include "shell/statusbar/status_notifier_watcher.h"
#include "shell/statusbar/system_tray_model.h"

namespace bayu::powerprofiles {
class PowerProfiles;
} // namespace bayu::powerprofiles

namespace bayu::shell {

class TaskManager;

// The status bar without its views: the tray and its watcher, the AppMenu
// registrar, the media player, battery, brightness and power profiles, the
// power actions, the clock's format, and org.bayu.Statusbar at
// /Statusbar.
class StatusBar
{
public:
    // `settingsPath` is shell.conf; `tasks` may be null.
    StatusBar(base::dbus::Connection *sessionBus, base::dbus::Connection *systemBus, TaskManager *tasks,
              std::string settingsPath);
    ~StatusBar();

    StatusBar(const StatusBar &) = delete;
    StatusBar &operator=(const StatusBar &) = delete;

    // org.bayu.Statusbar, org.kde.StatusNotifierWatcher and
    // com.canonical.AppMenu.Registrar; false when one is taken.
    bool registerServices();

    SystemTrayModel &systemTray() { return m_tray; }
    AppMenuRegistrar &appMenuRegistrar() { return m_registrar; }
    MprisManager &mpris() { return m_mpris; }
    Battery &battery() { return *m_battery; }
    Brightness &brightness() { return m_brightness; }
    powerprofiles::PowerProfiles &powerProfiles() { return *m_powerProfiles; }
    PowerActions &powerActions() { return m_powerActions; }

    // The clock follows the primary language, not LC_TIME: the region the
    // system was installed with would otherwise pick its words.
    const std::string &timeLocale() const { return m_timeLocale; }
    // A format string for base::Locale(timeLocale()).toString().
    const std::string &timeFormat() const { return m_timeFormat; }
    bool twentyFourTime() const { return m_twentyFourTime; }
    void setTwentyFourTime(bool enabled);
    bool showSeconds() const { return m_showSeconds; }
    void setShowSeconds(bool enabled);

    base::Signal<> timeFormatChanged;

    // Shell/Layout: the top bar with the dock, or this bar alone along the
    // bottom of the screen as the panel, with the menus in the windows.
    enum class Layout { Dock = 0, Panel = 1 };
    Layout layout() const { return m_layout; }
    void setLayout(Layout layout);
    // Panel/Floating: the panel stands off the screen's edge while no window
    // is open.
    bool panelFloating() const { return m_panelFloating; }
    void setPanelFloating(bool floating);
    // Panel/IconSize: kPanelIconSizes' small or medium.
    static constexpr int kSmallPanelIcons = 32;
    static constexpr int kMediumPanelIcons = 36;
    int panelIconSize() const { return m_panelIconSize; }
    void setPanelIconSize(int size);
    // Panel/GlassTint, from clear (0) to tinted (1), as the dock's.
    double panelGlassTint() const { return m_panelGlassTint; }
    void setPanelGlassTint(double tint);
    // Panel/WindowTitles: a button with its title for each window, instead
    // of the applications' icons.
    bool panelWindowTitles() const { return m_panelWindowTitles; }
    void setPanelWindowTitles(bool titles);

    // Shell/Theme: the material the dock and the top bar share.
    BarTheme theme() const { return m_theme; }
    void setTheme(BarTheme theme);

    base::Signal<> layoutChanged;
    base::Signal<> panelChanged;
    base::Signal<> themeChanged;

private:
    void reloadLanguage();
    void updateTimeFormat();

    base::dbus::Connection *m_bus;
    std::string m_settingsPath;
    StatusNotifierWatcher m_watcher;
    SystemTrayModel m_tray;
    AppMenuRegistrar m_registrar;
    MprisManager m_mpris;
    std::unique_ptr<Battery> m_battery;
    Brightness m_brightness;
    std::unique_ptr<powerprofiles::PowerProfiles> m_powerProfiles;
    PowerActions m_powerActions;
    std::unique_ptr<base::dbus::Object> m_object;
    base::dbus::Match m_languageChanged;
    base::Job m_load;
    base::Job m_language;
    bool m_twentyFourTime = false;
    bool m_twentyFourTimeSet = false;
    bool m_showSeconds = false;
    bool m_showSecondsSet = false;
    std::string m_timeLocale;
    std::string m_timeFormat;
    Layout m_layout = Layout::Dock;
    bool m_panelFloating = true;
    int m_panelIconSize = kMediumPanelIcons;
    double m_panelGlassTint = 0.5;
    bool m_panelWindowTitles = false;
    BarTheme m_theme = BarTheme::Default;
};

} // namespace bayu::shell

#endif // BAYU_SHELL_STATUSBAR_STATUS_BAR_H_
