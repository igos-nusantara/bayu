// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The status bar as the compositor draws it: its window, the layout of its
// items, panels, the tray, the clock and the colours it takes from what is
// behind it, headless on a fake renderer and a private bus.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <thread>

#include <linux/input-event-codes.h>

#include <gtest/gtest.h>

#include "compositor/compositor.h"
#include "compositor/surface.h"
#include "dbus_test_client.h"
#include "fake_renderer.h"
#include "input/input_manager.h"
#include "input/seat.h"
#include "loop_test_util.h"
#include "scene/node.h"
#include "scene/scene.h"
#include "shell/bar_theme.h"
#include "shell/shell.h"
#include "shell/shell_data.h"
#include "shell/shell_window.h"
#include "shell/statusbar/bar_item.h"
#include "shell/statusbar/battery.h"
#include "shell/statusbar/dbus_menu_popup.h"
#include "shell/statusbar/device_usage.h"
#include "shell/statusbar/status_bar.h"
#include "shell/statusbar/status_bar_backdrop.h"
#include "shell/wallpaper/wallpaper.h"
#include "shell/statusbar/status_bar_window.h"
#include "shell/statusbar/status_icon.h"
#include "shell/statusbar/system_tray_view.h"
#include "shell_test_util.h"
#include "statusbar_fakes.h"
#include "temporary_directory.h"
#include "ui/bitmap.h"
#include "ui/embedded_window.h"
#include "ui/menu.h"
#include "ui/theme.h"
#include "wm/window.h"
#include "wm/window_manager.h"
#include "wm/window_visual_controller.h"

using namespace bayu;
namespace dbus = base::dbus;
using Panel = shell::StatusBarPanels::Panel;

namespace {

input::Timestamp at(int ms)
{
    return input::Timestamp(std::chrono::milliseconds(ms));
}

const base::Rect kOutput(0, 0, 1440, 960);

dbus::Connection *sessionBus()
{
    return dbus::Connection::session(core::mainLoop());
}

class FakeIndicators final : public shell::StatusBarIndicators
{
public:
    Audio audio() const override { return m_audio; }
    Network network() const override { return m_network; }

    Audio m_audio;
    Network m_network;
};

class FakePanels final : public shell::StatusBarPanels
{
public:
    bool hasPanel(Panel panel) const override { return panel != Panel::NotificationCenter; }

    void openPanel(Panel panel, shell::ShellWindow *bar, const base::Rect &anchor) override
    {
        opened.push_back(panel);
        m_open = panel;
        m_isOpen = true;
        lastBar = bar;
        lastAnchor = anchor;
        panelsChanged();
    }

    void closePanels() override
    {
        ++closes;
        if (!m_isOpen)
            return;
        m_isOpen = false;
        panelsChanged();
    }

    bool isOpen(Panel panel) const override { return m_isOpen && m_open == panel; }

    std::vector<Panel> opened;
    int closes = 0;
    shell::ShellWindow *lastBar = nullptr;
    base::Rect lastAnchor;

private:
    Panel m_open = Panel::ControlCenter;
    bool m_isOpen = false;
};

class StatusBarViewTest : public testing::Test
{
protected:
    scene::Scene scene;
    scene::Node *surfaceContainer = scene.root()->addChild(std::make_unique<scene::Node>("surfaces"));
    compositor::Compositor compositor {surfaceContainer};
    testsupport::FakeRenderer renderer;
    input::Seat seat {&compositor};
    input::InputManager input {&seat};
    wm::WindowManager manager {&compositor, &seat};
    test::TemporaryDirectory directory;
    std::unique_ptr<shell::Shell> shell;
    shell::StatusBarWindow *bar = nullptr;
    int time = 1000;

