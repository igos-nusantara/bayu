// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BAYU_SHELL_BAR_THEME_H_
#define BAYU_SHELL_BAR_THEME_H_

namespace bayu::shell {

// The background material shared by the dock and the top bar. Default is the
// dock's existing glass; the others only change its tint and weight.
enum class BarTheme : int {
    Default = 0,
    Clear = 1,
    Tinted = 2,
};

struct BarThemeSpec {
    // The tint barGlass() turns into the shared material.
    double glassTint;
};

inline BarThemeSpec barTheme(BarTheme theme)
{
    switch (theme) {
    case BarTheme::Clear:
        return {0.15};
    case BarTheme::Tinted:
        return {0.85};
    case BarTheme::Default:
        break;
    }
    return {0.5};
}

inline BarTheme barThemeFromInt(int value)
{
    switch (value) {
    case int(BarTheme::Clear):
        return BarTheme::Clear;
    case int(BarTheme::Tinted):
        return BarTheme::Tinted;
    default:
        return BarTheme::Default;
    }
}

} // namespace bayu::shell

#endif // BAYU_SHELL_BAR_THEME_H_
