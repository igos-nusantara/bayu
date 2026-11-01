// SPDX-License-Identifier: BSD-3-Clause

#include "shell/statusbar/panel_tasks.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <utility>

#include "base/i18n.h"
#include "core/main_loop.h"
#include "shell/common/application_badges.h"
#include "shell/common/count_badge.h"
#include "shell/common/task_windows.h"
#include "shell/dock/dock_model.h"
#include "shell/statusbar/pitaloka.h"
#include "ui/bubble_shape.h"
#include "ui/icon.h"
#include "ui/label.h"
#include "ui/menu.h"
#include "ui/painter.h"
#include "ui/theme.h"

namespace bayu::shell {

using bayu::i18n::tr;

namespace {

// A Pitaloka colour token in the theme's mode.
ui::Color token(const char *light, const char *dark)
{
    const char *hex = ui::Theme::current().darkMode ? dark : light;
    return ui::Color::rgb(uint32_t(std::strtoul(hex + 1, nullptr, 16)));
}
#define PIT_COLOR(name) token(PITALOKA_##name##_LIGHT, PITALOKA_##name##_DARK)

// The accent is the desktop's (Pitaloka's --pit-accent by default); hovered,
// a step darker as --pit-accent-hover.
ui::Color accent(bool hovered)
{
    const ui::Color color = ui::Theme::current().highlight;
    if (!hovered)
        return color;
    const auto darker = [](uint8_t c) { return uint8_t(std::lround(c * 0.9)); };
    return {darker(color.r), darker(color.g), darker(color.b), color.a};
}

constexpr double kGap = PITALOKA_SPACE_1;
constexpr double kDividerGap = PITALOKA_SPACE_2;
constexpr double kMaxWindowWidth = 200;
constexpr double kDot = 5;
constexpr double kFontSize = 13;
// The theme size the applications' icons are looked up at.
constexpr int kAppArtwork = 48;

// Takes the button out now and lets it go on the next turn: its own click
// may be what changed the list.
void retire(ui::View *parent, ui::View *child)
{
    std::shared_ptr<ui::View> retired(parent->takeChild(child));
    retired->setVisible(false);
    core::mainLoop()->post([retired] {});
}

} // namespace

// A clickable tile of the panel: hovered, pressed, selected and focused by
// Pitaloka's States. A right click asks for its menu.
class PanelTasks::Button : public ui::View
{
public:
    Button() { setTracksHover(true); }

    std::function<void(const ui::PointerEvent &)> onClicked;
    std::function<void()> onMenuRequested;
    void setSelected(bool selected)
    {
        if (m_selected != selected) {
            m_selected = selected;
            requestPaint();
        }
    }
    void setMenuOpen(bool open)
    {
        if (m_menuOpen != open) {
            m_menuOpen = open;
            requestPaint();
        }
    }
    bool acceptsPointer() const override { return true; }
    bool isSelected() const { return m_selected; }

protected:
    bool m_selected = false;
    bool m_pressed = false;
    bool m_menuOpen = false;

    void hoverChanged() override { requestPaint(); }
    void pointerPress(const ui::PointerEvent &event) override
    {
        if (event.button == ui::MouseButton::Right) {
            if (onMenuRequested)
                onMenuRequested();
            return;
        }
        m_pressed = true;
        requestPaint();
    }
    void pointerRelease(const ui::PointerEvent &event) override
    {
        if (event.button == ui::MouseButton::Right)
            return;
        const bool inside = localRect().contains(event.position);
        m_pressed = false;
        requestPaint();
        if (inside && onClicked)
            onClicked(event);
    }

    // The plate: --pit-bg-selected selected, pressed or with its menu open,
    // --pit-bg-hover hovered or `resting`.
    void paintPlate(ui::Painter &painter, bool resting = false)
    {
        std::optional<ui::Color> plate;
        if (m_selected || m_pressed || m_menuOpen)
            plate = PIT_COLOR(BG_SELECTED);
        else if (isHovered() || resting)
            plate = PIT_COLOR(BG_HOVER);
        if (plate)
            painter.fillRoundedRect(localRect(), PITALOKA_RADIUS_CONTROL, *plate);
    }
};

// Start: the launcher, a square in the accent with the launcher's grid.
class PanelTasks::StartButton final : public PanelTasks::Button
{
protected:
    static constexpr double kGlyph = 16;