    void SetUp() override
    {
        shell::setShellDataDirectoryForTesting(BAYU_SOURCE_DIR "/data/shell");
        scene.setSize(kOutput.size());
        manager.setOutputBounds(kOutput);
        input.setBounds(kOutput.size());
        input.setEventSink(&manager);
        shell::ShellEnvironment environment {&compositor, &manager, &seat};
        environment.servicesSettings = directory.filePath("services.conf");
        shell = std::make_unique<shell::Shell>(environment);
        auto apps = std::make_unique<test::FakeApplications>();
        apps->scan();
        shell->setApplications(std::move(apps));
        shell->setTaskWindows(std::make_unique<test::FakeTaskWindows>());
        directory.write("shell.conf", "");
        auto backend = std::make_unique<shell::StatusBar>(sessionBus(), dbus::Connection::system(core::mainLoop()),
                                                          &shell->tasks(), directory.filePath("shell.conf"));
        bar = shell->addComponent(std::make_unique<shell::StatusBarWindow>(shell.get(), std::move(backend)));
        frame();
    }

    void TearDown() override
    {
        shell.reset();
        seat.shutdown();
    }

    void frame() { compositor.updateTextures(renderer); }

    void move(double x, double y)
    {
        input.onPointerMotionAbsolute(at(time += 20), {x, y});
        frame();
    }

    void click(double x, double y, uint32_t button = BTN_LEFT)
    {
        move(x, y);
        input.onPointerButton(at(time += 20), button, true);
        input.onPointerButton(at(time += 20), button, false);
        frame();
    }

    // An item's rect on the screen.
    static ui::RectF onScreen(const ui::View *view)
    {
        const ui::PointF origin = view->mapToWindow({0, 0});
        return {origin.x, origin.y, view->geometry().width, view->geometry().height};
    }
};

} // namespace

TEST_F(StatusBarViewTest, TheBarIsAPanelAlongThePrimaryOutputsTop)
{
    wm::Window *window = bar->window()->wmWindow();
    ASSERT_NE(nullptr, window);
    EXPECT_EQ(wm::WindowRole::Panel, window->role());
    EXPECT_EQ(wm::FocusPolicy::NoFocus, window->focusPolicy());
    EXPECT_EQ(base::Rect(0, 0, 1440, 32), window->bounds());
    EXPECT_EQ(base::Rect(0, 32, 1440, 928), shell->workArea(*shell->output({})));

    // The output's bounds change: the bar follows.
    manager.setOutputBounds({0, 0, 1280, 800});
    test::runFor(std::chrono::milliseconds(20));
    frame();
    EXPECT_EQ(base::Rect(0, 0, 1280, 32), bar->window()->wmWindow()->bounds());
}

TEST_F(StatusBarViewTest, ItemsAbutTheEdgeAndEachOther)
{
    // The clock's slot reaches the screen's right edge; its plate stops 9
    // short of it.
    const ui::RectF clock = onScreen(bar->clock());
    EXPECT_NEAR(1440, clock.x + clock.width, 1);
    EXPECT_DOUBLE_EQ(32, clock.height);
    // 6 + text + 12 + 12.
    EXPECT_GT(clock.width, 30);

    // The control centre's icon: 12 + 16 + 12, right before the clock.
    const ui::RectF control = onScreen(bar->controlIcon());
    EXPECT_DOUBLE_EQ(40, control.width);
    EXPECT_DOUBLE_EQ(clock.x, control.x + control.width);
    EXPECT_EQ(shell::shellDataFile("statusbar/light/control.svg").empty(), false);

    // Hovering lights nothing; checked, the plate shows inset 3 each side,
    // 4 from the top, 24 tall.
    move(control.x + 20, 16);
    EXPECT_FALSE(bar->controlIcon()->isPlateShown());
    bar->controlIcon()->setChecked(true);
    EXPECT_TRUE(bar->controlIcon()->isPlateShown());
    const ui::RectF plate = bar->controlIcon()->plateRect();
    EXPECT_DOUBLE_EQ(3, plate.x);
    EXPECT_DOUBLE_EQ(34, plate.width);
    EXPECT_DOUBLE_EQ(4, plate.y);
    EXPECT_DOUBLE_EQ(24, plate.height);
    // Dark contents over a light theme: black at 0.1.
    EXPECT_FALSE(bar->lightContents());
    EXPECT_EQ(26, bar->controlIcon()->plateColor().a);
    EXPECT_EQ(0, bar->controlIcon()->plateColor().r);
    bar->controlIcon()->setChecked(false);

    // Without indicators there is no volume or Wi-Fi icon.
    EXPECT_FALSE(bar->volumeIcon()->isVisible());
    EXPECT_FALSE(bar->networkIcon()->isVisible());
    // The battery shows when the system has one.
    EXPECT_EQ(bar->statusBar().battery().isAvailable(), bar->batteryIcon()->isVisible());
}

