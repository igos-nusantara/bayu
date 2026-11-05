// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <memory>
#include <string>

#include "base/settings.h"
#include "base/signal.h"

namespace bayu::input {
class DBusExport;
}

namespace bayu::wm {
class TilingController;
}

namespace bayu::accelerators {

// Settings › Tiling (#149), kept in the compositor's preferences and applied
// to the TilingController, and org.bayu.Tiling1 for the Settings app: whether
// desks tile, the gap, the applications that always float, and the active
// desk's tiling.
class TilingSettings
{
public:
    explicit TilingSettings(wm::TilingController *tiling, const std::string &settingsFile = {});
    ~TilingSettings();

    bool registerService();

    void setTilesByDefault(bool tiled);
    void setGap(int gap);
    // One application id a line.
    std::string floatingApplications() const;
    void setFloatingApplications(const std::string &applications);

private:
    void persist();

    wm::TilingController *m_tiling = nullptr;
    base::Settings m_settings;
    std::unique_ptr<input::DBusExport> m_object;
    base::Subscription m_changed;
};

} // namespace bayu::accelerators