    void paint(ui::Painter &painter) override
    {
        painter.fillRoundedRect(localRect(), PITALOKA_RADIUS_CONTROL, accent(isHovered() || m_pressed));
        // Four rounded squares, centred.
        const ui::Color ink = token(PITALOKA_TEXT_ON_ACCENT_LIGHT, PITALOKA_TEXT_ON_ACCENT_DARK);
        const double left = std::round((localRect().width - kGlyph) / 2);
        const double top = std::round((localRect().height - kGlyph) / 2);
        const double cell = 7;
        for (int row = 0; row < 2; ++row) {
            for (int column = 0; column < 2; ++column)
                painter.strokeRoundedRect({left + column * 9 + 0.5, top + row * 9 + 0.5, cell - 1, cell - 1}, 1.5, ink,
                                          2);
        }
    }
};

// An application: its icon on a tile. The panel draws its dots below the tile
// while it runs, one for each window, up to two, as the dock's.
class PanelTasks::AppButton final : public PanelTasks::Button
{
public:
    explicit AppButton(const DockItem &item)
        : id(item.id)
    {
        m_icon = addChild<ui::ThemeIcon>(std::string(), 24);
        // The application artwork at any size: the theme has small action
        // glyphs under some applications' names (utilities-terminal at 24).
        m_icon->setThemeSize(kAppArtwork);
        m_badge = addChild<CountBadge>();
        update(item);
    }

    void setCount(int64_t count) { m_badge->setCount(count); }

    std::string id;

    void update(const DockItem &item)
    {
        m_icon->setIconName(item.iconName.empty() ? "application-x-desktop" : item.iconName);
        m_running = !item.windows.empty();
        m_dots = std::clamp(int(item.windows.size()), 1, 2);
        setSelected(item.isActive);
        requestPaint();
    }
    int dots() const { return m_running ? m_dots : 0; }
    // The icon, in the window's coordinates.
    ui::RectF iconTarget() const
    {
        const ui::PointF origin = mapToWindow({m_icon->geometry().x, m_icon->geometry().y});
        return {origin.x, origin.y, m_icon->geometry().width, m_icon->geometry().height};
    }

protected:
    void layout() override
    {
        const double size = std::round(localRect().height * 0.66);
        m_icon->setSize(size);
        m_icon->setGeometry({std::round((localRect().width - size) / 2), std::round((localRect().height - size) / 2),
                             size, size});
        m_badge->setIconRect(m_icon->geometry());
    }
    void paint(ui::Painter &painter) override { paintPlate(painter); }

private:
    ui::ThemeIcon *m_icon = nullptr;
    CountBadge *m_badge = nullptr;
    bool m_running = false;
    int m_dots = 1;
};

// A window: a dot, its application's icon and its title.
class PanelTasks::WindowButton final : public PanelTasks::Button
{
public:
    explicit WindowButton(uint64_t window)
        : id(window)
    {
        m_icon = addChild<ui::ThemeIcon>(std::string(), kIcon);
        m_icon->setThemeSize(kAppArtwork);
        m_title = addChild<ui::Label>();
        m_title->setFontSize(kFontSize);
        m_title->setElide(ui::Label::Elide::Right);
    }

    uint64_t id;

    void update(const std::string &icon, const std::string &title, bool active)
    {
        m_icon->setIconName(icon.empty() ? "application-x-desktop" : icon);
        m_title->setText(title);
        m_title->setWeight(active ? 500 : 400);
        setSelected(active);
        requestPaint();
    }

    double preferredWidth() const
    {
        // Whole pixels up: the layout floors the width, which would elide the title.
        return std::min(kMaxWindowWidth, kTextStart + std::ceil(m_title->sizeHint().width) + 1 + PITALOKA_SPACE_3);
    }
    // The icon, in the window's coordinates.
    ui::RectF iconTarget() const
    {
        const ui::PointF origin = mapToWindow({m_icon->geometry().x, m_icon->geometry().y});
        return {origin.x, origin.y, kIcon, kIcon};
    }

protected:
    static constexpr double kIcon = 18;
    static constexpr double kTextStart = 10 + kDot + 8 + kIcon + 8;

    void layout() override
    {
        const double height = localRect().height;
        m_icon->setGeometry({10 + kDot + 8, std::round((height - kIcon) / 2), kIcon, kIcon});
        const ui::SizeF size = m_title->sizeHint();
        m_title->setGeometry({kTextStart, (height - size.height) / 2,
                              std::max(0.0, localRect().width - kTextStart - PITALOKA_SPACE_3), size.height});
    }