TEST_F(StatusBarViewTest, AnUnchangedClockDrawsNoFrame)
{
    // Not across a minute's end.
    if (std::time(nullptr) % 60 >= 58)
        std::this_thread::sleep_for(std::chrono::seconds(3));
    bar->updateClock();
    frame();

    const ui::FrameStatistics &statistics = bar->window()->window()->frameStatistics();
    const uint64_t rendered = statistics.rendered;
    bar->updateClock();
    frame();
    EXPECT_EQ(rendered, statistics.rendered);
    // Hovering changes nothing either.
    move(onScreen(bar->clock()).x + 20, 16);
    EXPECT_EQ(rendered, statistics.rendered);

    bar->clock()->setChecked(true);
    frame();
    EXPECT_GT(statistics.rendered, rendered);
}

TEST_F(StatusBarViewTest, IndicatorsPickTheIcons)
{
    FakeIndicators indicators;
    indicators.m_audio = {true, false, 0.5};
    indicators.m_network = {true, true, false, "Home", "network-wireless-connected-66"};
    bar->setIndicators(&indicators);
    frame();
    EXPECT_TRUE(bar->volumeIcon()->isVisible());
    EXPECT_EQ("audio-volume-medium-symbolic", bar->volumeIcon()->iconName());
    EXPECT_EQ("network-upload-download", bar->networkIcon()->glyph());

    indicators.m_audio = {true, false, 0.2};
    indicators.m_network = {true, true, false, "", ""};
    indicators.changed();
    EXPECT_EQ("audio-volume-low-symbolic", bar->volumeIcon()->iconName());
    EXPECT_EQ("network-wireless-connected-00", bar->networkIcon()->glyph());

    indicators.m_audio = {true, true, 0.9};
    indicators.m_network = {true, false, false, "", ""};
    indicators.changed();
    EXPECT_EQ("audio-volume-muted-symbolic", bar->volumeIcon()->iconName());
    EXPECT_EQ("network-wireless-disabled", bar->networkIcon()->glyph());

    indicators.m_audio = {true, false, 0.9};
    indicators.m_network = {true, true, true, "", ""};
    indicators.changed();
    EXPECT_EQ("audio-volume-high-symbolic", bar->volumeIcon()->iconName());
    EXPECT_EQ("network-wireless-hotspot", bar->networkIcon()->glyph());

    // A wired connection is what the bar shows while Wi-Fi is on but idle.
    indicators.m_audio = {true, false, 0.5};
    indicators.m_network = {true, true, false, "", "", true, true, true, "Sambungan kabel 1", "network-wired"};
    indicators.changed();
    EXPECT_EQ("network-upload-download", bar->networkIcon()->glyph());
    EXPECT_TRUE(bar->networkIcon()->isVisible());

    // With both up, the primary connection still uses the activity arrows.
    indicators.m_network = {true, true, false, "Home", "network-wireless-connected-66",
                            true, true, false, "Sambungan kabel 1", "network-wired"};
    indicators.changed();
    EXPECT_EQ("network-upload-download", bar->networkIcon()->glyph());

    // Wired hardware alone, disconnected: its icon stays, not Wi-Fi's.
    indicators.m_network = {false, false, false, "", "", true, false, false, "", "network-wired-unavailable"};
    indicators.changed();
    EXPECT_EQ("network-wired-unavailable", bar->networkIcon()->glyph());
    EXPECT_TRUE(bar->networkIcon()->isVisible());

    // Wi-Fi on and disconnected keeps its own icon while the cable is out.
    indicators.m_network = {true, true, false, "", "", true, false, false, "", "network-wired-unavailable"};
    indicators.changed();
    EXPECT_EQ("network-wireless-connected-00", bar->networkIcon()->glyph());

    // The privacy dot widens the control icon by the gap and the dot.
    bar->setDeviceUsage(std::make_unique<shell::DeviceUsage>(nullptr, nullptr, nullptr));
    bar->deviceUsage()->setUsesForTesting({{shell::DeviceUse::Microphone, 42, {}, "recorder"}});
    frame();
    EXPECT_DOUBLE_EQ(12 + 16 + 6 + 6 + 12, onScreen(bar->controlIcon()).width);
    ASSERT_EQ(1u, bar->deviceUsage()->applications().size());
    EXPECT_EQ("recorder", bar->deviceUsage()->applications().front().name);
    EXPECT_TRUE(bar->deviceUsage()->applications().front().microphone);

    indicators.m_network = {};
    indicators.m_audio.hasSink = false;
    indicators.changed();
    EXPECT_FALSE(bar->volumeIcon()->isVisible());
    EXPECT_FALSE(bar->networkIcon()->isVisible());
    bar->setIndicators(nullptr);
}

