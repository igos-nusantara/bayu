// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BAYU_WM_TILING_CONTROLLER_H_
#define BAYU_WM_TILING_CONTROLLER_H_

#include <list>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "base/geometry.h"
#include "base/signal.h"
#include "wm/desks/desks_controller.h"
#include "wm/types.h"
#include "wm/window_observer.h"

namespace bayu::wm {

class Desk;
class Window;
class WindowManager;

// Auto-tiling (#149), per desk, as Pop Shell and COSMIC do it: each output of
// a tiled desk is a binary tree of splits, a new window splits the active
// tile along its longer side, and the tiles fill the work area, with gaps.
//
// A tiled window is an ordinary window, in the Normal state, whose bounds the
// tree owns. Maximizing, minimizing or hiding one takes it out of the tree and
// coming back puts it in again. Dialogs, transient and fixed-size windows,
// windows too large for a tile and the applications set to always float are
// left where they are.
class TilingController : public DesksObserver, public WindowObserver
{
public:
    enum class Direction {
        Left,
        Right,
        Up,
        Down,
    };

    explicit TilingController(WindowManager *manager);
    TilingController(const TilingController &) = delete;
    TilingController &operator=(const TilingController &) = delete;
    ~TilingController() override;

    bool isDeskTiled(const Desk *desk) const;
    void setDeskTiled(Desk *desk, bool tiled);
    bool isActiveDeskTiled() const;
    void toggleActiveDesk();

    // Whether the desks tile by default: every desk follows a change of it,
    // and new desks start with it.
    bool tilesByDefault() const { return m_tilesByDefault; }
    void setTilesByDefault(bool tiled);
    int gap() const { return m_gap; }
    void setGap(int gap);
    // Application ids whose windows never tile.
    const std::vector<std::string> &floatingApplications() const { return m_floatingApplications; }
    void setFloatingApplications(std::vector<std::string> applications);

    bool isTiled(const Window *window) const { return m_leaves.count(window) != 0; }
    // Takes a tiled window out of its tile, or puts one floated this way back.
    void toggleFloating(Window *window);

    // Keyboard: the tiled window next to `window` gets the focus, swaps places
    // with it, or the edge of `window` on that side moves. False when there is
    // nothing that way.
    bool focus(Window *window, Direction direction);
    bool swap(Window *window, Direction direction);
    bool resize(Window *window, Direction direction);

    // A tiled window resized by hand moves the splits it shares with its
    // neighbours; one moved by hand swaps with the tiled window it was
    // dropped on, or goes back to its tile.
    void resizeTo(Window *window, ResizeEdge edge, const base::Rect &bounds);
    void drop(Window *window, const base::PointF &location);

    // The active desk was tiled or untiled.
    base::Signal<> changed;

    // DesksObserver:
    void onDeskAdded(const Desk *desk) override;
    void onDeskRemoved(const Desk *desk) override;
    void onDeskActivationChanged(const Desk *activated, const Desk *deactivated) override;
    void onWindowDeskChanged(Window *window) override;

    // WindowObserver:
    void onWindowStateTypeChanged(Window *window, WindowStateType oldType) override;
    void onWindowBoundsChanged(Window *window, const base::Rect &oldBounds) override;
    void onWindowHiddenChanged(Window *window) override;
    void onWindowTransientParentChanged(Window *window, Window *oldParent) override;
    void onWindowModalChanged(Window *window) override;
    void onWindowApplicationIdChanged(Window *window) override;

private:
    struct Tree;
    struct Tile {
        // A leaf holds a window; a split holds two tiles, side by side or,
        // when vertical, one above the other.
        Window *window = nullptr;
        bool vertical = false;
        double ratio = 0.5;
        std::unique_ptr<Tile> a;
        std::unique_ptr<Tile> b;
        Tile *parent = nullptr;
        base::Rect rect;
    };
    struct Tree {
        const Desk *desk = nullptr;
        base::Rect output;
        std::unique_ptr<Tile> root;
    };
    struct Leaf {
        Tile *tile = nullptr;
        Tree *tree = nullptr;
    };

    bool canTile(const Window *window) const;
    // Puts the window in its tree or takes it out, whichever it needs now.
    void update(Window *window);
    void insert(Window *window);
    void remove(Window *window);
    void layout(Tree *tree);
    void layoutAll();
    void place(Tile *tile, const base::Rect &rect);
    Tree *treeFor(const Desk *desk, const base::Rect &output);
    Window *neighbour(Window *window, Direction direction) const;
    void observe(Window *window);
    void untile(const Desk *desk);

    WindowManager *m_manager = nullptr;
    std::set<const Desk *> m_tiledDesks;
    std::list<Tree> m_trees;
    std::map<const Window *, Leaf> m_leaves;
    std::set<Window *> m_observed;
    // Floated by hand, until it is closed or floated back.
    std::set<const Window *> m_floated;
    // Too large for the tile it would get: floats until it is closed.
    std::set<const Window *> m_tooLarge;
    // Where each tiled window was before it tiled, to go back to.
    std::map<const Window *, base::Rect> m_floatingBounds;
    bool m_tilesByDefault = false;
    int m_gap = 8;
    std::vector<std::string> m_floatingApplications;
    // Bounds the controller sets itself are not a move to another output.
    bool m_placing = false;
    // Tiling a whole desk: each window splits the one before, not the
    // active one.
    bool m_bulk = false;
    const Window *m_lastInserted = nullptr;
    base::Subscription m_windowAdded;
    base::Subscription m_windowRemoved;
    base::Subscription m_outputsChanged;
    base::Subscription m_zonesChanged;
};

} // namespace bayu::wm

#endif // BAYU_WM_TILING_CONTROLLER_H_
