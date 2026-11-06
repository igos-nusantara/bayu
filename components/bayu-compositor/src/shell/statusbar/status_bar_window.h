// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BAYU_SHELL_STATUSBAR_STATUS_BAR_WINDOW_H_
#define BAYU_SHELL_STATUSBAR_STATUS_BAR_WINDOW_H_

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "base/geometry.h"
#include "base/signal.h"
#include "render/glass.h"
#include "shell/shell.h"
#include "shell/statusbar/bar_item.h"
#include "shell/statusbar/status_bar_menu_bar.h"
#include "shell/statusbar/status_bar_panels.h"

namespace bayu::ui {
class Animation;
class Menu;
}

namespace bayu::shell {

class ClockItem;
class DeviceUsage;
class DockModel;
class PanelTasks;
class VpnNotifier;
class ShellWindow;
class StatusBar;
class StatusBarBackdrop;
class StatusIcon;
class StopItem;
class SystemTrayView;
class UpdateMonitor;
class Wallpaper;

// The status bar: a 32 px strip along the top of the primary output, reserved
// from the work area, with no background of its own. Left to right: the menu
// bar, the stop buttons of a recording and of an agent's control, the tray, the
// volume, Wi-Fi, battery and control centre icons, and the clock. Its contents
// are light or dark by what the wallpaper shows behind it.
//
// In the panel layout (Shell/Layout) the bar is the panel instead: the only
// bar, along the bottom, on its own glass, with the Start button, the pinned
// applications and the windows in the menu bar's place. It stands off the
// screen's edge while no window is open (Panel/Floating) and lies on the edge
// once one is, reserving its height from the work area.
class StatusBarWindow : public Component, public BarContext
{
public:
    static constexpr int kHeight = 32;
    // The panel on the edge: Pitaloka's headerbar height, so a maximized
    // window's header bar lines up with it.
    static constexpr int kPanelHeight = 48;
    // Standing off the edge.
    static constexpr int kFloatingHeight = 56;
    static constexpr int kFloatingBottom = 16;
    static constexpr int kFloatingSide = 24;
    static constexpr int kFloatingRadius = 12;
    static constexpr int kPanelTransitionMs = 180;

    // `statusBar` is the backend: tray, registrar, battery, power actions,
    // the clock's format and org.bayu.Statusbar.
    StatusBarWindow(Shell *shell, std::unique_ptr<StatusBar> statusBar);
    ~StatusBarWindow() override;

    StatusBar &statusBar() { return *m_statusBar; }
    StatusBarBackdrop &backdrop() { return *m_backdrop; }
    // What the desktop shows decides the bar's contents; null leaves them to
    // the theme.
    void setWallpaper(Wallpaper *wallpaper);
    // Which applications use a camera, a microphone or record the system's
    // audio: the control icon's dot, and the control centre's tile.
    DeviceUsage *deviceUsage() const { return m_deviceUsage.get(); }
    // Tests give their own; the session's is made with the bar on a bus.
    void setDeviceUsage(std::unique_ptr<DeviceUsage> usage);
    // Tells a VPN that went down on its own (NetworkManager's
    // Handler::vpnFailed, from the control centre's client).
    VpnNotifier &vpnNotifier() { return *m_vpnNotifier; }
    ShellWindow *window() const { return m_window.get(); }

    // --- The menu bar, the panels and the indicators of other components ---
    // The menu bar goes in the first slot; null takes it out (before its
    // component goes).
    StatusBarMenuBarHost menuBarHost();
    void setMenuBar(std::unique_ptr<StatusBarMenuBar> menuBar);

    void addPanels(StatusBarPanels *panels);
    void removePanels(StatusBarPanels *panels);
    void setIndicators(StatusBarIndicators *indicators);
    // An item's click: every popup closes, then the panel opens unless it
    // was the one open.
    void togglePanel(StatusBarPanels::Panel panel);
    // Closes the panels and every other popup of the bar.
    void closePanels();
    void closePopups();

    // The stop buttons, shown while a recording or an agent's control runs.
    void setRecording(bool recording);
    // The active desk's auto-tiling (#149); a click toggles it.
    void setTiling(bool tiled);
    std::function<void()> onToggleTiling;
    std::function<void()> onStopRecording;
    void setAgentControl(bool active);
    std::function<void()> onStopAgentControl;