TEST_F(StatusBarViewTest, TheTopbarUsesTheDockTheme)
{
    using shell::BarTheme;
    bar->statusBar().setTheme(BarTheme::Default);

    EXPECT_EQ(BarTheme::Default, bar->statusBar().theme());
    EXPECT_EQ(ui::Theme::current().darkMode, bar->lightContents());
    EXPECT_FALSE(bar->window()->surface()->backgroundBlurRect().isEmpty());
    EXPECT_EQ(shell::barGlass(shell::barTheme(BarTheme::Default).glassTint),
              bar->window()->surface()->glass());

    bar->statusBar().setTheme(BarTheme::Clear);
    EXPECT_EQ(BarTheme::Clear, bar->statusBar().theme());
    EXPECT_EQ(shell::barGlass(shell::barTheme(BarTheme::Clear).glassTint),
              bar->window()->surface()->glass());
}

TEST_F(StatusBarViewTest, AnIconTogglesItsPanelUnderItsPlate)
{
    FakePanels panels;
    bar->addPanels(&panels);
    const ui::RectF control = onScreen(bar->controlIcon());
    click(control.x + 20, 16);
    ASSERT_EQ(1u, panels.opened.size());
    EXPECT_EQ(Panel::ControlCenter, panels.opened.front());
    EXPECT_EQ(bar->window(), panels.lastBar);
    EXPECT_EQ(int(control.x), panels.lastAnchor.x());
    EXPECT_EQ(32, panels.lastAnchor.height());
    EXPECT_TRUE(bar->controlIcon()->isChecked());
    EXPECT_TRUE(bar->panelOpen());

    // The bar keeps its shared glass while the plate shows, and the plate has
    // no outline.
    const compositor::Surface *surface = bar->window()->surface();
    EXPECT_EQ(base::Rect(0, 0, 1440, 32), surface->backgroundBlurRect());
    EXPECT_TRUE(surface->glass().enabled);
    EXPECT_TRUE(surface->backgroundBlurShape().isEmpty());
    EXPECT_FALSE(wm::WindowVisualController::evaluate(*bar->window()->wmWindow()).drawOutline);

    const ui::RectF clock = onScreen(bar->clock());

    // Its icon again closes it, and opens nothing.
    click(control.x + 20, 16);
    EXPECT_EQ(1u, panels.opened.size());
    EXPECT_FALSE(bar->controlIcon()->isChecked());
    EXPECT_FALSE(bar->panelOpen());

    // The plate goes with the panel; hovering brings none back.
    EXPECT_FALSE(bar->controlIcon()->isPlateShown());
    EXPECT_EQ(base::Rect(0, 0, 1440, 32), surface->backgroundBlurRect());
    EXPECT_TRUE(surface->glass().enabled);
    move(clock.x + 20, 16);
    EXPECT_FALSE(bar->clock()->isPlateShown());
    EXPECT_EQ(base::Rect(0, 0, 1440, 32), surface->backgroundBlurRect());

    // The clock's panel is nobody's here.
    click(clock.x + 20, 16);
    EXPECT_EQ(1u, panels.opened.size());
    bar->removePanels(&panels);
}

