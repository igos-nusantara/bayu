// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "wm/toplevel_event_handler.h"

#include <linux/input-event-codes.h>

#include <algorithm>
#include <cmath>
#include <utility>

#include "compositor/compositor.h"
#include "core/logging.h"
#include "input/seat.h"
#include "scene/window_animation.h"
#include "wm/overview/overview_controller.h"
#include "wm/resize_cursor.h"
#include "wm/snap_drag_controller.h"
#include "wm/window.h"
#include "wm/window_manager.h"
#include "wm/window_resizer.h"
#include "wm/window_snap.h"
#include "wm/window_state.h"
#include "wm/wm_event.h"

namespace bayu::wm {

namespace {

constexpr int kHeldResizeTimeoutMs = 50;

scene::WindowAnimation *animationFor(Window *window)
{
    return dynamic_cast<scene::WindowAnimation *>(window->node());
}

// The cross-fade a drag leaves behind when it changes the window's bounds in
// one step: the old appearance dissolves into the new one instead of being
// replaced between two frames.
// Upstream: the CrossFadeAnimation calls in
// ash/wm/workspace/workspace_window_resizer.cc.
void crossFade(Window *window, const base::Rect &from, scene::WindowAnimation::Type type)
{
    scene::WindowAnimation *animation = animationFor(window);
    if (animation && from != window->bounds())
        animation->start(type, from, window->bounds());
}

} // namespace

ToplevelEventHandler::ToplevelEventHandler(WindowManager *manager)
    : m_manager(manager)
    , m_snapController(std::make_unique<SnapDragController>(manager))
{
}

ToplevelEventHandler::~ToplevelEventHandler()
{
    m_frameStarted.reset();
}

Window *ToplevelEventHandler::draggedWindow() const
{
    return m_resizer ? m_resizer->window() : nullptr;
}

ResizeEdge ToplevelEventHandler::draggedEdge() const
{
    return m_resizer ? m_resizer->edge() : ResizeEdge::None;
}

bool ToplevelEventHandler::beginMove(Window *window, const base::PointF &pointerPosition,
                                     std::optional<int32_t> touchId)
{
    return beginDrag(window, ResizeEdge::None, pointerPosition, touchId);
}

bool ToplevelEventHandler::beginResize(Window *window, ResizeEdge edge,
                                       const base::PointF &pointerPosition,
                                       std::optional<int32_t> touchId)
{
    if (edge == ResizeEdge::None || !window || !window->isResizable())
        return false;
    return beginDrag(window, edge, pointerPosition, touchId);
}

bool ToplevelEventHandler::beginDrag(Window *window, ResizeEdge edge,
                                     const base::PointF &pointerPosition,
                                     std::optional<int32_t> touchId)
{
    if (!window || window->isPopup() || m_resizer)
        return false;

    // A fullscreen window's bounds belong to the output, and a minimized one
    // has nothing on screen to grab. A maximized window can only be moved, not
    // resized: there is no free edge to pull.
    WindowState *state = window->state();
    if (state->isMinimized() || state->isFullscreen())
        return false;
    if (edge != ResizeEdge::None && !state->isNormal() && !state->isSnapped())
        return false;

    // Moving a tiled window pulls it back to the size it will restore to, from
    // the first motion past the threshold onwards.
    base::Rect restoreBounds;
    if (edge == ResizeEdge::None && !state->isNormal())
        restoreBounds = state->restoreBounds();

    m_initialStateType = state->stateType();
    m_initialRestoreBounds = state->restoreBounds();

    m_resizer = std::make_unique<WindowResizer>(window, pointerPosition, edge,
                                                m_manager->workAreaFor(window), restoreBounds,
                                                m_manager->visibleAreaFor(window));
    m_touchId = touchId;
    CF_DEBUG(cfInputTransitions) << "drag begin target" << window << "edge" << toString(edge)
                                << (touchId ? "touch point" : "pointer") << touchId.value_or(-1);
    window->setDraggedInternal(true);
    window->setResizingInternal(edge != ResizeEdge::None);
    if (edge != ResizeEdge::None && state->isSnapped())
        collectPartners(window, edge);
    if (edge == ResizeEdge::None)
        m_snapController->begin(window, pointerPosition);
    CF_DEBUG(cfCompositor) << "drag started:" << toString(edge) << "from"
                          << window->bounds();
    if (auto *seat = m_manager->seat()) {
        if (touchId) {
            // The client asked to be moved by this point; the rest of it
            // belongs to the drag, as ash transfers the touch sequence.
            seat->takeTouchPoint(*touchId);
        } else {
            seat->setCursorOverride(edge == ResizeEdge::None ? input::CursorShape::Grabbing
                                                             : resizeCursor(edge));
            // The client must drop its pointer state before restore changes its hit regions.
            seat->clearPointerFocus();
        }
    }
    return true;
}

bool ToplevelEventHandler::onPointerMotion(const base::PointF &location)
{
    if (!m_resizer)
        return false;
    if (!m_touchId)
        drag(location);
    return true;
}

bool ToplevelEventHandler::onTouchMotion(int32_t id, const base::PointF &location)
{
    if (!m_resizer || m_touchId != id)
        return false;
    drag(location);
    return true;
}

bool ToplevelEventHandler::onTouchUp(int32_t id)
{
    if (!m_resizer || m_touchId != id)
        return false;
    CF_DEBUG(cfInputTransitions) << "drag terminating touch up target" << draggedWindow();
    completeDrag(DragResult::Success);
    return true;
}

void ToplevelEventHandler::onTouchCancel()
{
    if (isTouchDrag())
        completeDrag(DragResult::Success);
}

void ToplevelEventHandler::drag(const base::PointF &location)
{
    compositor::Compositor *compositor = m_manager->compositor();
    if (!m_resizer->isResize() || !compositor) {
        applyMotion(location);
        return;
    }
    if (m_holdingResize) {
        m_heldResize = location;
        return;
    }
    if (!m_frameStarted) {
        m_frameStarted = compositor->frameStarted.connect([this](double) {
            releaseHeldResize();
        });
    }
    holdResize();
    applyMotion(location);
}

void ToplevelEventHandler::holdResize()
{
    m_holdingResize = true;
    // A backend without frames of its own, headless for one, still resizes.
    m_manager->runLater(std::chrono::milliseconds(kHeldResizeTimeoutMs), [this] {
        releaseHeldResize();
    });
}

void ToplevelEventHandler::releaseHeldResize()
{
    m_holdingResize = false;
    const auto location = std::exchange(m_heldResize, std::nullopt);
    if (!location || !m_resizer)
        return;
    holdResize();
    applyMotion(*location);
}

void ToplevelEventHandler::applyMotion(const base::PointF &location)
{
    const bool hadMoved = m_resizer->hasMovedOrResized();
    Window *window = m_resizer->window();
    const base::Rect previousBounds = window->bounds();
    if (!m_resizer->isResize()) {
        // A move is bounded by the display under the pointer, so a window can
        // be dragged onto another display.
        const base::Rect pointer(int(std::floor(location.x())), int(std::floor(location.y())),
                                 1, 1);
        m_resizer->setAreas(m_manager->workAreaFor(window, pointer),
                            m_manager->visibleAreaFor(window, pointer));
    }
    m_resizer->drag(location);
    // New bounds reach the client, and a client-facing callback can end the
    // drag or destroy the window from inside that call.
    if (!m_resizer)
        return;
    followPartners();
    if (!m_resizer)
        return;

    // Crossing the threshold is what actually takes the window out of its
    // tiled state, as far as everything the user can see is concerned: the
    // state machine only follows at the end of the drag.
    if (!hadMoved && m_resizer->hasMovedOrResized() && !m_resizer->restoreBounds().isEmpty()) {
        window->state()->clearRestoreBounds();
        // The one step that is not the pointer's doing: the jump from the tiled
        // bounds to the restored size under the cursor. It has to be armed
        // before the restored look is announced, because that is what tells the
        // shell surface to hold the cross-fade until the client has redrawn.
        crossFade(window, previousBounds, scene::WindowAnimation::Type::DragBounds);
        window->setRestoringFromDragInternal(true);
        if (!m_resizer)
            return;
    } else if (auto *animation = animationFor(window)) {
        // The rest of the drag keeps re-aiming it, so it dissolves onto a
        // window that is still moving.
        animation->setDragTarget(window->bounds());
    }

    if (!m_resizer->isResize() && m_resizer->hasMovedOrResized())
        m_snapController->update(location);
}

void ToplevelEventHandler::collectPartners(Window *window, ResizeEdge edge)
{
    // Only the windows sharing the edge exactly: on this desk and output, and
    // tiled against it, as the other half or quarter of the screen.
    const base::Rect bounds = window->bounds();
    const base::Rect workArea = m_manager->workAreaFor(window);
    for (Window *other : m_manager->windows()) {
        if (other == window || other->isPopup() || !other->isVisible() || !other->isResizable()
            || !other->state()->isSnapped() || !(m_manager->workAreaFor(other) == workArea))
            continue;
        const base::Rect b = other->bounds();
        const bool rows = b.y() < bounds.bottom() && bounds.y() < b.bottom();
        const bool columns = b.x() < bounds.right() && bounds.x() < b.right();
        Partner partner {other, b};
        partner.left = isRightEdge(edge) && rows && b.x() == bounds.right();
        partner.right = isLeftEdge(edge) && rows && b.right() == bounds.x();
        partner.top = isBottomEdge(edge) && columns && b.y() == bounds.bottom();
        partner.bottom = isTopEdge(edge) && columns && b.bottom() == bounds.y();
        if (partner.left || partner.right || partner.top || partner.bottom) {
            other->setResizingInternal(true);
            m_partners.push_back(partner);
        }
    }
}

void ToplevelEventHandler::followPartners()
{
    if (m_partners.empty())
        return;
    Window *window = m_resizer->window();
    // The edge stops where a partner would go below its minimum size.
    base::Rect bounds = window->bounds();
    int left = bounds.x(), top = bounds.y(), right = bounds.right(), bottom = bounds.bottom();
    for (const Partner &partner : m_partners) {
        if (!partner.window)
            continue;
        const base::Rect &b = partner.initialBounds;
        const base::Size min = partner.window->minimumSize();
        if (partner.left)
            right = std::min(right, b.right() - min.width());
        if (partner.right)
            left = std::max(left, b.x() + min.width());
        if (partner.top)
            bottom = std::min(bottom, b.bottom() - min.height());
        if (partner.bottom)
            top = std::max(top, b.y() + min.height());
    }
    const base::Rect clamped(left, top, right - left, bottom - top);
    if (!(clamped == bounds)) {
        bounds = clamped;
        window->setBounds(bounds);
        if (!m_resizer)
            return;
    }

    for (size_t i = 0; i < m_partners.size(); ++i) {
        const Partner partner = m_partners[i];
        if (!partner.window || !partner.window->state()->isSnapped())
            continue;
        const base::Rect &b = partner.initialBounds;
        int pl = b.x(), pt = b.y(), pr = b.right(), pb = b.bottom();
        if (partner.left)
            pl = bounds.right();
        if (partner.right)
            pr = bounds.x();
        if (partner.top)
            pt = bounds.bottom();
        if (partner.bottom)
            pb = bounds.y();
        const base::Rect next(pl, pt, pr - pl, pb - pt);
        if (!(next == partner.window->bounds()))
            partner.window->setBounds(next);
        if (!m_resizer)
            return;
    }
}

void ToplevelEventHandler::endPartners(bool revert)
{
    for (const Partner &partner : std::exchange(m_partners, {})) {
        if (!partner.window)
            continue;
        if (revert)
            partner.window->setBounds(partner.initialBounds);
        partner.window->setResizingInternal(false);
    }
}

bool ToplevelEventHandler::onPointerButtonReleased()
{
    if (!m_resizer)
        return false;
    if (m_touchId)
        return true;
    CF_DEBUG(cfInputTransitions) << "drag terminating release target" << draggedWindow();
    completeDrag(DragResult::Success);
    return true;
}

bool ToplevelEventHandler::onKeyPressed(uint32_t evdevCode)
{
    if (!m_resizer || evdevCode != KEY_ESC)
        return false;
    completeDrag(DragResult::Revert);
    return true;
}

void ToplevelEventHandler::settleDrag(WindowResizer *resizer, SnapType snapped,
                                      const base::Rect &initialRestoreBounds)
{
    // A drag that never passed its threshold changed nothing, and settles
    // nothing either.
    if (!resizer->hasMovedOrResized())
        return;

    Window *window = resizer->window();
    WindowState *state = window->state();

    // A keyboard shortcut may have maximized or minimized the window while it
    // was being dragged. That state is the newer request; the drag does not
    // get to overrule it.
    if (state->stateType() != m_initialStateType)
        return;

    if (snapped != SnapType::None) {
        // Where it came from is where it goes back to. Entering a state from
        // Normal saves the current bounds itself; a window that was already
        // tiled keeps what it had before the drag consumed it.
        if (!state->hasRestoreBounds()) {
            state->setRestoreBounds(initialRestoreBounds.isEmpty() ? resizer->initialBounds()
                                                                   : initialRestoreBounds);
        }
        window->snap(snapped);
        // Dropping a window back on the edge it already belongs to is not a
        // state change, so nothing has put it back where that state says it
        // goes; the drag left it under the pointer.
        if (state->stateType() == snapStateType(snapped)) {
            const base::Rect dragged = window->bounds();
            state->onEvent(WMEvent(WMEventType::WorkAreaChanged));
            crossFade(window, dragged, scene::WindowAnimation::Type::Bounds);
        }
        return;
    }

    if (state->isNormal()) {
        // The window was moved or resized by hand and stays where the user put
        // it; there is nothing left to restore to.
        state->clearRestoreBounds();
        return;
    }

    // Dragged out of a tiled state and released in the middle of the screen:
    // the state machine catches up with what is already on screen. A resize
    // that kept snapped bounds - the shared edge was pulled - stays snapped.
    if (state->isSnapped() && resizer->isResize()
        && areBoundsValidSnappedBounds(state->stateType(), window->bounds(),
                                       m_manager->workAreaFor(window))) {
        return;
    }
    state->saveCurrentBoundsForRestore();
    state->onEvent(WMEvent(WMEventType::Normal));
}

void ToplevelEventHandler::completeDrag(DragResult result)
{
    // The drag ends where the pointer last was; a reverted one does not care.
    if (result == DragResult::Success)
        releaseHeldResize();
    m_heldResize.reset();
    m_holdingResize = false;
    if (!m_resizer)
        return;

    CF_DEBUG(cfInputTransitions) << "drag finish target" << draggedWindow() << "reason"
                                << (result == DragResult::Success ? "release/explicit-success"
                                            : result == DragResult::Revert
                                            ? "escape/explicit-revert"
                                            : "window-destroyed");

    // Out of the member before anything below can call back into here: the
    // state changes at the end of a drag reach the client, and a client-facing
    // callback may start another drag or destroy the window.
    std::unique_ptr<WindowResizer> resizer = std::move(m_resizer);
    m_touchId.reset();
    const base::Rect initialRestoreBounds = std::exchange(m_initialRestoreBounds, base::Rect());
    CF_DEBUG(cfInputTransitions) << "drag capture cleared";
    const SnapType snapped = m_snapController->end();
    endPartners(result == DragResult::Revert);
    Window *assist = nullptr;

    if (result != DragResult::WindowDestroyed) {
        Window *window = resizer->window();
        if (result == DragResult::Revert) {
            resizer->revertDrag();
            if (auto *animation = animationFor(window))
                animation->setDragTarget(window->bounds());
            // Everything the drag took away comes back: the restore bounds it
            // cleared, and the tiled look it replaced.
            if (!initialRestoreBounds.isEmpty())
                window->state()->setRestoreBounds(initialRestoreBounds);
        } else {
            resizer->completeDrag();
            settleDrag(resizer.get(), snapped, initialRestoreBounds);
            if ((snapped == SnapType::Primary || snapped == SnapType::Secondary)
                && window->stateType() != m_initialStateType
                && window->stateType() == snapStateType(snapped))
                assist = window;
        }
        // Cleared after the state change, so the window is never drawn tiled
        // at the bounds the pointer left it at.
        window->setRestoringFromDragInternal(false);
        window->setResizingInternal(false);
        window->setDraggedInternal(false);
    }

    // Completion callbacks may already have started another drag. Preserve
    // its override, and notify only after this drag is no longer used.
    if (!m_resizer) {
        if (auto *seat = m_manager->seat())
            seat->setCursorOverride(std::nullopt);
        if (assist && m_manager->hasWindow(assist) && m_manager->overview())
            m_manager->overview()->enterSnapAssist(assist);
    }
}

void ToplevelEventHandler::onWindowRemoving(Window *window)
{
    for (Partner &partner : m_partners) {
        if (partner.window == window)
            partner.window = nullptr;
    }
    if (m_resizer && m_resizer->window() == window)
        completeDrag(DragResult::WindowDestroyed);
}

} // namespace bayu::wm
