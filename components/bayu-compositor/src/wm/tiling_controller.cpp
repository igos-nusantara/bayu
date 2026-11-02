// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "wm/tiling_controller.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdlib>

#include "scene/window_animation.h"
#include "wm/activation.h"
#include "wm/desks/desk.h"
#include "wm/stacking.h"
#include "wm/toplevel_event_handler.h"
#include "wm/window.h"
#include "wm/window_manager.h"
#include "wm/window_state.h"

namespace bayu::wm {
namespace {

// A split never leaves either side less than this share of it.
constexpr double kMinimumRatio = 0.1;
// How far a key moves an edge.
constexpr int kResizeStep = 48;

// The two halves of a split of `rect`, `gap` apart.
void splitRect(const base::Rect &rect, bool vertical, double ratio, int gap, base::Rect *a,
               base::Rect *b)
{
    if (vertical) {
        const int height = std::max(0, rect.height() - gap);
        const int first = int(std::lround(height * ratio));
        *a = base::Rect(rect.x(), rect.y(), rect.width(), first);
        *b = base::Rect(rect.x(), rect.y() + first + gap, rect.width(), height - first);
    } else {
        const int width = std::max(0, rect.width() - gap);
        const int first = int(std::lround(width * ratio));
        *a = base::Rect(rect.x(), rect.y(), first, rect.height());
        *b = base::Rect(rect.x() + first + gap, rect.y(), width - first, rect.height());
    }
}

bool fits(const Window *window, const base::Rect &rect)
{
    const base::Size minimum = window->minimumSize();
    return minimum.width() <= rect.width() && minimum.height() <= rect.height();
}

base::Rect inset(const base::Rect &rect, int by)
{
    return base::Rect(rect.x() + by, rect.y() + by, std::max(0, rect.width() - 2 * by),
                      std::max(0, rect.height() - 2 * by));
}

base::Point centre(const base::Rect &rect)
{
    return {rect.x() + rect.width() / 2, rect.y() + rect.height() / 2};
}

} // namespace

TilingController::TilingController(WindowManager *manager)
    : m_manager(manager)
{
    m_manager->desks()->addObserver(this);
    m_windowAdded = m_manager->windowAdded.connect([this](Window *window) {
        observe(window);
        update(window);
    });
    m_windowRemoved = m_manager->windowRemoved.connect([this](Window *window) {
        if (isTiled(window))
            remove(window);
        if (m_observed.erase(window))
            window->removeObserver(this);
        m_floated.erase(window);
        m_tooLarge.erase(window);
        m_floatingBounds.erase(window);
    });
    // Outputs that come and go take their trees with them: every window is
    // tiled again on the output it is on now.
    m_outputsChanged = m_manager->outputsChanged.connect([this] {
        std::vector<Window *> windows;
        for (const auto &[window, leaf] : m_leaves)
            windows.push_back(leaf.tile->window);
        std::sort(windows.begin(), windows.end(),
                  [](const Window *a, const Window *b) { return a->id() < b->id(); });
        for (Window *window : windows)
            remove(window);
        for (Window *window : windows)
            update(window);
    });
    m_zonesChanged = m_manager->exclusiveZonesChanged.connect([this] { layoutAll(); });
}

TilingController::~TilingController()
{
    m_manager->desks()->removeObserver(this);
    for (Window *window : m_observed)
        window->removeObserver(this);
}

bool TilingController::isDeskTiled(const Desk *desk) const
{
    return m_tiledDesks.count(desk) != 0;
}

bool TilingController::isActiveDeskTiled() const
{
    return isDeskTiled(m_manager->desks()->activeDesk());
}

void TilingController::toggleActiveDesk()
{
    if (Desk *desk = m_manager->desks()->activeDesk())
        setDeskTiled(desk, !isDeskTiled(desk));
}

void TilingController::setDeskTiled(Desk *desk, bool tiled)
{
    if (!desk || isDeskTiled(desk) == tiled)
        return;
    if (tiled) {
        m_tiledDesks.insert(desk);
        // Bottom to top, each splitting the one before: the window on top
        // gets the smallest tile, the one at the bottom the largest.
        for (Window *window : std::vector<Window *>(m_manager->windows())) {
            if (!m_manager->hasWindow(window) || m_manager->desks()->deskOf(window) != desk)
                continue;
            m_bulk = true;
            // A snapped window gives up its half to the tree.
            if (window->state()->isSnapped() && canTile(window))
                window->restore();
            update(window);
            m_bulk = false;
        }
    } else {
        untile(desk);
    }
    if (desk == m_manager->desks()->activeDesk())
        changed();
}

void TilingController::setTilesByDefault(bool tiled)
{
    m_tilesByDefault = tiled;
    for (const auto &desk : m_manager->desks()->desks())
        setDeskTiled(desk.get(), tiled);
}

void TilingController::setGap(int gap)
{
    gap = std::clamp(gap, 0, 64);
    if (gap == m_gap)
        return;
    m_gap = gap;
    layoutAll();
}

void TilingController::setFloatingApplications(std::vector<std::string> applications)
{
    m_floatingApplications = std::move(applications);
    for (Window *window : std::vector<Window *>(m_observed.begin(), m_observed.end()))
        update(window);
}

void TilingController::toggleFloating(Window *window)
{
    if (!window)
        return;
    if (isTiled(window)) {
        remove(window);
        m_floated.insert(window);
        const auto bounds = m_floatingBounds.find(window);
        if (bounds != m_floatingBounds.end()) {
            window->setBounds(bounds->second);
            m_floatingBounds.erase(bounds);
        }
        return;
    }
    if (m_floated.erase(window) || m_tooLarge.erase(window))
        update(window);
}

bool TilingController::canTile(const Window *window) const
{
    if (window->isPopup() || window->isShellWindow() || window->transientParent()
        || window->isModal() || window->isSystemDialog() || !window->isResizable()
        || window->isAlwaysOnTop() || window->isDragPreview()
        || layerFor(window) != StackingLayer::Normal)
        return false;
    // Fixed size: there is no tile it would fill.
    const base::Size maximum = window->maximumSize();
    if (!maximum.isEmpty() && maximum == window->minimumSize())
        return false;
    if (m_floated.count(window) || m_tooLarge.count(window))
        return false;
    return std::find(m_floatingApplications.begin(), m_floatingApplications.end(),
                     window->applicationId())
            == m_floatingApplications.end();
}

void TilingController::update(Window *window)
{
    if (!m_manager->hasWindow(window))
        return;
    const Desk *desk = m_manager->desks()->deskOf(window);
    const bool wanted = desk && isDeskTiled(desk) && canTile(window)
            && window->state()->isNormal() && !window->isHidden();
    const auto it = m_leaves.find(window);
    if (it != m_leaves.end()) {
        if (wanted && it->second.tree->desk == desk
            && it->second.tree->output == m_manager->outputBoundsFor(window))
            return;
        remove(window);
    }
    if (wanted)
        insert(window);
}

TilingController::Tree *TilingController::treeFor(const Desk *desk, const base::Rect &output)
{
    for (Tree &tree : m_trees) {
        if (tree.desk == desk && tree.output == output)
            return &tree;
    }
    m_trees.push_back({desk, output, nullptr});
    return &m_trees.back();
}

void TilingController::insert(Window *window)
{
    const Desk *desk = m_manager->desks()->deskOf(window);
    const base::Rect output = m_manager->outputBoundsFor(window);
    const base::Rect area = inset(m_manager->workAreaForOutput(output), m_gap);
    Tree *tree = treeFor(desk, output);

    auto leaf = std::make_unique<Tile>();
    leaf->window = window;
    Tile *placed = leaf.get();
    if (!tree->root) {
        if (!fits(window, area)) {
            m_tooLarge.insert(window);
            m_trees.remove_if([tree](const Tree &other) { return &other == tree; });
            return;
        }
        leaf->rect = area;
        tree->root = std::move(leaf);
    } else {
        // Next to the active window, or, tiling a desk, the window before;
        // failing either, in the largest tile.
        Tile *target = nullptr;
        const Window *after = m_bulk ? m_lastInserted : m_manager->activation()->activeWindow();
        if (const auto it = m_leaves.find(after); it != m_leaves.end() && it->second.tree == tree)
            target = it->second.tile;
        if (!target) {
            for (const auto &[other, entry] : m_leaves) {
                if (entry.tree == tree
                    && (!target
                        || int64_t(entry.tile->rect.width()) * entry.tile->rect.height()
                                > int64_t(target->rect.width()) * target->rect.height()))
                    target = entry.tile;
            }
        }
        if (!target)
            return;
        // Along the longer side, as the tile is now.
        const bool vertical = target->rect.height() > target->rect.width();
        base::Rect first, second;
        splitRect(target->rect, vertical, 0.5, m_gap, &first, &second);
        if (!fits(target->window, first) || !fits(window, second)) {
            m_tooLarge.insert(window);
            return;
        }
        auto old = std::make_unique<Tile>();
        old->window = target->window;
        old->parent = target;
        old->rect = first;
        m_leaves[old->window].tile = old.get();
        leaf->parent = target;
        leaf->rect = second;
        target->window = nullptr;
        target->vertical = vertical;
        target->ratio = 0.5;
        target->a = std::move(old);
        target->b = std::move(leaf);
    }
    m_floatingBounds.emplace(window, window->bounds());
    m_leaves[window] = {placed, tree};
    m_lastInserted = window;
    window->setTiledInternal(true);
    layout(tree);
}

void TilingController::remove(Window *window)
{
    const auto it = m_leaves.find(window);
    if (it == m_leaves.end())
        return;
    const Leaf leaf = it->second;
    m_leaves.erase(it);
    if (m_lastInserted == window)
        m_lastInserted = nullptr;
    if (m_manager->hasWindow(window))
        window->setTiledInternal(false);

    Tree *tree = leaf.tree;
    Tile *parent = leaf.tile->parent;
    if (!parent) {
        m_trees.remove_if([tree](const Tree &other) { return &other == tree; });
        return;
    }
    // The other half takes the whole of what the two shared.
    std::unique_ptr<Tile> sibling =
            std::move(parent->a.get() == leaf.tile ? parent->b : parent->a);
    sibling->parent = parent->parent;
    std::unique_ptr<Tile> &slot = !parent->parent ? tree->root
            : parent->parent->a.get() == parent  ? parent->parent->a
                                                 : parent->parent->b;
    slot = std::move(sibling);
    layout(tree);
}

void TilingController::untile(const Desk *desk)
{
    std::vector<Window *> windows;
    for (const auto &[window, leaf] : m_leaves) {
        if (leaf.tree->desk == desk)
            windows.push_back(leaf.tile->window);
    }
    for (Window *window : windows) {
        remove(window);
        // Back where it was before it tiled, on the work area it is on now.
        const auto bounds = m_floatingBounds.find(window);
        if (bounds == m_floatingBounds.end())
            continue;
        const base::Rect area = m_manager->workAreaFor(window);
        base::Rect restored = bounds->second;
        restored.setSize(base::Size(std::min(restored.width(), area.width()),
                                    std::min(restored.height(), area.height())));
        restored.setX(std::clamp(restored.x(), area.x(), area.right() - restored.width()));
        restored.setY(std::clamp(restored.y(), area.y(), area.bottom() - restored.height()));
        const base::Rect from = window->bounds();
        window->setBounds(restored);
        if (auto *animation = dynamic_cast<scene::WindowAnimation *>(window->node()))
            animation->start(scene::WindowAnimation::Type::Bounds, from, restored);
        m_floatingBounds.erase(bounds);
    }
    m_tiledDesks.erase(desk);
}

void TilingController::layoutAll()
{
    for (Tree &tree : m_trees)
        layout(&tree);
}

void TilingController::layout(Tree *tree)
{
    if (tree->root)
        place(tree->root.get(), inset(m_manager->workAreaForOutput(tree->output), m_gap));
}

void TilingController::place(Tile *tile, const base::Rect &rect)
{
    tile->rect = rect;
    if (Window *window = tile->window) {
        const base::Rect from = window->bounds();
        if (from == rect)
            return;
        m_placing = true;
        window->setBounds(rect);
        m_placing = false;
        // Glides there, unless a drag is under way: that follows the pointer.
        if (!m_manager->eventHandler()->isDragging() && window->isVisible()) {
            if (auto *animation = dynamic_cast<scene::WindowAnimation *>(window->node()))
                animation->start(scene::WindowAnimation::Type::Bounds, from, rect);
        }
        return;
    }
    base::Rect a, b;
    splitRect(rect, tile->vertical, tile->ratio, m_gap, &a, &b);
    place(tile->a.get(), a);
    place(tile->b.get(), b);
}

Window *TilingController::neighbour(Window *window, Direction direction) const
{
    const auto self = m_leaves.find(window);
    if (self == m_leaves.end())
        return nullptr;
    const base::Rect from = window->bounds();
    const base::Point here = centre(from);
    Window *best = nullptr;
    int64_t bestScore = INT64_MAX;
    for (const auto &[other, leaf] : m_leaves) {
        if (other == window || leaf.tree->desk != self->second.tree->desk)
            continue;
        const base::Rect to = other->bounds();
        const base::Point there = centre(to);
        // How far it is that way, and how far off to the side.
        int distance = 0, overlap = 0, offset = 0;
        switch (direction) {
        case Direction::Left:
            if (there.x() >= from.x())
                continue;
            distance = from.x() - to.right();
            overlap = std::min(from.bottom(), to.bottom()) - std::max(from.y(), to.y());
            offset = std::abs(there.y() - here.y());
            break;
        case Direction::Right:
            if (there.x() <= from.right())
                continue;
            distance = to.x() - from.right();
            overlap = std::min(from.bottom(), to.bottom()) - std::max(from.y(), to.y());
            offset = std::abs(there.y() - here.y());
            break;
        case Direction::Up:
            if (there.y() >= from.y())
                continue;
            distance = from.y() - to.bottom();
            overlap = std::min(from.right(), to.right()) - std::max(from.x(), to.x());
            offset = std::abs(there.x() - here.x());
            break;
        case Direction::Down:
            if (there.y() <= from.bottom())
                continue;
            distance = to.y() - from.bottom();
            overlap = std::min(from.right(), to.right()) - std::max(from.x(), to.x());
            offset = std::abs(there.x() - here.x());
            break;
        }
        // Side by side first, then the nearest, then the most in line.
        const int64_t score = (overlap > 0 ? 0 : int64_t(1) << 40)
                + int64_t(std::max(0, distance)) * 100000 + offset;
        if (score < bestScore) {
            bestScore = score;
            best = leaf.tile->window;
        }
    }
    return best;
}

bool TilingController::focus(Window *window, Direction direction)
{
    Window *next = neighbour(window, direction);
    if (!next)
        return false;
    m_manager->activation()->activate(next);
    return true;
}

bool TilingController::swap(Window *window, Direction direction)
{
    Window *other = neighbour(window, direction);
    if (!other)
        return false;
    const Leaf a = m_leaves[window];
    const Leaf b = m_leaves[other];
    a.tile->window = other;
    b.tile->window = window;
    m_leaves[window] = b;
    m_leaves[other] = a;
    layout(a.tree);
    if (b.tree != a.tree)
        layout(b.tree);
    return true;
}

bool TilingController::resize(Window *window, Direction direction)
{
    const auto it = m_leaves.find(window);
    if (it == m_leaves.end())
        return false;
    const bool horizontal = direction == Direction::Left || direction == Direction::Right;
    const bool forward = direction == Direction::Right || direction == Direction::Down;
    // The edge on that side, or the other one when that side is the screen's.
    for (const bool facing : {true, false}) {
        for (Tile *child = it->second.tile, *split = child->parent; split;
             child = split, split = split->parent) {
            if (split->vertical == horizontal)
                continue;
            const bool first = split->a.get() == child;
            if ((forward ? first : !first) != facing)
                continue;
            const int length = (horizontal ? split->rect.width() : split->rect.height()) - m_gap;
            if (length <= 0)
                return false;
            const double step = double(forward ? kResizeStep : -kResizeStep) / length;
            split->ratio = std::clamp(split->ratio + step, kMinimumRatio, 1 - kMinimumRatio);
            layout(it->second.tree);
            return true;
        }
    }
    return false;
}

void TilingController::resizeTo(Window *window, ResizeEdge edge, const base::Rect &bounds)
{
    const auto it = m_leaves.find(window);
    if (it == m_leaves.end())
        return;
    // The split each dragged edge is the line of, if any: the window is in its
    // first half for a right or bottom edge, its second for a left or top one.
    // ponytail: ratios are clamped to 10-90%, not to the windows' minimum sizes.
    const auto follow = [&](bool vertical, bool inFirst, int line) {
        for (Tile *child = it->second.tile, *split = child->parent; split;
             child = split, split = split->parent) {
            if (split->vertical != vertical || (split->a.get() == child) != inFirst)
                continue;
            const int start = vertical ? split->rect.y() : split->rect.x();
            const int length = (vertical ? split->rect.height() : split->rect.width()) - m_gap;
            if (length > 0)
                split->ratio = std::clamp(double(line - start) / length, kMinimumRatio,
                                          1 - kMinimumRatio);
            return;
        }
    };
    if (isRightEdge(edge))
        follow(false, true, bounds.right());
    if (isLeftEdge(edge))
        follow(false, false, bounds.x() - m_gap);
    if (isBottomEdge(edge))
        follow(true, true, bounds.bottom());
    if (isTopEdge(edge))
        follow(true, false, bounds.y() - m_gap);
    layout(it->second.tree);
}

void TilingController::drop(Window *window, const base::PointF &location)
{
    const auto it = m_leaves.find(window);
    if (it == m_leaves.end())
        return;
    const base::Point point(int(std::floor(location.x())), int(std::floor(location.y())));
    for (const auto &[other, leaf] : m_leaves) {
        if (other == window || leaf.tree->desk != it->second.tree->desk
            || !other->bounds().contains(point))
            continue;
        // Dropped on another tiled window: the two change places.
        const Leaf a = it->second;
        const Leaf b = leaf;
        Window *swapped = b.tile->window;
        a.tile->window = swapped;
        b.tile->window = window;
        m_leaves[window] = b;
        m_leaves[swapped] = a;
        layout(a.tree);
        if (b.tree != a.tree)
            layout(b.tree);
        return;
    }
    // Anywhere else it goes back to its tile, or to a tile on the output it
    // was dropped on.
    const Leaf leaf = it->second;
    update(window);
    if (isTiled(window) && m_leaves[window].tree == leaf.tree)
        layout(leaf.tree);
}

void TilingController::observe(Window *window)
{
    if (window->isPopup() || window->isShellWindow() || !m_observed.insert(window).second)
        return;
    window->addObserver(this);
}

void TilingController::onDeskAdded(const Desk *desk)
{
    if (m_tilesByDefault)
        m_tiledDesks.insert(desk);
}

void TilingController::onDeskRemoved(const Desk *desk)
{
    // Its windows are on another desk already.
    m_trees.remove_if([desk](const Tree &tree) { return tree.desk == desk; });
    m_tiledDesks.erase(desk);
}

void TilingController::onDeskActivationChanged(const Desk *, const Desk *)
{
    changed();
}

void TilingController::onWindowDeskChanged(Window *window)
{
    if (m_observed.count(window))
        update(window);
}

void TilingController::onWindowStateTypeChanged(Window *window, WindowStateType)
{
    update(window);
}

void TilingController::onWindowBoundsChanged(Window *window, const base::Rect &)
{
    // Only a tiled window that went to another output by some other way than
    // a drag, which drop() settles.
    if (m_placing || !isTiled(window) || window->isDragged())
        return;
    if (m_leaves[window].tree->output != m_manager->outputBoundsFor(window))
        update(window);
}

void TilingController::onWindowHiddenChanged(Window *window)
{
    update(window);
}

void TilingController::onWindowTransientParentChanged(Window *window, Window *)
{
    update(window);
}

void TilingController::onWindowModalChanged(Window *window)
{
    update(window);
}

void TilingController::onWindowApplicationIdChanged(Window *window)
{
    update(window);
}

} // namespace bayu::wm
