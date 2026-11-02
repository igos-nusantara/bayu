// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Modelled on chromium/src/ash/wm/workspace/workspace_window_resizer.cc
// (revision 7acd40ddefff49c9d36144999aafdbb674dbd07a): GetNonSnapWorkArea,
// GetActiveDragAreaForSnapAndMaximize and GetSnapType, plus the snapped bounds
// ash::GetSnappedWindowBounds computes.
//
// Only Ash's landscape layout is ported: left and right snap to half the work
// area, the top maximizes. Bayu adds the corners, which snap to a quarter
// (#149). Ash's portrait orientation, where the top and the
// bottom snap instead, has no Bayu equivalent and is left out. The snap
// ratio is always a half; Ash carries a per-window ratio for its resizable
// split view.

#ifndef BAYU_WM_WINDOW_SNAP_H_
#define BAYU_WM_WINDOW_SNAP_H_

#include "base/geometry.h"
#include "wm/types.h"

namespace bayu::wm {

// What the edge the pointer is dragging against asks for.
enum class SnapType {
    None,
    Primary, // Left half.
    Secondary, // Right half.
    Maximize,
    TopLeft, // Quarters.
    TopRight,
    BottomLeft,
    BottomRight,
};

// The area a drag can move through without asking for anything. Outside it, on
// one of the screen edges, is where a snap or a maximize is triggered. The
// tolerance is only added on the edges where the work area reaches the output:
// past a dock the pointer has to cross the dock first.
base::Rect nonSnapArea(const base::Rect &workArea, const base::Rect &outputBounds);

// `location` is in output coordinates. Returns what a drag ending there would
// ask for, before any window constraint is taken into account.
SnapType snapTypeAt(const base::PointF &location, const base::Rect &workArea, const base::Rect &outputBounds);

// Empty for SnapType::None.
base::Rect snapBounds(SnapType type, const base::Rect &workArea);

WindowStateType snapStateType(SnapType type);
// The snap a snapped state is; None for the others.
SnapType snapTypeOf(WindowStateType type);

// A window larger than half the work area (than a quarter, for a corner)
// cannot be snapped into it.
bool canSnapSize(const base::Size &minimumOuterSize, const base::Rect &workArea,
                 SnapType type = SnapType::Primary);

// Whether `bounds` still look snapped: full work-area height, against one of
// its vertical edges (a quarter: in its corner). Ash's
// WorkspaceWindowResizer::AreBoundsValidSnappedBounds.
bool areBoundsValidSnappedBounds(WindowStateType type, const base::Rect &bounds, const base::Rect &workArea);

const char *toString(SnapType type);

} // namespace bayu::wm

#endif // BAYU_WM_WINDOW_SNAP_H_
