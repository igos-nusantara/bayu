// SPDX-License-Identifier: BSD-3-Clause

#ifndef BAYU_CORE_BAR_APPLETS_H_
#define BAYU_CORE_BAR_APPLETS_H_

#include <wayland-server-core.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/geometry.h"
#include "base/signal.h"
#include "wm/window_manager.h"
#include "wm/window_observer.h"

namespace bayu::base {
class Process;
}

namespace bayu::compositor {
class Surface;
}

namespace bayu::shell {
class StatusBarWindow;
}

namespace bayu::wayland {
class InputMethodLauncher;
}

namespace bayu::core {

// Applets in the bar (#167): programs that draw one item of it, such as
// Bayu's applets forked from COSMIC's. Each runs on a Wayland connection of
// its own that the compositor makes, as the input method does, so only the
// applets' windows are taken into the bar: as panels that take no keyboard,
// lying on the bar's slots, out of the dock, the task list and the switcher.
// Their popups are ordinary xdg_popups and open from the bar.
//
// They are libcosmic applets: the panel is described to them in
// COSMIC_PANEL_* variables, so they size themselves for the bar and draw
// light or dark as its contents, and bayu-applets-theme writes the toolkit
// theme's colours, radii, font and icons as the COSMIC theme they read, in
// their own XDG_CONFIG_HOME (~/.config/bayu/applets). A change of any of it
// starts them again.
//
// Which ones, in order: Applets/Enabled in shell.conf, as desktop file ids.
class BarApplets final : public wm::WindowObserver
{
public:
    BarApplets(wl_display *display, wm::WindowManager *windows, shell::StatusBarWindow *bar,
               const std::string &settingsPath);
    ~BarApplets() override;
    BarApplets(const BarApplets &) = delete;
    BarApplets &operator=(const BarApplets &) = delete;

    // For WaylandXdgShell::setToplevelPolicy: an applet's toplevel is a
    // panel on its slot; anything else is left alone.
    std::optional<wm::ShellWindowMetadata> toplevelFor(wl_client *client, compositor::Surface *surface);

    // The COSMIC_PANEL_* entries for the bar as it is now.
    static std::vector<std::string> panelVariables(int height, bool bottom, bool lightContents);
    // bayu-applets-theme's arguments for the toolkit theme now.
    static std::vector<std::string> themeArguments();

    // wm::WindowObserver:
    void onWindowBoundsChanged(wm::Window *window, const base::Rect &oldBounds) override;
    void onWindowDestroying(wm::Window *window) override;

private:
    struct Applet;

    Applet *appletFor(const wm::Window *window) const;
    void windowAdded(wm::Window *window);
    void updateWidths();
    void place(const std::vector<base::Rect> &slots);
    // Writes the theme when it changed, then starts the applets again when
    // it or the panel did.
    void refresh();
    void startAll();

    wl_display *m_display;
    wm::WindowManager *m_windows;
    shell::StatusBarWindow *m_bar;
    std::vector<std::unique_ptr<Applet>> m_applets;
    std::vector<std::string> m_variables;
    std::vector<std::string> m_themeArguments;
    std::unique_ptr<base::Process> m_themeWriter;
    bool m_refreshAgain = false;
    std::vector<base::Rect> m_slots;
    base::Subscription m_added;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};

} // namespace bayu::core

#endif // BAYU_CORE_BAR_APPLETS_H_
