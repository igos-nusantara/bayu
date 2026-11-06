// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BAYU_SHELL_SHELL_DATA_H_
#define BAYU_SHELL_SHELL_DATA_H_

#include <string>
#include <string_view>

#include "render/glass.h"

namespace bayu::shell {

// The shell's own artwork, installed with the compositor.
std::string shellDataFile(std::string_view name);
// Tests read the artwork from the source tree.
void setShellDataDirectoryForTesting(std::string directory);

// FishUI's glass as a client's bayu_surface_blur_v1 requests carry it:
// every value through wl_fixed, so the material is the one the separate
// shell's surfaces got.
float glassFixed(double value);

// FishUI's GlassEffect defaults (glassmaterial.h), enabled, before a
// component sets its own values.
render::GlassParams fishuiGlass();
// FishUI's PopupGlass: tooltips, menus, the OSD; `tint` premultiplied, from
// rgba components rounded to 16 bits each.
render::GlassParams popupGlass(double red, double green, double blue, double alpha);

// The dock's and the top bar's shared glass, by BarThemeSpec::glassTint:
// 0 is More Clear, 1 More Tinted, 0.5 the default.
render::GlassParams barGlass(double tint);

} // namespace bayu::shell

#endif // BAYU_SHELL_SHELL_DATA_H_
