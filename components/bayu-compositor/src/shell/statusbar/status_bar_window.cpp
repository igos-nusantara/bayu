// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shell/statusbar/status_bar_window.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>

#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "base/dbus.h"
#include "base/locale.h"
#include "core/main_loop.h"
#include "shell/common/task_windows.h"
#include "shell/shell_window.h"
#include "shell/statusbar/panel_tasks.h"
#include "shell/statusbar/pitaloka.h"
#include "shell/statusbar/clock_format.h"
#include "shell/statusbar/status_bar.h"
#include "shell/statusbar/device_usage.h"
#include "shell/statusbar/status_bar_backdrop.h"
#include "shell/statusbar/status_icon.h"
#include "shell/statusbar/system_tray_view.h"
#include "shell/statusbar/vpn_notifier.h"
#include "shell/updates/update_monitor.h"
#include "base/i18n.h"
#include "ui/menu.h"
#include "ui/animation.h"
#include "ui/embedded_window.h"
#include "ui/painter.h"
#include "ui/cell_layout.h"
#include "ui/path.h"
#include "ui/theme.h"
#include "wm/window.h"
#include "wm/window_manager.h"

namespace bayu::shell {

using Panel = StatusBarPanels::Panel;

// The bar's row; a press between its items closes every popup.
class StatusBarWindow::Root final : public ui::RowLayout
{
public:
    explicit Root(StatusBarWindow *bar)
        : m_bar(bar)
    {
        setName("statusbar");
        setSpacing(0);
    }

    bool acceptsPointer() const override { return true; }

protected:
    void pointerPress(const ui::PointerEvent &) override { m_bar->closePopups(); }

    void layout() override
    {
        ui::RowLayout::layout();
        // A theme change lays every window out again; the system's dark
        // mode is the bar's fallback.
        if (ui::Theme::generation() != m_themeGeneration) {
            m_themeGeneration = ui::Theme::generation();
            core::mainLoop()->post([bar = m_bar, alive = std::weak_ptr<bool>(m_bar->m_alive)] {
                if (alive.lock())
                    bar->updateLook();
            });
        }
    }

    // The plate moves with its item, wherever in the row it sits.
    void childrenLaidOut() override { m_bar->updatePlateBlur(); }

