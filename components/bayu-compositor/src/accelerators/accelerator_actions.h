// Copyright 2023 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.chromium file.
//
// Upstream: chromium/src/ash/public/cpp/accelerator_actions.h
// Revision: 8abbac30b9cf2fe976422c58c000c94685648b65
//
// Adapted for Bayu: the launcher, the lock screen, the window actions,
// the application menu's Quit, Hide and Hide Others, desks, screen capture and
// the volume, microphone, brightness and keyboard backlight keys. Media keys
// stay with the applications; browser, IME, accessibility, tablet and debug
// actions have nothing to act on.

#ifndef BAYU_ACCELERATORS_ACCELERATOR_ACTIONS_H_
#define BAYU_ACCELERATORS_ACCELERATOR_ACTIONS_H_

#include <cstdint>
#include <optional>
#include <vector>

#include <string>
#include <string_view>

namespace bayu::accelerators {

// Persisted and exposed over D-Bus by name, never by value, so the order here
// is free to change.
enum class AcceleratorAction : uint32_t {
    // General.
    ToggleAppList,
    LockScreen,

    // Windows.
    CycleForwardMru,
    CycleBackwardMru,
    ToggleMaximized,
    WindowMinimize,
    // The application menu's Quit, Hide and Hide Others.
    QuitApplication,
    HideApplication,
    HideOtherApplications,
    ToggleFullscreen,
    WindowCycleSnapLeft,
    WindowCycleSnapRight,
    // Super+Up and Down: a snapped half to its top or bottom quarter and back;
    // otherwise maximize and minimize (#149).
    WindowSnapUp,
    WindowSnapDown,
    // Auto-tiling (#149).
    TilingToggle,
    TilingToggleFloating,
    TilingSwapLeft,
    TilingSwapRight,
    TilingSwapUp,
    TilingSwapDown,
    TilingResizeLeft,
    TilingResizeRight,
    TilingResizeUp,
    TilingResizeDown,
    CloseWindow,
    MoveActiveWindowBetweenDisplays,
    ToggleOverview,
    ToggleShowDesktop,
    // Desks.
    DesksActivateDeskLeft,
    DesksActivateDeskRight,
    DesksMoveActiveItemLeft,
    DesksMoveActiveItemRight,
    DesksNewDesk,
    DesksRemoveCurrentDesk,
    DesksActivate0,
    DesksActivate1,
    DesksActivate2,
    DesksActivate3,
    DesksActivate4,
    DesksActivate5,
    DesksActivate6,
    DesksActivate7,
    DesksToggleAssignToAllDesks,

    // Screen capture, performed by the shell.
    TakeScreenshot,
    TakeScreenshotToClipboard,
    TakePartialScreenshot,
    TakePartialScreenshotToClipboard,
    StartCaptureMode,
    StopScreenRecording,

    // The clipboard history, performed by the shell.
    ToggleClipboardHistory,

    // Device keys, performed by the shell, which shows the new level.
    VolumeMuteToggle,
    VolumeDown,
    VolumeUp,
    MicrophoneMuteToggle,
    BrightnessDown,
    BrightnessUp,
    KeyboardBrightnessDown,
    KeyboardBrightnessUp,
};

using AcceleratorActionId = uint32_t;

constexpr AcceleratorActionId toActionId(AcceleratorAction action)
{
    return static_cast<AcceleratorActionId>(action);
}

// The kebab-case identifier used in preferences and on the bus.
std::string acceleratorActionName(AcceleratorAction action);
std::optional<AcceleratorAction> acceleratorActionFromName(std::string_view name);

// Every action, in declaration order. An action need not have a default
// binding to exist: it is then listed with none until the user adds one.
std::vector<AcceleratorAction> allAcceleratorActions();

} // namespace bayu::accelerators

#endif // BAYU_ACCELERATORS_ACCELERATOR_ACTIONS_H_
