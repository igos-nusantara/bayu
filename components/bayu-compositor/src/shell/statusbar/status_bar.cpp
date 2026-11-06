// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shell/statusbar/status_bar.h"

#include <algorithm>
#include <utility>

#include "base/desktop_entry.h"
#include "base/logging.h"
#include "base/settings.h"
#include "bayu/powerprofiles/power_profiles.h"
#include "bayu/upower/battery_status.h"
#include "shell/common/file_thread.h"
#include "shell/statusbar/clock_format.h"

namespace bayu::shell {

namespace dbus = base::dbus;

static const base::LogCategory cfStatusBar("bayu.shell.statusbar");

StatusBar::StatusBar(dbus::Connection *sessionBus, dbus::Connection *systemBus, TaskManager *tasks,
                     std::string settingsPath)
    : m_bus(sessionBus)
    , m_settingsPath(std::move(settingsPath))
    , m_watcher(sessionBus)
    , m_tray(m_watcher, sessionBus, tasks)
    , m_registrar(sessionBus)
    , m_mpris(sessionBus)
    , m_battery(std::make_unique<Battery>(std::make_shared<upower::BatteryStatus>(sessionBus, systemBus),
                                          m_settingsPath))
    , m_brightness(sessionBus)
    , m_powerProfiles(std::make_unique<powerprofiles::PowerProfiles>(systemBus))
    , m_powerActions(sessionBus)
{
    m_battery->followService(sessionBus, [sessionBus, systemBus] {
        return std::make_shared<upower::BatteryStatus>(sessionBus, systemBus);
    });
    m_timeFormat = clock_format::pattern(m_timeLocale, m_twentyFourTime, m_showSeconds);

    // Read now, not on the file thread: the layout decides which of the bar
    // and the dock the session shows first.
    {
        const base::Settings settings(m_settingsPath);
        m_layout = settings.intValue("Shell/Layout", 0) == int(Layout::Panel) ? Layout::Panel : Layout::Dock;
        m_panelFloating = settings.boolValue("Panel/Floating", true);
        m_panelIconSize = settings.intValue("Panel/IconSize", kMediumPanelIcons) == kSmallPanelIcons
                ? kSmallPanelIcons
                : kMediumPanelIcons;
        m_panelGlassTint = std::clamp(settings.doubleValue("Panel/GlassTint", 0.5), 0.0, 1.0);
        m_panelWindowTitles = settings.boolValue("Panel/WindowTitles", false);
        m_theme = barThemeFromInt(settings.intValue("Shell/Theme", int(BarTheme::Default)));
    }

    m_load = runOnFileThread(
            [path = m_settingsPath] {
                const base::Settings settings(path);
                return std::pair(settings.boolValue("Statusbar/TwentyFourTime", false),
                                 settings.boolValue("Statusbar/ShowSeconds", false));
            },
            [this](std::pair<bool, bool> saved) {
                // A choice made while the file was read stays.
                bool changed = false;
                if (!m_twentyFourTimeSet && saved.first != m_twentyFourTime) {
                    m_twentyFourTime = saved.first;
                    changed = true;
                }
                if (!m_showSecondsSet && saved.second != m_showSeconds) {
                    m_showSeconds = saved.second;
                    changed = true;
                }
                if (changed)
                    updateTimeFormat();
            });

    if (m_bus) {
        dbus::MatchRule rule;
        rule.sender = "org.bayu.Services";
        rule.path = "/org/bayu/Services/Language";
        rule.interface = "org.bayu.Services.Language";
        rule.member = "languageChanged";
        m_languageChanged = m_bus->subscribe(rule, [this](const dbus::Message &) { reloadLanguage(); });
    }
    reloadLanguage();
}

StatusBar::~StatusBar()
{
    if (m_object && m_bus)
        m_bus->releaseName("org.bayu.Statusbar");
}

bool StatusBar::registerServices()
{
    if (!m_object) {
        if (!m_bus)
            return false;
        auto object = std::make_unique<dbus::Object>(m_bus, "/Statusbar", "org.bayu.Statusbar");
        object->addMethod("setBatteryPercentage", [this](bool enabled) { m_battery->setShowPercentage(enabled); },
                          dbus::ArgumentNames{{"batteryPercentage"}, {}});
        object->addMethod("setTwentyFourTime", [this](bool enabled) { setTwentyFourTime(enabled); },
                          dbus::ArgumentNames{{"t"}, {}});
        object->addMethod("setShowSeconds", [this](bool enabled) { setShowSeconds(enabled); },
                          dbus::ArgumentNames{{"showSeconds"}, {}});
        object->addMethod("setLayout", [this](int32_t layout) {
                              setLayout(layout == int(Layout::Panel) ? Layout::Panel : Layout::Dock);
                          },
                          dbus::ArgumentNames{{"layout"}, {}});
        // Whoever shows the layout follows it: 0 the dock, 1 the panel.
        object->addSignal("LayoutChanged", "i", {"layout"});
        object->addMethod("setPanelFloating", [this](bool floating) { setPanelFloating(floating); },
                          dbus::ArgumentNames{{"floating"}, {}});
        object->addMethod("setPanelIconSize", [this](int32_t size) { setPanelIconSize(size); },
                          dbus::ArgumentNames{{"size"}, {}});
        object->addMethod("setPanelGlassTint", [this](double tint) { setPanelGlassTint(tint); },
                          dbus::ArgumentNames{{"tint"}, {}});
        object->addMethod("setPanelWindowTitles", [this](bool titles) { setPanelWindowTitles(titles); },
                          dbus::ArgumentNames{{"titles"}, {}});
        object->addMethod("setTheme",
                          [this](int32_t theme) { setTheme(barThemeFromInt(theme)); },
                          dbus::ArgumentNames{{"theme"}, {}});
        object->addSignal("ThemeChanged", "i", {"theme"});
        // The same as the menu's: the user is asked first. The session's
        // own methods act at once.
        object->addMethod("requestLogout", [this] { m_powerActions.request(PowerActions::Logout); });
        object->addMethod("requestReboot", [this] { m_powerActions.request(PowerActions::Reboot); });
        object->addMethod("requestShutdown", [this] { m_powerActions.request(PowerActions::Shutdown); });
        if (!object->publish() || !m_bus->requestName("org.bayu.Statusbar", dbus::Connection::DoNotQueue)) {
            CF_WARNING(cfStatusBar) << "Cannot own org.bayu.Statusbar";
            return false;
        }
        m_object = std::move(object);
    }
    const bool watcher = m_watcher.registerService();
    // The panel has no menu bar: without the registrar, applications keep
    // their menus in their windows.
    const bool registrar = m_layout == Layout::Panel || m_registrar.registerService();
    return watcher && registrar;
}

void StatusBar::setTwentyFourTime(bool enabled)
{
    m_twentyFourTimeSet = true;
    if (m_twentyFourTime == enabled)
        return;
    m_twentyFourTime = enabled;
    writeSettings(m_settingsPath, [enabled](base::Settings &settings) {
        settings.setValue("Statusbar/TwentyFourTime", enabled);
    });
    updateTimeFormat();
}

void StatusBar::setShowSeconds(bool enabled)
{
    m_showSecondsSet = true;
    if (m_showSeconds == enabled)
        return;
    m_showSeconds = enabled;
    writeSettings(m_settingsPath, [enabled](base::Settings &settings) {
        settings.setValue("Statusbar/ShowSeconds", enabled);
    });
    updateTimeFormat();
}

void StatusBar::setLayout(Layout layout)
{
    if (m_layout == layout)
        return;
    m_layout = layout;
    writeSettings(m_settingsPath, [layout](base::Settings &settings) {
        settings.setValue("Shell/Layout", int(layout));
    });
    if (layout == Layout::Panel)
        m_registrar.unregisterService();
    else if (m_object)
        m_registrar.registerService();
    if (m_object)
        m_object->emitSignal("LayoutChanged", int32_t(layout));
    layoutChanged();
}

void StatusBar::setPanelFloating(bool floating)
{
    if (m_panelFloating == floating)
        return;
    m_panelFloating = floating;
    writeSettings(m_settingsPath, [floating](base::Settings &settings) {
        settings.setValue("Panel/Floating", floating);
    });
    panelChanged();
}

void StatusBar::setPanelIconSize(int size)
{
    size = size == kSmallPanelIcons ? kSmallPanelIcons : kMediumPanelIcons;
    if (m_panelIconSize == size)
        return;
    m_panelIconSize = size;
    writeSettings(m_settingsPath, [size](base::Settings &settings) { settings.setValue("Panel/IconSize", size); });
    panelChanged();
}

void StatusBar::setPanelGlassTint(double tint)
{
    tint = std::clamp(tint, 0.0, 1.0);
    if (m_panelGlassTint == tint)
        return;
    m_panelGlassTint = tint;
    writeSettings(m_settingsPath, [tint](base::Settings &settings) { settings.setValue("Panel/GlassTint", tint); });
    panelChanged();
}

void StatusBar::setPanelWindowTitles(bool titles)
{
    if (m_panelWindowTitles == titles)
        return;
    m_panelWindowTitles = titles;
    writeSettings(m_settingsPath, [titles](base::Settings &settings) {
        settings.setValue("Panel/WindowTitles", titles);
    });
    panelChanged();
}

void StatusBar::setTheme(BarTheme theme)
{
    if (m_theme == theme)
        return;
    m_theme = theme;
    writeSettings(m_settingsPath, [theme](base::Settings &settings) {
        settings.setValue("Shell/Theme", int(theme));
    });
    themeChanged();
    if (m_object)
        m_object->emitSignal("ThemeChanged", int32_t(theme));
}

void StatusBar::reloadLanguage()
{
    // The session language lives in bayu-services' settings.
    m_language = runOnFileThread([] { return base::DesktopEntry::displayLanguage(); },
                                 [this](std::string language) {
                                     if (language == m_timeLocale)
                                         return;
                                     m_timeLocale = language;
                                     updateTimeFormat();
                                 });
}

void StatusBar::updateTimeFormat()
{
    m_timeFormat = clock_format::pattern(m_timeLocale, m_twentyFourTime, m_showSeconds);
    timeFormatChanged();
}

} // namespace bayu::shell