    void paint(ui::Painter &painter) override
    {
        // Every window rests on --pit-bg-hover; the active one is selected.
        paintPlate(painter, true);
        const ui::Color dot = m_selected ? accent(false) : PIT_COLOR(TEXT_TERTIARY);
        painter.fillEllipse({10, (localRect().height - kDot) / 2, kDot, kDot}, dot);
    }

private:
    ui::ThemeIcon *m_icon = nullptr;
    ui::Label *m_title = nullptr;
};

PanelTasks::PanelTasks(TaskWindows &windows, DockModel &applications, ApplicationBadges *badges)
    : m_windows(windows)
    , m_applications(applications)
    , m_badges(badges)
{
    if (m_badges) {
        m_subscriptions.push_back(m_badges->changed.connect([this] {
            for (AppButton *button : m_apps)
                button->setCount(m_badges->count(button->id));
        }));
    }
    m_start = addChild<StartButton>();
    m_start->onClicked = [this](const ui::PointerEvent &) {
        if (onStart)
            onStart();
    };

    const auto applicationsChanged = [this](auto &&...) { syncApplications(); };
    m_subscriptions.push_back(m_applications.rowsInserted.connect(applicationsChanged));
    m_subscriptions.push_back(m_applications.rowsRemoved.connect(applicationsChanged));
    m_subscriptions.push_back(m_applications.rowMoved.connect(applicationsChanged));
    m_subscriptions.push_back(m_applications.dataChanged.connect(applicationsChanged));
    m_subscriptions.push_back(m_applications.modelReset.connect(applicationsChanged));
    m_subscriptions.push_back(m_applications.sectionsChanged.connect(applicationsChanged));
    m_subscriptions.push_back(m_applications.loaded.connect(applicationsChanged));

    m_subscriptions.push_back(m_windows.windowAdded.connect([this](uint64_t) { syncWindows(); }));
    m_subscriptions.push_back(m_windows.windowRemoved.connect([this](uint64_t) { syncWindows(); }));
    m_subscriptions.push_back(m_windows.windowChanged.connect([this](uint64_t id, uint32_t changes) {
        if (changes & (kSkipTaskbarChange | kAppIdChange | kParentChange))
            syncWindows();
        else if (changes & (kTitleChange | kStateChange))
            updateWindow(id);
    }));
    m_subscriptions.push_back(m_windows.activeWindowChanged.connect([this](uint64_t) {
        for (WindowButton *button : m_windowButtons)
            updateWindow(button->id);
    }));

    rebuild();
}

PanelTasks::~PanelTasks()
{
    m_subscriptions.clear();
    closeMenu();
}

void PanelTasks::setTileSize(double size)
{
    if (m_tileSize == size)
        return;
    m_tileSize = size;
    requestLayout();
}

void PanelTasks::setWindowTitles(bool titles)
{
    if (m_windowTitles == titles)
        return;
    m_windowTitles = titles;
    rebuild();
}

bool PanelTasks::listed(uint64_t id) const
{
    const TaskWindow *window = m_windows.window(id);
    return window && window->isTaskListWindow() && !window->parent;
}

void PanelTasks::rebuild()
{
    closeMenu();
    for (AppButton *button : std::exchange(m_apps, {}))
        retire(this, button);
    for (WindowButton *button : std::exchange(m_windowButtons, {}))
        retire(this, button);
    syncApplications();
    syncWindows();
}

void PanelTasks::syncApplications()
{
    // As icons, every application the dock shows; with titles, the pinned
    // ones only, the windows having buttons of their own.
    std::vector<const DockItem *> items;
    for (const auto &item : m_applications.items()) {
        if (!item->fixed && (item->isPinned || !m_windowTitles))
            items.push_back(item.get());
    }
    const bool same = items.size() == m_apps.size()
            && std::equal(items.begin(), items.end(), m_apps.begin(),
                          [](const DockItem *item, const AppButton *button) { return item->id == button->id; });
    if (same) {
        // Only their state changed: keep the buttons, and so their hover.
        for (size_t i = 0; i < items.size(); ++i)
            m_apps[i]->update(*items[i]);
        // The dots, which this view draws.
        requestPaint();
        targetsChanged();
        return;
    }
    if (m_menuButton && std::find(m_apps.begin(), m_apps.end(), m_menuButton) != m_apps.end())
        closeMenu();
    for (AppButton *button : std::exchange(m_apps, {}))
        retire(this, button);
    for (const DockItem *item : items) {
        AppButton *button = addChild<AppButton>(*item);
        button->onClicked = [this, id = item->id](const ui::PointerEvent &) {
            if (onActivated)
                onActivated();
            m_applications.clicked(id);
        };
        button->onMenuRequested = [this, button, id = item->id] { popupMenu(button, id); };
        if (m_badges)
            button->setCount(m_badges->count(item->id));
        m_apps.push_back(button);
    }
    requestLayout();
    requestPaint();
}

void PanelTasks::syncWindows()
{
    std::vector<uint64_t> ids;
    if (m_windowTitles) {
        for (uint64_t id : m_windows.windows()) {
            if (listed(id))
                ids.push_back(id);
        }
    }
    if (ids == windowButtons()) {
        for (uint64_t id : ids)
            updateWindow(id);
        return;
    }
    if (m_menuButton && std::find(m_windowButtons.begin(), m_windowButtons.end(), m_menuButton) != m_windowButtons.end())
        closeMenu();
    for (WindowButton *button : std::exchange(m_windowButtons, {}))
        retire(this, button);
    for (uint64_t id : ids) {
        WindowButton *button = addChild<WindowButton>(id);
        button->onClicked = [this, id](const ui::PointerEvent &event) {
            if (onActivated)
                onActivated();
            const TaskWindow *window = m_windows.window(id);
            if (!window)
                return;
            if (event.button == ui::MouseButton::Middle)
                m_windows.close(id);
            else if (m_windows.activeWindow() == id && !window->minimized && !window->hidden)
                m_windows.minimize(id);
            else
                m_windows.activate(id);
        };
        button->onMenuRequested = [this, button, id] {
            const std::string application = m_applications.appIdForWindow(id);
            if (!application.empty())
                popupMenu(button, application);
        };
        m_windowButtons.push_back(button);
        updateWindow(id);
    }
    requestLayout();
    requestPaint();
}

void PanelTasks::updateWindow(uint64_t id)
{
    const auto it = std::find_if(m_windowButtons.begin(), m_windowButtons.end(),
                                 [id](const WindowButton *button) { return button->id == id; });
    const TaskWindow *window = m_windows.window(id);
    if (it == m_windowButtons.end() || !window)
        return;
    const bool active = m_windows.activeWindow() == id && !window->minimized && !window->hidden;
    (*it)->update(m_applications.iconNameForWindow(id), window->title, active);
    requestLayout();
}

void PanelTasks::popupMenu(Button *button, const std::string &application)
{
    closeMenu();
    if (onActivated)
        onActivated();
    const int row = m_applications.indexOf(application);
    ui::Window *window = this->window();
    if (row < 0 || !window)
        return;
    // The dock's menu, as DockWindow::iconMenuRequested makes it.
    auto menu = std::make_unique<ui::Menu>();
    DockModel &model = m_applications;
    const DockItem &item = model.at(row);
    const auto windows = model.windows(application);
    for (const DockModel::WindowInfo &info : windows) {
        const size_t index = menu->addItem(info.title.empty() ? item.visibleName : info.title,
                                           [&model, id = info.id] { model.activateWindow(id); });
        menu->setItemChecked(index, info.active);
        menu->setItemMaximumWidth(index, 320);
    }
    if (!windows.empty())
        menu->addSeparator();
    if (windows.empty())
        menu->addItem(tr("Open"), [&model, application] { model.openNewInstance(application); });
    if (item.pinnable()) {
        const bool pinned = item.isPinned;
        menu->addItem(pinned ? tr("Unpin") : tr("Pin"), [&model, application, pinned] {
            pinned ? model.unPin(application) : model.pin(application);
        });
    }
    if (!windows.empty())
        menu->addItem(tr("Quit"), [&model, application] { model.quit(application); });

    m_menu = std::move(menu);
    m_menuButton = button;
    m_menu->onClosed = [this] {
        if (Button *open = std::exchange(m_menuButton, nullptr))
            open->setMenuOpen(false);
    };
    // Above the button, its arrow at the button's centre.
    const ui::PointF topLeft = button->mapToWindow({0, 0});
    const ui::RectF slot {topLeft.x, topLeft.y, button->geometry().width, button->geometry().height};
    const ui::PointF centre {slot.x + slot.width / 2, slot.y};
    m_menu->setArrow(ui::BubbleShape::Edge::Bottom, centre);
    m_menu->setContinuousCorners(true);
    if (availableGeometry)
        m_menu->setAvailableGeometry(availableGeometry());
    const ui::SizeF size = m_menu->implicitSize(window);
    const double gap = ui::Theme::current().smallSpacing;
    const double inset = m_menu->minimumEdgeLength() / 2;
    const ui::PointF origin {std::min(slot.x, centre.x - inset), -size.height - gap};
    m_menu->popupAt(window, {std::round(origin.x), std::round(origin.y)});
    button->setMenuOpen(m_menu->isOpen());
}

void PanelTasks::closeMenu()
{
    if (!m_menu)
        return;
    auto menu = std::move(m_menu);
    menu->onClosed = nullptr;
    menu->close();
    if (Button *open = std::exchange(m_menuButton, nullptr))
        open->setMenuOpen(false);
    // The menu may be closing from its own action.
    std::shared_ptr<ui::Menu> retired(std::move(menu));
    core::mainLoop()->post([retired] {});
}

void PanelTasks::layout()
{
    const ui::RectF bounds = localRect();
    const double tile = std::min(m_tileSize, bounds.height);
    const double top = std::round((bounds.height - tile) / 2);
    m_dividers.clear();
    double x = 0;
    m_start->setGeometry({x, top, tile, tile});
    x += tile + kDividerGap;
    m_dividers.push_back(x);
    x += kDividerGap + 1;
    for (AppButton *button : m_apps) {
        button->setGeometry({x, top, tile, tile});
        x += tile + kGap;
    }
    if (!m_apps.empty() && !m_windowButtons.empty()) {
        x += kDividerGap - kGap;
        m_dividers.push_back(x);
        x += kDividerGap + 1;
    }
    // The windows share what is left, each up to its title's width.
    const double room = std::max(0.0, bounds.width - x);
    const size_t count = m_windowButtons.size();
    const double share = count ? (room - kGap * double(count - 1)) / double(count) : 0;
    for (WindowButton *button : m_windowButtons) {
        const double width = std::max(0.0, std::floor(std::min(button->preferredWidth(), share)));
        button->setVisible(width >= tile);
        button->setGeometry({x, top, width, tile});
        x += width + kGap;
    }
    targetsChanged();
}

void PanelTasks::paint(ui::Painter &painter)
{
    // The dividers: 1 px of --pit-border, as tall as an icon.
    const double height = std::round(std::min(m_tileSize, localRect().height) * 0.55);
    for (double x : m_dividers)
        painter.fillRect({x, std::round((localRect().height - height) / 2), 1, height}, PIT_COLOR(BORDER));

    // The running applications' dots, below their tiles, a dot apart.
    constexpr double dot = 4;
    for (const AppButton *button : m_apps) {
        const ui::RectF tile = button->geometry();
        const double y = std::min(tile.y + tile.height + 2, localRect().height - dot);
        for (int i = 0, dots = button->dots(); i < dots; ++i) {
            const double x = tile.x + (tile.width - dot) / 2 + (i - (dots - 1) / 2.0) * dot * 2;
            painter.fillEllipse({x, y, dot, dot}, button->isSelected() ? accent(false) : PIT_COLOR(TEXT_SECONDARY));
        }
    }
}

std::map<uint64_t, ui::RectF> PanelTasks::windowTargets() const
{
    std::map<uint64_t, ui::RectF> targets;
    for (const WindowButton *button : m_windowButtons) {
        if (button->isVisible())
            targets[button->id] = button->iconTarget();
    }
    if (m_windowTitles)
        return targets;
    // As icons: every window into its application's.
    for (uint64_t id : m_windows.windows()) {
        if (!listed(id))
            continue;
        const std::string application = m_applications.appIdForWindow(id);
        for (const AppButton *button : m_apps) {
            if (button->id == application)
                targets[id] = button->iconTarget();
        }
    }
    return targets;
}

std::vector<std::string> PanelTasks::applicationButtons() const
{
    std::vector<std::string> ids;
    for (const AppButton *button : m_apps)
        ids.push_back(button->id);
    return ids;
}

int PanelTasks::applicationDots(const std::string &application) const
{
    for (const AppButton *button : m_apps) {
        if (button->id == application)
            return button->dots();
    }
    return 0;
}

std::vector<uint64_t> PanelTasks::windowButtons() const
{
    std::vector<uint64_t> ids;
    for (const WindowButton *button : m_windowButtons)
        ids.push_back(button->id);
    return ids;
}

} // namespace bayu::shell