    // --- The panel layout ---
    // The dock's pinned applications, the launcher's toggle and the dock's
    // glass for a tint: the panel's, from the components that own them.
    void setPanelSources(DockModel *applications, std::function<void()> toggleLauncher,
                         std::function<render::GlassParams(double tint)> glass);
    bool isPanel() const;
    // The updates waiting: an icon while there are any, with a menu to see
    // them. Null takes it away (before the monitor goes).
    void setUpdates(UpdateMonitor *updates);
    // The panel stands off the edge (or is on its way there).
    bool isFloating() const { return m_floating; }
    PanelTasks *panelTasks() const { return m_panelTasks; }

    // --- For tests and the reference render ---
    SystemTrayView *tray() const { return m_tray; }
    StatusIcon *volumeIcon() const { return m_volume; }
    StatusIcon *networkIcon() const { return m_network; }
    StatusIcon *batteryIcon() const { return m_battery; }
    StatusIcon *controlIcon() const { return m_control; }
    ClockItem *clock() const { return m_clock; }
    // The bar's contents are light.
    bool lightContents() const override;
    // One of the panels is open.
    bool panelOpen() const;
    // Reads the clock now.
    void updateClock();

private:
    class Root;
    class PanelSlot;

    void updateLayoutMode();
    void updateFloating(bool animate);
    bool anyWindowOpen() const;
    // The panel's rect on the screen, `floating` of the way off the edge.
    base::Rect panelRect(double floating) const;
    void paintPanel(ui::Painter &painter);
    void updatePanelGlass();
    void updateMinimizeTargets();
    void updateGeometry();
    void updateLook();
    void updateIndicators();
    void updateBattery();
    void layoutRow();
    void armClock();
    StatusBarPanels *panelsFor(StatusBarPanels::Panel panel) const;
    BarItem *itemFor(StatusBarPanels::Panel panel) const;
    void updateChecked();
    void updateTopbarGlass();
    ui::RectF availableGeometry() const;

    Shell *m_shell;
    std::unique_ptr<StatusBar> m_statusBar;
    std::unique_ptr<StatusBarBackdrop> m_backdrop;
    std::unique_ptr<DeviceUsage> m_deviceUsage;
    std::unique_ptr<VpnNotifier> m_vpnNotifier;
    base::Subscription m_deviceUsageChanged;
    std::unique_ptr<ShellWindow> m_window;
    Root *m_root = nullptr;
    // The slot's placeholder, or the menu bar.
    ui::View *m_menuSlot = nullptr;
    StatusBarMenuBar *m_menuBar = nullptr;
    StopItem *m_recording = nullptr;
    StopItem *m_agentControl = nullptr;
    SystemTrayView *m_tray = nullptr;
    StatusIcon *m_tiling = nullptr;
    StatusIcon *m_volume = nullptr;
    StatusIcon *m_network = nullptr;
    StatusIcon *m_battery = nullptr;
    StatusIcon *m_control = nullptr;
    ClockItem *m_clock = nullptr;
    std::vector<StatusBarPanels *> m_panels;
    std::vector<base::Subscription> m_panelSubscriptions;
    StatusBarIndicators *m_indicators = nullptr;
    base::Subscription m_indicatorsChanged;
    // A wall-clock timerfd: it fires on the minute (or second) the clock
    // shows next, after a suspend, and whenever the system time is set.
    int m_clockFd = -1;
    int m_clockWatch = 0;
    bool m_lightContents = false;
    // The top bar's dock-theme glass.
    render::GlassParams m_topbarGlass;
    bool m_topbarGlassSet = false;
    base::Rect m_topbarGlassRect;
    // The panel layout's.
    ui::View *m_panelSlot = nullptr;
    PanelTasks *m_panelTasks = nullptr;
    DockModel *m_applications = nullptr;
    std::function<void()> m_toggleLauncher;
    std::function<render::GlassParams(double)> m_glass;
    bool m_panelShown = false;
    bool m_floating = false;
    // 0 on the edge, 1 off it.
    double m_floatProgress = 0;
    double m_floatFrom = 0;
    std::unique_ptr<ui::Animation> m_floatAnimation;
    std::map<uint64_t, base::Rect> m_sentTargets;
    std::vector<base::Subscription> m_panelLayoutSubscriptions;
    StatusIcon *m_updates = nullptr;
    UpdateMonitor *m_updateMonitor = nullptr;
    base::Subscription m_updatesChanged;
    std::unique_ptr<ui::Menu> m_updatesMenu;
    void updateUpdates();
    void showUpdatesMenu();
    std::vector<base::Subscription> m_subscriptions;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};

} // namespace bayu::shell

#endif // BAYU_SHELL_STATUSBAR_STATUS_BAR_WINDOW_H_