    void paint(ui::Painter &painter) override
    {
        if (m_bar->isPanel())
            m_bar->paintPanel(painter);
    }

private:
    StatusBarWindow *m_bar;
    uint64_t m_themeGeneration = 0;
};

// The menu bar's slot: what it holds fills it.
class MenuSlot final : public ui::View
{
protected:
    void layout() override
    {
        for (const auto &child : children())
            child->setGeometry(localRect());
    }
};

// The panel's left side: what it holds fills it, in from the panel's edge.
class StatusBarWindow::PanelSlot final : public ui::View
{
protected:
    void layout() override
    {
        const ui::RectF bounds = localRect();
        const double inset = (bounds.height - std::min(bounds.height, 36.0)) / 2 + 2;
        for (const auto &child : children())
            child->setGeometry({inset, 0, std::max(0.0, bounds.width - inset), bounds.height});
    }
};

StatusBarWindow::StatusBarWindow(Shell *shell, std::unique_ptr<StatusBar> statusBar)
    : m_shell(shell)
    , m_statusBar(std::move(statusBar))
    , m_backdrop(std::make_unique<StatusBarBackdrop>())
{
    base::dbus::Connection *bus = m_shell->bus();
    m_vpnNotifier = std::make_unique<VpnNotifier>(bus);
    m_vpnNotifier->openSettings = [shell] { shell->openSettings({"-m", "vpn"}); };
    m_window = std::make_unique<ShellWindow>(shell, wm::WindowRole::Panel);
    m_window->setFocusPolicy(wm::FocusPolicy::NoFocus);
    m_window->window()->setBackgroundColor(ui::Color {0, 0, 0, 0});
    auto root = std::make_unique<Root>(this);
    m_root = root.get();

    // Empty until the panel's sources plug in.
    m_panelSlot = m_root->addChild<PanelSlot>();
    m_panelSlot->setName("panel");
    m_panelSlot->setVisible(false);
    // Empty until the menu bar's component plugs in.
    m_menuSlot = m_root->addChild<MenuSlot>();
    m_menuSlot->setName("menubar");
    ui::LayoutHints fill;
    fill.fillWidth = true;
    fill.fillHeight = true;
    m_menuSlot->setLayoutHints(fill);
    m_menuSlot->setClipsChildren(true);
    m_panelSlot->setLayoutHints(fill);
    m_panelSlot->setClipsChildren(true);

    m_recording = m_root->addChild<StopItem>(this);
    m_recording->setName("recording");
    m_recording->setVisible(false);
    m_recording->onClicked = [this](const ui::PointerEvent &) {
        closePopups();
        if (onStopRecording)
            onStopRecording();
    };
    m_agentControl = m_root->addChild<StopItem>(this);
    m_agentControl->setName("agent-control");
    m_agentControl->setVisible(false);
    m_agentControl->onClicked = [this](const ui::PointerEvent &) {
        closePopups();
        if (onStopAgentControl)
            onStopAgentControl();
    };

    SystemTrayView::Host trayHost;
    trayHost.closePanels = [this] { closePanels(); };
    trayHost.closeMenuBar = [this] {
        if (m_menuBar)
            m_menuBar->closeMenu();
    };
    trayHost.closePopups = [this] { closePopups(); };
    trayHost.availableGeometry = [this] { return availableGeometry(); };
    trayHost.origin = [this] {
        const base::Point origin = m_window->geometry().topLeft();
        return ui::PointF {double(origin.x()), double(origin.y())};
    };
    m_tray = m_root->addChild<SystemTrayView>(this, &m_statusBar->systemTray(), std::move(trayHost));
    m_tray->setName("tray");
    m_tray->onCountChanged = [this] { layoutRow(); };

    // Shown while updates wait (setUpdates).
    m_updates = m_root->addChild<StatusIcon>(this);
    m_updates->setName("updates");
    m_updates->setIconName("updates");
    m_updates->setVisible(false);
    m_updates->onClicked = [this](const ui::PointerEvent &) { showUpdatesMenu(); };
    m_updates->onPreferredWidthChanged = [this] { layoutRow(); };

    m_tiling = m_root->addChild<StatusIcon>(this);
    m_tiling->setName("tiling");
    m_tiling->setIconName("tiling-off");
    m_tiling->onClicked = [this](const ui::PointerEvent &) {
        if (onToggleTiling)
            onToggleTiling();
    };
    m_tiling->onPreferredWidthChanged = [this] { layoutRow(); };

    const auto statusIcon = [this](const char *name, Panel panel) {
        StatusIcon *icon = m_root->addChild<StatusIcon>(this);
        icon->setName(name);
        icon->onClicked = [this, panel](const ui::PointerEvent &) { togglePanel(panel); };
        icon->onPreferredWidthChanged = [this] { layoutRow(); };
        return icon;
    };
    m_volume = statusIcon("volume", Panel::Volume);
    m_network = statusIcon("network", Panel::Network);
    m_battery = statusIcon("battery", Panel::Battery);
    m_battery->setIconWidth(BarContext::kIconSize + 6);
    m_control = statusIcon("control", Panel::ControlCenter);
    m_control->setIconName("control");
    m_clock = m_root->addChild<ClockItem>(this);
    m_clock->onClicked = [this](const ui::PointerEvent &) { togglePanel(Panel::NotificationCenter); };
    m_clock->onPreferredWidthChanged = [this] { layoutRow(); };

    m_window->setContentView(std::move(root));

    // The clock, in the primary language's words.
    m_clockFd = timerfd_create(CLOCK_REALTIME, TFD_CLOEXEC | TFD_NONBLOCK);
    if (m_clockFd >= 0) {
        m_clockWatch = core::mainLoop()->watch(m_clockFd, EPOLLIN, [this](uint32_t) {
            uint64_t expirations = 0;
            // Fails with ECANCELED when the system time was set.
            [[maybe_unused]] const ssize_t size = read(m_clockFd, &expirations, sizeof(expirations));
            updateClock();
        });
    }
    m_subscriptions.push_back(m_statusBar->timeFormatChanged.connect([this] { updateClock(); }));
    m_subscriptions.push_back(m_statusBar->layoutChanged.connect([this] { updateLayoutMode(); }));
    m_subscriptions.push_back(m_statusBar->panelChanged.connect([this] {
        if (!isPanel())
            return;
        if (m_panelTasks) {
            m_panelTasks->setTileSize(m_statusBar->panelIconSize() - (m_floating ? 0 : 4));
            m_panelTasks->setWindowTitles(m_statusBar->panelWindowTitles());
        }
        updatePanelGlass();
        updateFloating(true);
    }));

    Battery &battery = m_statusBar->battery();
    m_subscriptions.push_back(battery.changed.connect([this] { updateBattery(); }));
    m_subscriptions.push_back(battery.showPercentageChanged.connect([this] { updateBattery(); }));

    m_subscriptions.push_back(m_backdrop->changed.connect([this] { updateLook(); }));
    if (bus)
        setDeviceUsage(DeviceUsage::create(&m_shell->tasks()));
    m_subscriptions.push_back(m_shell->outputsChanged.connect([this] { updateGeometry(); }));
    // The theme's dark mode stands in while nothing behind the bar was judged.
    m_subscriptions.push_back(m_window->mappedChanged.connect([this] { updateLook(); }));

    updateGeometry();
    updateBattery();
    updateIndicators();
    updateLook();
    updateClock();
    m_window->show();
}

StatusBarWindow::~StatusBarWindow()
{
    m_updatesChanged.reset();
    if (m_updatesMenu)
        m_updatesMenu->close();
    m_floatAnimation.reset();
    m_panelLayoutSubscriptions.clear();
    m_subscriptions.clear();
    m_panelSubscriptions.clear();
    m_indicatorsChanged.reset();
    m_deviceUsageChanged.reset();
    m_backdrop->follow(nullptr);
    m_window.reset();
    if (m_clockWatch)
        core::mainLoop()->unwatch(m_clockWatch);
    if (m_clockFd >= 0)
        close(m_clockFd);
}

void StatusBarWindow::updateGeometry()
{
    // Always the primary output, all its width.
    const Shell::Output *output = m_shell->output({});
    if (!output)
        return;
    m_window->setOutput(output->name);
    if (isPanel()) {
        m_window->setGeometry(panelRect(m_floatProgress));
        // Reserved once on the edge; nothing while standing off it, as no
        // window is open then.
        m_window->setExclusiveZone(wm::ScreenEdge::Bottom, m_floating ? 0 : kPanelHeight);
        updatePanelGlass();
        return;
    }
    m_window->setGeometry(base::Rect(output->bounds.x(), output->bounds.y(), output->bounds.width(), kHeight));
    m_window->setExclusiveZone(wm::ScreenEdge::Top, kHeight);
}

void StatusBarWindow::setUpdates(UpdateMonitor *updates)
{
    m_updatesChanged.reset();
    m_updateMonitor = updates;
    if (updates)
        m_updatesChanged = updates->changed.connect([this] { updateUpdates(); });
    updateUpdates();
}

void StatusBarWindow::updateUpdates()
{
    const bool shown = m_updateMonitor
            && (m_updateMonitor->total() > 0 || m_updateMonitor->state() == UpdateMonitor::State::Ready);
    // A dot of red for a security update, as Pitaloka's danger.
    m_updates->setIndicatorColor(shown && m_updateMonitor->securityUpdates() > 0
                                         ? std::optional<ui::Color>(ui::accent::red)
                                         : std::nullopt);
    if (m_updates->isVisible() != shown) {
        if (!shown && m_updatesMenu)
            m_updatesMenu->close();
        m_updates->setVisible(shown);
    }
    layoutRow();
}

void StatusBarWindow::showUpdatesMenu()
{
    if (!m_updateMonitor)
        return;
    const bool open = m_updatesMenu && m_updatesMenu->isOpen();
    closePopups();
    if (open)
        return;
    using bayu::i18n::tr;
    using bayu::i18n::trn;
    UpdateMonitor *monitor = m_updateMonitor;
    m_updatesMenu = std::make_unique<ui::Menu>();
    if (monitor->state() == UpdateMonitor::State::Ready) {
        m_updatesMenu->addItem(tr("Restart to install the updates"), [monitor] {
            if (monitor->openSystemUpdates)
                monitor->openSystemUpdates();
        });
    } else if (monitor->systemUpdates() > 0) {
        const size_t index = m_updatesMenu->addItem(
                trn("{n} system update", "{n} system updates", uint64_t(monitor->systemUpdates())), [monitor] {
                    if (monitor->openSystemUpdates)
                        monitor->openSystemUpdates();
                });
        m_updatesMenu->setItemIcon(index, "system-software-update");
    }
    if (monitor->appUpdates() > 0) {
        const size_t index = m_updatesMenu->addItem(
                trn("{n} application update", "{n} application updates", uint64_t(monitor->appUpdates())), [monitor] {
                    if (monitor->openAppUpdates)
                        monitor->openAppUpdates();
                });
        // Bayu Store's own icon (bayu-store.desktop); the theme has no "bayu-store".
        m_updatesMenu->setItemIcon(index, "system-software-install");
    }
    m_updatesMenu->addSeparator();
    m_updatesMenu->addItem(tr("Check for Updates"), [monitor] { monitor->check(); });
    m_updates->setChecked(true);
    m_updatesMenu->onClosed = [this] { m_updates->setChecked(false); };
    m_updatesMenu->setAvailableGeometry(availableGeometry());
    m_updatesMenu->popupFor(m_updates);
}

bool StatusBarWindow::isPanel() const
{
    return m_panelShown;
}

void StatusBarWindow::setPanelSources(DockModel *applications, std::function<void()> toggleLauncher,
                                      std::function<render::GlassParams(double)> glass)
{
    m_applications = applications;
    m_toggleLauncher = std::move(toggleLauncher);
    m_glass = std::move(glass);
    TaskWindows &windows = m_shell->taskWindows();
    const auto changed = [this] { updateFloating(true); };
    m_panelLayoutSubscriptions.push_back(windows.windowAdded.connect([changed](uint64_t) { changed(); }));
    m_panelLayoutSubscriptions.push_back(windows.windowRemoved.connect([this, changed](uint64_t id) {
        m_sentTargets.erase(id);
        changed();
    }));
    m_panelLayoutSubscriptions.push_back(windows.windowChanged.connect([this, changed](uint64_t, uint32_t changes) {
        if (changes & (kStateChange | kSkipTaskbarChange))
            changed();
        if (changes & (kGeometryChange | kStateChange))
            updateMinimizeTargets();
    }));
    updateLayoutMode();
}

void StatusBarWindow::updateLayoutMode()
{
    const bool panel = m_statusBar->layout() == StatusBar::Layout::Panel && m_applications;
    if (panel == m_panelShown)
        return;
    closePopups();
    m_panelShown = panel;
    if (panel && !m_panelTasks) {
        auto tasks = std::make_unique<PanelTasks>(m_shell->taskWindows(), *m_applications, &m_shell->badges());
        m_panelTasks = tasks.get();
        m_panelTasks->onStart = [this] {
            closePopups();
            if (m_toggleLauncher)
                m_toggleLauncher();
        };
        m_panelTasks->onActivated = [this] { closePopups(); };
        m_panelTasks->availableGeometry = [this] { return availableGeometry(); };
        m_panelLayoutSubscriptions.push_back(m_panelTasks->targetsChanged.connect([this] { updateMinimizeTargets(); }));
        m_panelSlot->addChild(std::move(tasks));
    }
    m_panelSlot->setVisible(panel);
    m_menuSlot->setVisible(!panel);
    // The other edge's reservation goes with the other layout.
    m_window->clearExclusiveZone();
    m_sentTargets.clear();
    m_blurPlate = {};
    m_window->setBackgroundBlur(base::Rect(), 0);
    if (panel) {
        m_floating = m_statusBar->panelFloating() && !anyWindowOpen();
        m_floatProgress = m_floating ? 1 : 0;
        m_panelTasks->setTileSize(m_statusBar->panelIconSize() - (m_floating ? 0 : 4));
        m_panelTasks->setWindowTitles(m_statusBar->panelWindowTitles());
    }
    updateGeometry();
    updateLook();
    m_root->requestLayout();
    m_root->requestPaint();
    updateMinimizeTargets();
}

bool StatusBarWindow::anyWindowOpen() const
{
    const TaskWindows &windows = m_shell->taskWindows();
    for (uint64_t id : windows.windows()) {
        const TaskWindow *window = windows.window(id);
        if (window && window->isTaskListWindow() && !window->minimized && !window->hidden)
            return true;
    }
    return false;
}

void StatusBarWindow::updateFloating(bool animate)
{
    if (!isPanel())
        return;
    const bool floating = m_statusBar->panelFloating() && !anyWindowOpen();
    if (floating == m_floating)
        return;
    m_floating = floating;
    m_panelTasks->setTileSize(m_statusBar->panelIconSize() - (floating ? 0 : 4));
    // The reservation changes at once, so a window opening is placed in the
    // work area it will have.
    updateGeometry();
    if (!animate || !m_window->window()) {
        m_floatProgress = floating ? 1 : 0;
        updateGeometry();
        return;
    }
    if (!m_floatAnimation) {
        m_floatAnimation = std::make_unique<ui::Animation>(m_window->window());
        m_floatAnimation->setDuration(std::chrono::milliseconds(kPanelTransitionMs));
        m_floatAnimation->setEasing(ui::Animation::Easing::OutCubic);
    }
    m_floatAnimation->stop();
    m_floatFrom = m_floatProgress;
    m_floatAnimation->onUpdate = [this](double value) {
        const double target = m_floating ? 1 : 0;
        m_floatProgress = m_floatFrom + (target - m_floatFrom) * value;
        updateGeometry();
        m_root->requestPaint();
    };
    m_floatAnimation->start();
}

base::Rect StatusBarWindow::panelRect(double floating) const
{
    const Shell::Output *output = m_shell->output({});
    if (!output)
        return {};
    const base::Rect screen = output->bounds;
    const auto lerp = [floating](double edge, double off) { return int(std::lround(edge + (off - edge) * floating)); };
    const int side = lerp(0, kFloatingSide);
    const int height = lerp(kPanelHeight, kFloatingHeight);
    const int bottom = lerp(0, kFloatingBottom);
    return base::Rect(screen.x() + side, screen.y() + screen.height() - bottom - height, screen.width() - side * 2,
                      height);
}

void StatusBarWindow::paintPanel(ui::Painter &painter)
{
    // A 1 px --pit-border: all round while off the edge, along the top on it.
    const bool dark = ui::Theme::current().darkMode;
    const char *hex = dark ? PITALOKA_BORDER_DARK : PITALOKA_BORDER_LIGHT;
    const ui::Color border = ui::Color::rgb(uint32_t(std::strtoul(hex + 1, nullptr, 16))).withAlpha(0.9);
    const ui::RectF rect = m_root->localRect();
    const double radius = kFloatingRadius * m_floatProgress;
    if (m_floatProgress > 0)
        painter.strokeRoundedRect(rect, radius, border, 1);
    else
        painter.fillRect({0, 0, rect.width, 1}, border);
}

void StatusBarWindow::updatePanelGlass()
{
    if (!isPanel() || !m_glass)
        return;
    const base::Rect geometry = m_window->geometry();
    const double radius = kFloatingRadius * m_floatProgress;
    const ui::RectF rect {0, 0, double(geometry.width()), double(geometry.height())};
    ui::Path shape;
    shape.addRoundedRect(rect, radius, radius);
    m_window->setBackgroundBlur(base::Rect(0, 0, geometry.width(), geometry.height()), int(std::lround(radius)),
                                m_glass(m_statusBar->panelGlassTint()), shape);
}

void StatusBarWindow::updateMinimizeTargets()
{
    if (!isPanel() || !m_panelTasks)
        return;
    wm::Window *panel = m_window->wmWindow();
    if (!panel)
        return;
    wm::WindowManager *manager = m_shell->windowManager();
    for (const auto &[id, rect] : m_panelTasks->windowTargets()) {
        const base::Rect target(int(std::lround(rect.x)), int(std::lround(rect.y)), int(std::lround(rect.width)),
                                int(std::lround(rect.height)));
        if (m_sentTargets[id] == target)
            continue;
        m_sentTargets[id] = target;
        if (wm::Window *window = manager->windowById(id))
            window->setMinimizedGeometry(panel, target);
    }
}

void StatusBarWindow::setWallpaper(Wallpaper *wallpaper)
{
    m_backdrop->follow(wallpaper);
}

bool StatusBarWindow::lightContents() const
{
    return m_lightContents;
}

bool StatusBarWindow::panelOpen() const
{
    for (StatusBarPanels *panels : m_panels) {
        for (Panel panel : {Panel::ControlCenter, Panel::Network, Panel::Volume, Panel::Battery,
                            Panel::NotificationCenter}) {
            if (panels->hasPanel(panel) && panels->isOpen(panel))
                return true;
        }
    }
    return false;
}

void StatusBarWindow::updateLook()
{
    // What is behind the bar decides; the system's dark mode while nothing
    // has been judged.
    // The panel is on its own glass, which the theme's mode tints.
    const bool light = !isPanel() && m_backdrop->isValid() ? m_backdrop->useLightForeground()
                                                          : ui::Theme::current().darkMode;
    m_lightContents = light;
    if (m_menuBar)
        m_menuBar->barChanged();
    m_tray->barChanged();
    for (StatusIcon *icon : {m_updates, m_tiling, m_volume, m_network, m_battery, m_control})
        icon->updateLook();
    m_clock->updateLook();
    m_recording->updateLook();
    m_agentControl->updateLook();
    // The panel's glass follows the theme's mode.
    updatePanelGlass();
}

void StatusBarWindow::platePainted()
{
    updatePlateBlur();
}

static const BarItem *findPlate(const ui::View *view)
{
    if (!view->isVisible())
        return nullptr;
    if (const auto *item = dynamic_cast<const BarItem *>(view); item && item->isPlateShown())
        return item;
    for (const auto &child : view->children()) {
        if (const BarItem *item = findPlate(child.get()))
            return item;
    }
    return nullptr;
}

void StatusBarWindow::updatePlateBlur()
{
    // The panel's glass is the window's one blur.
    if (!m_window || !m_root || isPanel())
        return;
    // A surface has one blur, and one plate shows at a time: opening a panel
    // or a menu closes the others.
    const BarItem *item = findPlate(m_root);
    ui::RectF plate = item ? item->plateRect() : ui::RectF();
    if (plate.width > 0 && plate.height > 0) {
        const ui::PointF origin = item->mapToWindow({plate.x, plate.y});
        plate = {origin.x, origin.y, plate.width, plate.height};
    } else {
        plate = {};
    }
    if (plate == m_blurPlate)
        return;
    m_blurPlate = plate;
    if (plate.width <= 0) {
        m_window->setBackgroundBlur(base::Rect(), 0);
        return;
    }
    // The rectangle in whole logical pixels; the shape clips the blur to the
    // plate as drawn, which sits on fractional ones at most scales.
    const double radius = ui::Theme::current().smallRadius;
    const int left = int(std::floor(plate.x));
    const int top = int(std::floor(plate.y));
    const base::Rect rect(left, top, int(std::ceil(plate.x + plate.width)) - left,
                          int(std::ceil(plate.y + plate.height)) - top);
    ui::Path shape;
    shape.addRoundedRect(plate, radius, radius);
    m_window->setBackgroundBlur(rect, int(std::lround(radius)), {}, shape);
}

void StatusBarWindow::updateBattery()
{
    const Battery &battery = m_statusBar->battery();
    m_battery->showBattery(battery, battery.showPercentage());
    layoutRow();
}

void StatusBarWindow::setIndicators(StatusBarIndicators *indicators)
{
    m_indicatorsChanged.reset();
    m_indicators = indicators;
    if (m_indicators)
        m_indicatorsChanged = m_indicators->changed.connect([this] { updateIndicators(); });
    updateIndicators();
}

void StatusBarWindow::updateIndicators()
{
    const StatusBarIndicators::Audio audio = m_indicators ? m_indicators->audio() : StatusBarIndicators::Audio();
    m_volume->setVisible(audio.hasSink);
    const double level = audio.hasSink ? audio.volume * 100.0 : -1;
    const char *volumeIcon = level <= 0 ? "audio-volume-muted-symbolic"
            : level <= 25              ? "audio-volume-low-symbolic"
            : level <= 75              ? "audio-volume-medium-symbolic"
                                       : "audio-volume-high-symbolic";
    m_volume->setIconName(audio.hasSink && !audio.muted ? volumeIcon : "audio-volume-muted-symbolic");

    m_network->showNetwork(m_indicators ? m_indicators->network() : StatusBarIndicators::Network());

    layoutRow();
}

void StatusBarWindow::setDeviceUsage(std::unique_ptr<DeviceUsage> usage)
{
    m_deviceUsageChanged.reset();
    m_deviceUsage = std::move(usage);
    const auto update = [this] {
        // Camera before microphone before system audio.
        const DeviceUsage *usage = m_deviceUsage.get();
        m_control->setIndicatorColor(usage && usage->camera()            ? std::optional<ui::Color>(ui::accent::green)
                                     : usage && usage->microphone()    ? std::optional<ui::Color>(ui::accent::orange)
                                     : usage && usage->systemAudio()   ? std::optional<ui::Color>(ui::accent::purple)
                                                                       : std::nullopt);
        layoutRow();
    };
    if (m_deviceUsage)
        m_deviceUsageChanged = m_deviceUsage->usesChanged.connect(update);
    update();
}

void StatusBarWindow::setTiling(bool tiled)
{
    m_tiling->setIconName(tiled ? "tiling" : "tiling-off");
}

void StatusBarWindow::setRecording(bool recording)
{
    m_recording->setVisible(recording);
    layoutRow();
}

void StatusBarWindow::setAgentControl(bool active)
{
    m_agentControl->setVisible(active);
    layoutRow();
}

void StatusBarWindow::layoutRow()
{
    // Not before every item exists.
    if (!m_clock)
        return;
    const auto preferred = [](ui::View *view, double width) {
        ui::LayoutHints hints;
        hints.preferredWidth = width;
        hints.fillHeight = true;
        if (view->layoutHints().preferredWidth != width || !view->layoutHints().fillHeight)
            view->setLayoutHints(hints);
    };
    preferred(m_recording, m_recording->preferredWidth());
    preferred(m_agentControl, m_agentControl->preferredWidth());
    m_tray->setVisible(m_tray->count() > 0);
    preferred(m_tray, m_tray->preferredWidth());
    preferred(m_updates, m_updates->preferredWidth());
    for (StatusIcon *icon : {m_tiling, m_volume, m_network, m_battery, m_control})
        preferred(icon, icon->preferredWidth());
    preferred(m_clock, m_clock->preferredWidth());
    m_root->requestLayout();
}

void StatusBarWindow::updateClock()
{
    const auto now = std::chrono::system_clock::now();
    const int64_t seconds = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    const base::Locale locale(m_statusBar->timeLocale());
    m_clock->setText(locale.toString(base::DateTime::fromSecondsSinceEpoch(seconds), m_statusBar->timeFormat()));
    armClock();
}

void StatusBarWindow::armClock()
{
    if (m_clockFd < 0)
        return;
    timespec now {};
    clock_gettime(CLOCK_REALTIME, &now);
    const time_t period = clock_format::showsSeconds(m_statusBar->timeFormat()) ? 1 : 60;
    itimerspec spec {};
    spec.it_value.tv_sec = (now.tv_sec / period + 1) * period;
    timerfd_settime(m_clockFd, TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET, &spec, nullptr);
}

void StatusBarWindow::addPanels(StatusBarPanels *panels)
{
    if (!panels || std::find(m_panels.begin(), m_panels.end(), panels) != m_panels.end())
        return;
    m_panels.push_back(panels);
    m_panelSubscriptions.push_back(panels->panelsChanged.connect([this] { updateChecked(); }));
    updateChecked();
}

void StatusBarWindow::removePanels(StatusBarPanels *panels)
{
    const auto it = std::find(m_panels.begin(), m_panels.end(), panels);
    if (it == m_panels.end())
        return;
    m_panelSubscriptions.erase(m_panelSubscriptions.begin() + (it - m_panels.begin()));
    m_panels.erase(it);
    updateChecked();
}

StatusBarPanels *StatusBarWindow::panelsFor(Panel panel) const
{
    for (StatusBarPanels *panels : m_panels) {
        if (panels->hasPanel(panel))
            return panels;
    }
    return nullptr;
}

BarItem *StatusBarWindow::itemFor(Panel panel) const
{
    switch (panel) {
    case Panel::ControlCenter:
        return m_control;
    case Panel::Network:
        return m_network;
    case Panel::Volume:
        return m_volume;
    case Panel::Battery:
        return m_battery;
    case Panel::NotificationCenter:
        return m_clock;
    }
    return nullptr;
}

void StatusBarWindow::updateChecked()
{
    for (Panel panel : {Panel::ControlCenter, Panel::Network, Panel::Volume, Panel::Battery,
                        Panel::NotificationCenter}) {
        StatusBarPanels *panels = panelsFor(panel);
        itemFor(panel)->setChecked(panels && panels->isOpen(panel));
    }
}

void StatusBarWindow::togglePanel(Panel panel)
{
    StatusBarPanels *panels = panelsFor(panel);
    if (!panels)
        return;
    const bool wasOpen = panels->isOpen(panel);
    closePopups();
    if (wasOpen)
        return;
    BarItem *item = itemFor(panel);
    const ui::PointF origin = item->mapToWindow({0, 0});
    const base::Rect anchor(int(std::lround(origin.x)), int(std::lround(origin.y)),
                            int(std::lround(item->geometry().width)), int(std::lround(item->geometry().height)));
    panels->openPanel(panel, m_window.get(), anchor);
}

void StatusBarWindow::closePanels()
{
    for (StatusBarPanels *panels : m_panels)
        panels->closePanels();
}

void StatusBarWindow::closePopups()
{
    closePanels();
    if (m_menuBar)
        m_menuBar->closeMenu();
    m_tray->closeMenu();
    if (m_updatesMenu)
        m_updatesMenu->close();
}

StatusBarMenuBarHost StatusBarWindow::menuBarHost()
{
    StatusBarMenuBarHost host;
    host.bar = this;
    host.statusBar = m_statusBar.get();
    host.closePanels = [this] { closePanels(); };
    host.closeTrayMenu = [this] { m_tray->closeMenu(); };
    host.availableGeometry = [this] { return availableGeometry(); };
    return host;
}

void StatusBarWindow::setMenuBar(std::unique_ptr<StatusBarMenuBar> menuBar)
{
    if (m_menuBar) {
        m_menuBar->closeMenu();
        m_menuBar->setVisible(false);
        std::shared_ptr<ui::View> retired(m_menuSlot->takeChild(m_menuBar));
        core::mainLoop()->post([retired] {});
        m_menuBar = nullptr;
    }
    if (!menuBar)
        return;
    m_menuBar = menuBar.get();
    m_menuSlot->addChild(std::move(menuBar));
    m_menuBar->barChanged();
    m_menuSlot->requestLayout();
}

ui::RectF StatusBarWindow::availableGeometry() const
{
    // The menus stay inside the work area the shell reserves.
    const Shell::Output *output = m_shell->output(m_window->outputName());
    if (!output)
        return {};
    const base::Rect area = m_shell->workArea(*output);
    const base::Point origin = m_window->geometry().topLeft();
    return {double(area.x() - origin.x()), double(area.y() - origin.y()), double(area.width()),
            double(area.height())};
}

} // namespace bayu::shell
