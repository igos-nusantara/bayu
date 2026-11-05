// SPDX-License-Identifier: BSD-3-Clause

#include "core/bar_applets.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>
#include <fstream>
#include <sstream>

#include "base/desktop_entry.h"
#include "base/event_loop.h"
#include "base/paths.h"
#include "base/process.h"
#include "base/settings.h"
#include "core/logging.h"
#include "core/main_loop.h"
#include "core/process_environment.h"
#include "shell/statusbar/status_bar_window.h"
#include "ui/theme.h"
#include "wayland/input_method_launcher.h"
#include "wm/window.h"

namespace bayu::core {

namespace {

// The applet's desktop file, where the XDG data directories have it.
std::optional<base::DesktopEntry> findApplet(const std::string &id)
{
    std::vector<std::string> directories {base::paths::dataHome()};
    for (const std::string &directory : base::paths::dataDirectories())
        directories.push_back(directory);
    for (const std::string &directory : directories) {
        const std::string path = directory + "/applications/" + id + ".desktop";
        std::ifstream file(path);
        if (!file)
            continue;
        std::stringstream contents;
        contents << file.rdbuf();
        return base::DesktopEntry::parse(id, path, contents.str(), {});
    }
    return std::nullopt;
}

std::string hex(const ui::Color &color)
{
    char text[8];
    snprintf(text, sizeof text, "%02x%02x%02x", color.r, color.g, color.b);
    return text;
}

std::string configHome()
{
    return base::paths::configHome() + "/bayu/applets";
}

// The compositor's environment with `variables`, NAME=value entries, in
// place of its own of the same names.
std::vector<std::string> environmentWith(const std::vector<std::string> &variables)
{
    std::vector<std::string> environment = childEnvironment();
    for (const std::string &variable : variables) {
        const std::string prefix = variable.substr(0, variable.find('=') + 1);
        std::erase_if(environment, [&](const std::string &entry) { return entry.starts_with(prefix); });
        environment.push_back(variable);
    }
    return environment;
}

} // namespace

struct BarApplets::Applet {
    std::string id;
    std::vector<std::string> command;
    std::unique_ptr<wayland::InputMethodLauncher> launcher;
    compositor::Surface *surface = nullptr;
    wm::Window *window = nullptr;
};

BarApplets::BarApplets(wl_display *display, wm::WindowManager *windows, shell::StatusBarWindow *bar,
                       const std::string &settingsPath)
    : m_display(display)
    , m_windows(windows)
    , m_bar(bar)
{
    for (const std::string &id : base::Settings(settingsPath).stringListValue("Applets/Enabled")) {
        const auto entry = findApplet(id);
        if (!entry || entry->command().empty()) {
            CF_WARNING(cfCompositor) << "No applet" << id;
            continue;
        }
        auto applet = std::make_unique<Applet>();
        applet->id = id;
        applet->command = entry->command();
        applet->launcher = std::make_unique<wayland::InputMethodLauncher>(m_display);
        m_applets.push_back(std::move(applet));
    }
    m_added = m_windows->windowAdded.connect([this](wm::Window *window) { windowAdded(window); });
    m_bar->onAppletsPlaced = [this](const std::vector<base::Rect> &slots) { place(slots); };
    m_bar->onLookChanged = [this] { refresh(); };
    refresh();
}

BarApplets::~BarApplets()
{
    m_bar->onAppletsPlaced = nullptr;
    m_bar->onLookChanged = nullptr;
    for (const auto &applet : m_applets) {
        if (applet->window)
            applet->window->removeObserver(this);
    }
}

std::vector<std::string> BarApplets::panelVariables(int height, bool bottom, bool lightContents)
{
    // A custom size is the applet's height: half of it the icon, a quarter
    // each side the padding, so 32 gives the bar's 16 pixel icons.
    return {
        "COSMIC_PANEL_NAME=Panel",
        "COSMIC_PANEL_SIZE=Custom(" + std::to_string(height) + ")",
        std::string("COSMIC_PANEL_ANCHOR=") + (bottom ? "Bottom" : "Top"),
        // Light contents are on a dark backdrop.
        std::string("COSMIC_PANEL_BACKGROUND=") + (lightContents ? "Dark" : "Light"),
        "COSMIC_PANEL_SPACING=0",
    };
}

std::optional<wm::ShellWindowMetadata> BarApplets::toplevelFor(wl_client *client, compositor::Surface *surface)
{
    for (const auto &applet : m_applets) {
        if (!applet->launcher->isInputMethod(client))
            continue;
        applet->surface = surface;
        wm::ShellWindowMetadata metadata;
        metadata.role = wm::WindowRole::Panel;
        metadata.focusPolicy = wm::FocusPolicy::NoFocus;
        // Off the screen until it has a slot of its width.
        metadata.hasPosition = true;
        metadata.position = base::Point(-10000, -10000);
        return metadata;
    }
    return std::nullopt;
}

BarApplets::Applet *BarApplets::appletFor(const wm::Window *window) const
{
    for (const auto &applet : m_applets) {
        if (applet->window == window)
            return applet.get();
    }
    return nullptr;
}

void BarApplets::windowAdded(wm::Window *window)
{
    for (const auto &applet : m_applets) {
        if (applet->surface && window->rootSurface() == applet->surface && !window->isPopup()) {
            applet->window = window;
            window->addObserver(this);
            updateWidths();
            return;
        }
    }
}

void BarApplets::onWindowBoundsChanged(wm::Window *window, const base::Rect &oldBounds)
{
    if (appletFor(window) && window->bounds().size() != oldBounds.size())
        updateWidths();
}

void BarApplets::onWindowDestroying(wm::Window *window)
{
    if (Applet *applet = appletFor(window)) {
        window->removeObserver(this);
        applet->window = nullptr;
        applet->surface = nullptr;
        updateWidths();
    }
}

void BarApplets::updateWidths()
{
    std::vector<int> widths;
    for (const auto &applet : m_applets) {
        if (applet->window)
            widths.push_back(applet->window->bounds().width());
    }
    m_bar->setAppletWidths(widths);
    // The same widths lay nothing out again; the windows still need their
    // slots.
    place(m_slots);
}

void BarApplets::place(const std::vector<base::Rect> &slots)
{
    m_slots = slots;
    size_t slot = 0;
    for (const auto &applet : m_applets) {
        if (!applet->window || slot >= slots.size())
            continue;
        const base::Rect &rect = slots[slot++];
        const base::Size size = applet->window->bounds().size();
        applet->window->setClientPosition(
                base::Point(rect.x(), rect.y() + (rect.height() - size.height()) / 2));
    }
}

std::vector<std::string> BarApplets::themeArguments()
{
    const ui::Theme &theme = ui::Theme::current();
    const ui::Theme light = theme.darkMode ? ui::Theme::light() : theme;
    const ui::Theme dark = theme.darkMode ? theme : ui::Theme::dark();
    const auto radius = [](double value) { return std::to_string(int(std::lround(value))); };
    return {
        std::string("mode=") + (theme.darkMode ? "dark" : "light"),
        "accent=" + hex(theme.highlight),
        "light-bg=" + hex(light.background),
        "light-text=" + hex(light.text),
        "dark-bg=" + hex(dark.background),
        "dark-text=" + hex(dark.text),
        "radii=" + radius(theme.smallRadius) + "," + radius(theme.mediumRadius) + ","
                + radius(theme.bigRadius) + "," + radius(theme.hugeRadius),
        "font=" + theme.fontFamily,
        "icons=" + theme.iconTheme,
    };
}

void BarApplets::refresh()
{
    if (m_applets.empty())
        return;
    // One write at a time; the last change is written after it.
    if (m_themeWriter) {
        m_refreshAgain = true;
        return;
    }
    auto variables = panelVariables(m_bar->barHeight(), m_bar->isPanel(), m_bar->lightContents());
    auto arguments = themeArguments();
    if (variables == m_variables && arguments == m_themeArguments)
        return;
    m_variables = std::move(variables);
    if (arguments == m_themeArguments) {
        startAll();
        return;
    }
    m_themeArguments = std::move(arguments);
    m_themeWriter = std::make_unique<base::Process>();
    m_themeWriter->setEnvironment(environmentWith({"XDG_CONFIG_HOME=" + configHome()}));
    m_themeWriter->onFinished = [this](int code, bool) {
        if (code != 0)
            CF_WARNING(cfCompositor) << "bayu-applets-theme exited with" << code;
        // Not inside the process's own callback.
        core::mainLoop()->post([this, alive = std::weak_ptr<bool>(m_alive),
                                writer = std::shared_ptr<base::Process>(std::move(m_themeWriter))] {
            if (!alive.lock())
                return;
            startAll();
            if (std::exchange(m_refreshAgain, false))
                refresh();
        });
    };
    if (!m_themeWriter->start("bayu-applets-theme", m_themeArguments)) {
        CF_WARNING(cfCompositor) << "Cannot start bayu-applets-theme";
        m_themeWriter.reset();
        startAll();
    }
}

void BarApplets::startAll()
{
    std::vector<std::string> variables = m_variables;
    variables.push_back("XDG_CONFIG_HOME=" + configHome());
    const std::vector<std::string> environment = environmentWith(variables);
    for (const auto &applet : m_applets) {
        applet->launcher->setEnvironment(environment);
        if (applet->launcher->isRunning())
            applet->launcher->restart();
        else
            applet->launcher->setCommand(applet->command);
    }
}

} // namespace bayu::core
