// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "wm/window_snap.h"

#include <algorithm>

namespace bayu::wm {

namespace {

// Ash's kScreenEdgeInsetForSnappingSides / kScreenEdgeInsetForSnappingTop. The
// top is less aggressive: the caption is what gets dragged there, and a wide
// band would snap windows the user only wanted to align.
constexpr int kSideTolerance = 32;
constexpr int kTopTolerance = 8;
// How far from a corner, along the edge the pointer is on, a drag snaps to
// that corner's quarter: an eighth of the side, at least this.
constexpr int kMinimumCornerSize = 64;

int cornerSize(int length)
{
    return std::max(kMinimumCornerSize, length / 8);
}

} // namespace

base::Rect nonSnapArea(const base::Rect &workArea, const base::Rect &outputBounds)
{
    base::Rect area = workArea;
    if (area.isEmpty())
        return area;

    int top = 0;
    int left = 0;
    int right = 0;
    if (outputBounds.isEmpty() || area.top() == outputBounds.top())
        top = kTopTolerance;
    if (outputBounds.isEmpty() || area.left() == outputBounds.left())
        left = kSideTolerance;
    if (outputBounds.isEmpty() || area.right() == outputBounds.right())
        right = kSideTolerance;
    return area.adjusted(left, top, -right, 0);
}

SnapType snapTypeAt(const base::PointF &location, const base::Rect &workArea, const base::Rect &outputBounds)
{
    const base::Rect area = nonSnapArea(workArea, outputBounds);
    if (area.isEmpty())
        return SnapType::None;

    const int vertical = cornerSize(workArea.height());
    const bool top = location.y() <= workArea.top() + vertical;
    const bool bottom = location.y() >= workArea.bottom() - vertical;
    if (location.x() <= area.left())
        return top ? SnapType::TopLeft : bottom ? SnapType::BottomLeft : SnapType::Primary;
    if (location.x() >= area.right() - 1)
        return top ? SnapType::TopRight : bottom ? SnapType::BottomRight : SnapType::Secondary;
    if (location.y() > area.top())
        return SnapType::None;
    // Along the top edge, its ends are the top corners.
    const int horizontal = cornerSize(workArea.width());
    if (location.x() <= workArea.left() + horizontal)
        return SnapType::TopLeft;
    if (location.x() >= workArea.right() - horizontal)
        return SnapType::TopRight;
    return SnapType::Maximize;
}

base::Rect snapBounds(SnapType type, const base::Rect &workArea)
{
    switch (type) {
    case SnapType::Primary:
        return base::Rect(workArea.x(), workArea.y(), workArea.width() / 2, workArea.height());
    case SnapType::Secondary: {
        const int width = workArea.width() / 2;
        return base::Rect(workArea.right() - width, workArea.y(), width, workArea.height());
    }
    case SnapType::Maximize:
        return workArea;
    case SnapType::TopLeft:
    case SnapType::TopRight:
    case SnapType::BottomLeft:
    case SnapType::BottomRight: {
        const int width = workArea.width() / 2;
        const int height = workArea.height() / 2;
        const bool left = type == SnapType::TopLeft || type == SnapType::BottomLeft;
        const bool top = type == SnapType::TopLeft || type == SnapType::TopRight;
        return base::Rect(left ? workArea.x() : workArea.right() - width,
                          top ? workArea.y() : workArea.bottom() - height, width, height);
    }
    case SnapType::None:
        break;
    }
    return base::Rect();
}

WindowStateType snapStateType(SnapType type)
{
    switch (type) {
    case SnapType::Primary:
        return WindowStateType::PrimarySnapped;
    case SnapType::Secondary:
        return WindowStateType::SecondarySnapped;
    case SnapType::Maximize:
        return WindowStateType::Maximized;
    case SnapType::TopLeft:
        return WindowStateType::TopLeftSnapped;
    case SnapType::TopRight:
        return WindowStateType::TopRightSnapped;
    case SnapType::BottomLeft:
        return WindowStateType::BottomLeftSnapped;
    case SnapType::BottomRight:
        return WindowStateType::BottomRightSnapped;
    case SnapType::None:
        break;
    }
    return WindowStateType::Normal;
}

SnapType snapTypeOf(WindowStateType type)
{
    switch (type) {
    case WindowStateType::PrimarySnapped:
        return SnapType::Primary;
    case WindowStateType::SecondarySnapped:
        return SnapType::Secondary;
    case WindowStateType::TopLeftSnapped:
        return SnapType::TopLeft;
    case WindowStateType::TopRightSnapped:
        return SnapType::TopRight;
    case WindowStateType::BottomLeftSnapped:
        return SnapType::BottomLeft;
    case WindowStateType::BottomRightSnapped:
        return SnapType::BottomRight;
    default:
        break;
    }
    return SnapType::None;
}

bool canSnapSize(const base::Size &minimumOuterSize, const base::Rect &workArea, SnapType type)
{
    const bool quarter = type == SnapType::TopLeft || type == SnapType::TopRight || type == SnapType::BottomLeft
            || type == SnapType::BottomRight;
    return !workArea.isEmpty() && minimumOuterSize.width() <= workArea.width() / 2
            && minimumOuterSize.height() <= (quarter ? workArea.height() / 2 : workArea.height());
}

bool areBoundsValidSnappedBounds(WindowStateType type, const base::Rect &bounds, const base::Rect &workArea)
{
    if (!isSnappedStateType(type) || workArea.isEmpty())
        return false;
    if (isQuarterSnappedStateType(type)) {
        const bool left = type == WindowStateType::TopLeftSnapped || type == WindowStateType::BottomLeftSnapped;
        const bool top = type == WindowStateType::TopLeftSnapped || type == WindowStateType::TopRightSnapped;
        return (left ? bounds.x() == workArea.x() : bounds.right() == workArea.right())
                && (top ? bounds.y() == workArea.y() : bounds.bottom() == workArea.bottom());
    }
    if (bounds.y() != workArea.y() || bounds.height() != workArea.height())
        return false;
    return type == WindowStateType::PrimarySnapped ? bounds.x() == workArea.x()
                                                   : bounds.right() == workArea.right();
}

const char *toString(SnapType type)
{
    switch (type) {
    case SnapType::None:
        return "none";
    case SnapType::Primary:
        return "primary";
    case SnapType::Secondary:
        return "secondary";
    case SnapType::Maximize:
        return "maximize";
    case SnapType::TopLeft:
        return "top-left";
    case SnapType::TopRight:
        return "top-right";
    case SnapType::BottomLeft:
        return "bottom-left";
    case SnapType::BottomRight:
        return "bottom-right";
    }
    return "unknown";
}

} // namespace bayu::wm