TEST_F(StatusBarViewTest, ThePictureBehindTheBarPicksItsColours)
{
    using Backdrop = shell::StatusBarBackdrop;
    Backdrop::Sample sample;
    sample.valid = true;
    // A smooth dark strip: light contents, no gradient.
    sample.red = sample.green = sample.blue = 20;
    Backdrop::Style style = Backdrop::styleForSample(sample);
    EXPECT_TRUE(style.useLightForeground);
    EXPECT_FALSE(style.gradientVisible);
    // A smooth mid tone: dark contents on a light gradient.
    sample.red = sample.green = sample.blue = 170;
    style = Backdrop::styleForSample(sample);
    EXPECT_FALSE(style.useLightForeground);
    EXPECT_TRUE(style.gradientVisible);
    EXPECT_EQ(115, style.gradientColor.a);
    // Busy: light contents up to a lighter average, and a dark gradient.
    sample.detail = 9;
    sample.red = sample.green = sample.blue = 150;
    style = Backdrop::styleForSample(sample);
    EXPECT_TRUE(style.useLightForeground);
    EXPECT_TRUE(style.gradientVisible);
    EXPECT_EQ(77, style.gradientColor.a);

    // The strip's mean colour and its texture.
    ui::Bitmap stripes(512, 64, ui::Bitmap::Format::Argb32Premultiplied);
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 512; ++x)
            stripes.setPixel(x, y, y < 32 ? 0xff102030u : 0xffffffffu);
    }
    const Backdrop::Sample flat = Backdrop::sampleImage(stripes, 32);
    EXPECT_TRUE(flat.valid);
    EXPECT_NEAR(0x10, flat.red, 0.5);
    EXPECT_NEAR(0x30, flat.blue, 0.5);
    EXPECT_NEAR(0, flat.detail, 1e-9);

    // A dark colour behind the bar: white contents, with the shadow edge.
    bar->backdrop().setColor(ui::Color::rgb(0x101820));
    EXPECT_TRUE(bar->lightContents());
    EXPECT_EQ(ui::Color::rgb(0xFFFFFF), bar->textColor());
    // The desktop's picture, judged once decoded on a worker.
    ui::Bitmap light(64, 64, ui::Bitmap::Format::Argb32Premultiplied);
    light.fill(0xfff0f0f0u);
    ASSERT_TRUE(light.savePng(directory.filePath("light.png")));
    auto *wallpaper = shell->addComponent(std::make_unique<shell::Wallpaper>(shell.get(), directory.filePath("cache")));
    shell::Wallpaper::Background background;
    background.path = directory.filePath("light.png");
    wallpaper->setBackground(background);
    bar->setWallpaper(wallpaper);
    EXPECT_TRUE(test::runUntil([&] { return !bar->lightContents(); }));
    EXPECT_EQ((ui::Color {0, 0, 0, 217}), bar->textColor());
    EXPECT_EQ(ui::Color::rgb(0x000000), bar->menuTextColor());
    bar->setWallpaper(nullptr);
    // Nothing shown: the theme decides again.
    bar->backdrop().setUnavailable();
    EXPECT_FALSE(bar->backdrop().isValid());
}

