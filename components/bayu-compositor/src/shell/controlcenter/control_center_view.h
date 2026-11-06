// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BAYU_SHELL_CONTROLCENTER_CONTROL_CENTER_VIEW_H_
#define BAYU_SHELL_CONTROLCENTER_CONTROL_CENTER_VIEW_H_

#include <functional>

#include "base/timer.h"
#include "ui/view.h"

namespace bayu::ui {
class Label;
}

namespace bayu::shell {

class CardItem;
class ControlCenterServices;
class DeviceUsersTile;
class MediaTile;
class PowerTile;
class TileSlider;

// The control centre's content without its window: the title, the
// device-users tile, the cards, the media card and the two sliders, 360 wide.
class ControlCenterView : public ui::View
{
public:
    static constexpr double kWidth = 360;
    // Between the tiles and round them.
    static constexpr double kGap = 10;
    // The status panels' text inset, from the side and from the top.
    static constexpr double kTitleInset = 16;

    explicit ControlCenterView(ControlCenterServices &services);
    ~ControlCenterView() override;

    // Reads the services again.
    void refresh();
    // Takes its text again after the session language changed.
    void retranslate();
    // The window height at `scale`: the layout's, on whole device pixels.
    double windowHeight(double scale) const;

    CardItem *networkCard() const { return m_network; }
    CardItem *bluetoothCard() const { return m_bluetooth; }
    CardItem *darkModeCard() const { return m_darkMode; }
    CardItem *doNotDisturbCard() const { return m_doNotDisturb; }
    CardItem *cameraCard() const { return m_camera; }
    MediaTile *media() const { return m_media; }
    TileSlider *brightnessSlider() const { return m_brightness; }
    TileSlider *soundSlider() const { return m_sound; }

    // Press-and-hold on a card hides the control centre for Settings.
    std::function<void()> onHideRequested;
    // The Network card opens the network panel from the bar.
    std::function<void()> onNetworkRequested;
    // The layout's height may have changed.
    std::function<void()> onHeightChanged;

protected:
    void layout() override;

private:
    double layoutHeight() const;
    double headerHeight() const;

    ControlCenterServices &m_services;
    ui::Label *m_title;
    DeviceUsersTile *m_users;
    CardItem *m_network;
    CardItem *m_bluetooth;
    CardItem *m_darkMode;
    CardItem *m_doNotDisturb;
    CardItem *m_camera;
    CardItem *m_microphone;
    MediaTile *m_media;
    TileSlider *m_brightness;
    TileSlider *m_sound;
    PowerTile *m_power;
    base::Timer m_brightnessTimer;
    bool m_darkModeShown = false;
};

} // namespace bayu::shell

#endif // BAYU_SHELL_CONTROLCENTER_CONTROL_CENTER_VIEW_H_
