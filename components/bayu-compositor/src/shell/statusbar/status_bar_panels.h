// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BAYU_SHELL_STATUSBAR_STATUS_BAR_PANELS_H_
#define BAYU_SHELL_STATUSBAR_STATUS_BAR_PANELS_H_

#include <string>

#include "base/geometry.h"
#include "base/signal.h"

namespace bayu::shell {

class ShellWindow;

// The panels the bar's items open, which other components own: the control
// centre and its status panels, the notification centre.
class StatusBarPanels
{
public:
    enum class Panel {
        ControlCenter,
        Network,
        Volume,
        Battery,
        NotificationCenter,
    };

    virtual ~StatusBarPanels() = default;

    virtual bool hasPanel(Panel panel) const = 0;
    // Called after the bar closed every popup.
    // `anchor` is the item's rect in the bar window's coordinates; the
    // panel's top-left goes to the left edge of the item's plate
    // (anchor.x() + 3) at the bar's bottom.
    virtual void openPanel(Panel panel, ShellWindow *bar, const base::Rect &anchor) = 0;
    virtual void closePanels() = 0;
    virtual bool isOpen(Panel panel) const = 0;

    // A panel opened or closed.
    base::Signal<> panelsChanged;
};

// What the bar's icons show, read from the clients the control centre reads
// too: the default sink and the Wi-Fi state.
class StatusBarIndicators
{
public:
    struct Audio {
        bool hasSink = false;
        bool muted = false;
        // The default sink's volume over PipeWire's normal volume.
        double volume = 0;

        bool operator==(const Audio &) const = default;
    };

    struct Network {
        bool wirelessHwEnabled = false;
        bool wirelessEnabled = false;
        bool hotspotActive = false;
        // The active wireless connection's, empty when there is none.
        std::string wirelessName;
        // A FishUI glyph name, network-wireless-connected-NN.
        std::string wirelessIcon;

        // An Ethernet device is there, an Ethernet connection is up, and
        // whether NetworkManager makes that wired connection primary.
        bool wiredHwEnabled = false;
        bool wiredConnected = false;
        bool wiredPrimary = false;
        // The active wired connection's, empty when there is none.
        std::string wiredName;
        // A FishUI glyph name, network-wired or network-wired-unavailable.
        std::string wiredIcon;

        bool operator==(const Network &) const = default;
    };

    virtual ~StatusBarIndicators() = default;

    virtual Audio audio() const = 0;
    virtual Network network() const = 0;

    base::Signal<> changed;
};

} // namespace bayu::shell

#endif // BAYU_SHELL_STATUSBAR_STATUS_BAR_PANELS_H_
