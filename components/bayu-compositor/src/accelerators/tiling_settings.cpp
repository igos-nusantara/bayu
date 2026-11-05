// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "accelerators/tiling_settings.h"

#include <algorithm>
#include <sstream>

#include "core/logging.h"
#include "core/preferences.h"
#include "input/dbus_export.h"
#include "wm/tiling_controller.h"

namespace bayu::accelerators {
namespace {

constexpr auto service = "org.bayu.Tiling1";
constexpr auto path = "/org/bayu/Tiling1";

constexpr auto kDefaultKey = "Tiling/TileDesks";
constexpr auto kGapKey = "Tiling/Gap";
constexpr auto kFloatingKey = "Tiling/FloatingApplications";

} // namespace

TilingSettings::TilingSettings(wm::TilingController *tiling, const std::string &settingsFile)
    : m_tiling(tiling)
    , m_settings(settingsFile.empty() ? preferencesFile("compositor") : settingsFile)
{
    m_tiling->setGap(int(m_settings.intValue(kGapKey, m_tiling->gap())));
    m_tiling->setFloatingApplications(m_settings.stringListValue(kFloatingKey));
    m_tiling->setTilesByDefault(m_settings.boolValue(kDefaultKey, false));
    m_changed = m_tiling->changed.connect([this] {
        if (m_object)
            m_object->emitSignal("changed");
    });
}

TilingSettings::~TilingSettings() = default;

bool TilingSettings::registerService()
{
    if (m_object)
        return true;
    auto exported = input::DBusExport::create(service, path, service);
    if (!exported) {
        CF_WARNING(cfInput) << "Cannot export tiling settings: no session bus";
        return false;
    }
    base::dbus::Object &object = exported->object();
    object.addProperty("tilesByDefault", [this] { return m_tiling->tilesByDefault(); });
    object.addProperty("gap", [this] { return int32_t(m_tiling->gap()); });
    object.addProperty("floatingApplications", [this] { return floatingApplications(); });
    object.addProperty("activeDeskTiled", [this] { return m_tiling->isActiveDeskTiled(); });
    object.addMethod("setTilesByDefault", [this](bool tiled) { setTilesByDefault(tiled); },
                     base::dbus::ArgumentNames{{"tiled"}, {}});
    object.addMethod("setGap", [this](int32_t gap) { setGap(gap); },
                     base::dbus::ArgumentNames{{"gap"}, {}});
    object.addMethod("setFloatingApplications",
                     [this](const std::string &applications) {
                         setFloatingApplications(applications);
                     },
                     base::dbus::ArgumentNames{{"applications"}, {}});
    object.addMethod("toggleActiveDesk", [this] { m_tiling->toggleActiveDesk(); });
    object.addSignal("changed", "");
    if (!exported->publish("tiling settings"))
        return false;
    m_object = std::move(exported);
    return true;
}

void TilingSettings::setTilesByDefault(bool tiled)
{
    m_tiling->setTilesByDefault(tiled);
    persist();
}

void TilingSettings::setGap(int gap)
{
    m_tiling->setGap(gap);
    persist();
}

std::string TilingSettings::floatingApplications() const
{
    std::string text;
    for (const std::string &application : m_tiling->floatingApplications())
        text += application + "\n";
    return text;
}

void TilingSettings::setFloatingApplications(const std::string &applications)
{
    std::vector<std::string> list;
    std::istringstream lines(applications);
    for (std::string line; std::getline(lines, line);) {
        if (!line.empty() && std::find(list.begin(), list.end(), line) == list.end())
            list.push_back(line);
    }
    m_tiling->setFloatingApplications(std::move(list));
    persist();
}

void TilingSettings::persist()
{
    m_settings.setValue(kDefaultKey, m_tiling->tilesByDefault());
    m_settings.setValue(kGapKey, m_tiling->gap());
    m_settings.setValue(kFloatingKey, m_tiling->floatingApplications());
    if (!m_settings.sync())
        CF_WARNING(cfInput) << "Cannot persist tiling settings to" << m_settings.path();
    if (m_object)
        m_object->emitSignal("changed");
}

} // namespace bayu::accelerators