TEST_F(StatusBarViewTest, TrayItemsSitRightToLeftAndTakeClicks)
{
    ASSERT_TRUE(bar->statusBar().registerServices());
    test::DBusTestClient application;
    test::FakeTrayItem item(application.connection.get());
    item.menuPath = "/MenuBar";
    item.iconName = "no-such-icon-anywhere";
    test::FakeMenuExporter exporter(application.connection.get(), "/MenuBar");
    exporter.layouts[0] = test::layout(0, {},
                                       {test::layout(1, {{"label", dbus::Variant(std::string("_Open"))}}),
                                        test::layout(2, {{"type", dbus::Variant(std::string("separator"))}}),
                                        test::layout(3, {{"label", dbus::Variant(std::string("_Quit"))}})});
    application.call("org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher", "org.kde.StatusNotifierWatcher",
                     "RegisterStatusNotifierItem", std::string("/StatusNotifierItem"));
    ASSERT_TRUE(test::runUntil([&] { return bar->tray()->count() == 1; }));
    frame();

    // 40 wide, right before the volume, Wi-Fi, battery and control icons.
    shell::TrayItemView *view = bar->tray()->item(0);
    const ui::RectF slot = onScreen(view);
    EXPECT_DOUBLE_EQ(40, slot.width);
    const ui::RectF tray = onScreen(bar->tray());
    EXPECT_DOUBLE_EQ(tray.x + tray.width, slot.x + slot.width);

    // Left: activated at the click.
    click(slot.x + 20, 16);
    ASSERT_TRUE(test::runUntil([&] { return !item.calls.empty(); }));
    EXPECT_EQ("Activate " + std::to_string(int(slot.x + 20)) + ",16", item.calls.front());

    // Right: its menu under the plate, the plate lit while it is up.
    ASSERT_TRUE(test::runUntil([&] { return bar->statusBar().systemTray().at(0)->menu(); }));
    click(slot.x + 20, 16, BTN_RIGHT);
    EXPECT_TRUE(view->isPlateShown());
    ASSERT_TRUE(test::runUntil([&] { return bar->tray()->openMenu() != nullptr; })) << item.calls.size();
    EXPECT_TRUE(view->isPlateShown());
    EXPECT_TRUE(test::runUntil([&] { return exporter.called("AboutToShow 0"); }));
    // A right click on it again closes it.
    click(slot.x + 20, 16, BTN_RIGHT);
    EXPECT_EQ(nullptr, bar->tray()->openMenu());
    move(700, 500);
    EXPECT_FALSE(view->isPlateShown());
    EXPECT_TRUE(test::runUntil([&] { return exporter.called("Event 0 closed"); }));
}

TEST_F(StatusBarViewTest, ARightClickOpensTheTrayMenuOnce)
{
    ASSERT_TRUE(bar->statusBar().registerServices());
    test::DBusTestClient application;
    test::FakeTrayItem item(application.connection.get());
    item.menuPath = "/MenuBar";
    test::FakeMenuExporter exporter(application.connection.get(), "/MenuBar");
    exporter.layouts[0] = test::layout(0, {}, {test::layout(1, {{"label", dbus::Variant(std::string("_Open"))}})});
    application.call("org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher", "org.kde.StatusNotifierWatcher",
                     "RegisterStatusNotifierItem", std::string("/StatusNotifierItem"));
    ASSERT_TRUE(test::runUntil([&] { return bar->tray()->count() == 1; }));
    frame();
    ASSERT_TRUE(test::runUntil([&] { return bar->statusBar().systemTray().at(0)->menu(); }));
    ASSERT_TRUE(test::runUntil([&] { return exporter.called("GetLayout 0 1"); }));

    const ui::RectF slot = onScreen(bar->tray()->item(0));
    click(slot.x + 20, 16, BTN_RIGHT);
    ASSERT_TRUE(test::runUntil([&] { return bar->tray()->openMenu() != nullptr; }));
    ASSERT_TRUE(test::runUntil([&] { return exporter.called("Event 0 opened"); }));
    test::runFor(std::chrono::milliseconds(100));

    // AboutToShow, then "opened" once the menu is up, and "closed" as it goes.
    std::vector<std::string> events;
    for (const std::string &call : exporter.calls) {
        if (!call.starts_with("GetLayout"))
            events.push_back(call);
    }
    EXPECT_EQ((std::vector<std::string>{"AboutToShow 0", "Event 0 opened"}), events);
    EXPECT_TRUE(item.calls.empty());

    click(slot.x + 20, 16, BTN_RIGHT);
    EXPECT_EQ(nullptr, bar->tray()->openMenu());
    ASSERT_TRUE(test::runUntil([&] { return exporter.called("Event 0 closed"); }));
    test::runFor(std::chrono::milliseconds(50));
    EXPECT_EQ(1, std::count(exporter.calls.begin(), exporter.calls.end(), "Event 0 closed"));
}

