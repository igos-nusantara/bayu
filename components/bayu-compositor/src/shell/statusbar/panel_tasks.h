// SPDX-License-Identifier: BSD-3-Clause

#ifndef BAYU_SHELL_STATUSBAR_PANEL_TASKS_H_
#define BAYU_SHELL_STATUSBAR_PANEL_TASKS_H_

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "base/signal.h"
#include "ui/view.h"

namespace bayu::ui {
class Menu;
}

namespace bayu::shell {

class ApplicationBadges;
class DockModel;
class TaskWindows;

// The panel's left side, in the panel layout (Shell/Layout): the Start
// button, then the applications. As icons (the default), the dock's pinned
// and running applications, a dot for each window under the running ones
// (up to three, as the dock), the active one
// selected; with titles (Panel/WindowTitles), the pinned applications and a
// button for each window. A right click gives the dock's menu: the
// application's windows, Open, Pin or Unpin, and Quit. Pitaloka's metrics
// and colours.
class PanelTasks : public ui::View
{
public:
    // `badges`, the counts applications put on their icons, may be null.
    PanelTasks(TaskWindows &windows, DockModel &applications, ApplicationBadges *badges = nullptr);
    ~PanelTasks() override;

    // The Start button.
    std::function<void()> onStart;
    // An application or a window was clicked, or its menu opens, so the
    // bar's popups close.
    std::function<void()> onActivated;
    // Where the menus may go, in the window's coordinates: the work area.
    std::function<ui::RectF()> availableGeometry;

    // The tile the icons sit on: Panel/IconSize floating, 4 less docked.
    void setTileSize(double size);
    // A button for each window, with its title, instead of the icons.
    void setWindowTitles(bool titles);
    bool windowTitles() const { return m_windowTitles; }
    // Where each window minimizes to, in the window's coordinates: its
    // button's icon, or its application's.
    std::map<uint64_t, ui::RectF> windowTargets() const;
    // The buttons moved.
    base::Signal<> targetsChanged;

    void closeMenu();

    // For tests: the applications shown as icons, the pinned ones before the
    // windows' buttons, and the windows' buttons.
    std::vector<std::string> applicationButtons() const;
    std::vector<uint64_t> windowButtons() const;
    // The dots under an application's icon; 0 without one.
    int applicationDots(const std::string &application) const;

protected:
    void layout() override;
    void paint(ui::Painter &painter) override;

private:
    class Button;
    class StartButton;
    class AppButton;
    class WindowButton;

    void rebuild();
    void syncApplications();
    void syncWindows();
    void updateWindow(uint64_t id);
    bool listed(uint64_t id) const;
    void popupMenu(Button *button, const std::string &application);

    TaskWindows &m_windows;
    DockModel &m_applications;
    ApplicationBadges *m_badges = nullptr;
    double m_tileSize = 36;
    bool m_windowTitles = false;
    StartButton *m_start = nullptr;
    std::vector<AppButton *> m_apps;
    std::vector<WindowButton *> m_windowButtons;
    // Where the dividers are drawn, after the Start button and the pins.
    std::vector<double> m_dividers;
    std::unique_ptr<ui::Menu> m_menu;
    Button *m_menuButton = nullptr;
    std::vector<base::Subscription> m_subscriptions;
};

} // namespace bayu::shell

#endif // BAYU_SHELL_STATUSBAR_PANEL_TASKS_H_