TEST_F(StatusBarViewTest, ALeftClickOpensTheMenuOfAnItemThatIsOnlyAMenu)
{
    ASSERT_TRUE(bar->statusBar().registerServices());
    test::DBusTestClient application;
    test::FakeTrayItem item(application.connection.get());
    item.menuPath = "/MenuBar";
    item.itemIsMenu = true;
    test::FakeMenuExporter exporter(application.connection.get(), "/MenuBar");
    exporter.layouts[0] = test::layout(0, {}, {test::layout(1, {{"label", dbus::Variant(std::string("_Open"))}})});
    application.call("org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher", "org.kde.StatusNotifierWatcher",
                     "RegisterStatusNotifierItem", std::string("/StatusNotifierItem"));
    ASSERT_TRUE(test::runUntil([&] { return bar->tray()->count() == 1; }));
    frame();
    ASSERT_TRUE(test::runUntil([&] { return bar->statusBar().systemTray().at(0)->menu(); }));

    const ui::RectF slot = onScreen(bar->tray()->item(0));
    click(slot.x + 20, 16);
    ASSERT_TRUE(test::runUntil([&] { return bar->tray()->openMenu() != nullptr; }));
    EXPECT_TRUE(exporter.called("AboutToShow 0"));
    // Again, it closes.
    click(slot.x + 20, 16);
    EXPECT_EQ(nullptr, bar->tray()->openMenu());
    test::runFor(std::chrono::milliseconds(50));
    EXPECT_TRUE(item.calls.empty());

    // No longer only a menu: a left click activates it again.
    item.itemIsMenu = false;
    item.object.emitSignal("NewTitle");
    test::runFor(std::chrono::milliseconds(100));
    click(slot.x + 20, 16);
    ASSERT_TRUE(test::runUntil([&] { return !item.calls.empty(); }));
    EXPECT_EQ("Activate " + std::to_string(int(slot.x + 20)) + ",16", item.calls.front());
    EXPECT_EQ(nullptr, bar->tray()->openMenu());
}

TEST_F(StatusBarViewTest, AnItemThatLeavesTakesItsOpenMenu)
{
    ASSERT_TRUE(bar->statusBar().registerServices());
    auto application = std::make_unique<test::DBusTestClient>();
    auto item = std::make_unique<test::FakeTrayItem>(application->connection.get());
    item->menuPath = "/MenuBar";
    auto exporter = std::make_unique<test::FakeMenuExporter>(application->connection.get(), "/MenuBar");
    exporter->layouts[0] = test::layout(0, {},
                                        {test::layout(1, {{"label", dbus::Variant(std::string("_Open"))}}),
                                         test::layout(2, {{"label", dbus::Variant(std::string("_Recent"))},
                                                          {"children-display", dbus::Variant(std::string("submenu"))}})});
    application->call("org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher", "org.kde.StatusNotifierWatcher",
                      "RegisterStatusNotifierItem", std::string("/StatusNotifierItem"));
    ASSERT_TRUE(test::runUntil([&] { return bar->tray()->count() == 1; }));
    frame();
    ASSERT_TRUE(test::runUntil([&] { return bar->statusBar().systemTray().at(0)->menu(); }));

    const ui::RectF slot = onScreen(bar->tray()->item(0));
    click(slot.x + 20, 16, BTN_RIGHT);
    ASSERT_TRUE(test::runUntil([&] { return bar->tray()->openMenu() != nullptr; }));

    // The application quits with its menu up: the menu closes without
    // reaching the item's menu client, which went first.
    exporter.reset();
    item.reset();
    application.reset();
    ASSERT_TRUE(test::runUntil([&] { return bar->tray()->count() == 0; }));
    frame();
    EXPECT_EQ(nullptr, bar->tray()->openMenu());
}

TEST(DBusMenuPopupTest, LabelsLoseTheirMnemonics)
{
    EXPECT_EQ("File", shell::DBusMenuPopup::plainLabel("_File"));
    EXPECT_EQ("Save_As", shell::DBusMenuPopup::plainLabel("Save__As"));
    EXPECT_EQ("Tom & Jerry", shell::DBusMenuPopup::plainLabel("Tom & Jerry"));
}
