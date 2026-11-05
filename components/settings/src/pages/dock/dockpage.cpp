// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include <algorithm>
#include <cmath>
#include <vector>

#include "base/i18n.h"
#include "pages/dock/docksettings.h"
#include "ui/layout.h"
#include "ui/cell_layout.h"
#include "ui/segmented_control.h"
#include "widgets/card.h"
#include "widgets/imageselectionitem.h"
#include "widgets/listrow.h"
#include "widgets/metrics.h"
#include "widgets/picture.h"
#include "widgets/sections.h"
#include "widgets/sliderrow.h"
#include "window/page.h"
#include "window/pagelist.h"
#include "window/settingswindow.h"

using namespace bayu::ui;
using bayu::i18n::tr;

namespace settings {

namespace {

class DockPage : public Page
{
public:
    explicit DockPage(SettingsWindow& window)
        : Page(window, tr("Desktop"))
    {
        // The layout: the top bar with the dock, or the panel alone.
        Section* layout = content()->addChild<Section>(tr("Layout"));
        Card* layouts = layout->addChild<Card>();
        RowLayout* layoutTiles = layouts->addChild<RowLayout>();
        layoutTiles->setSpacing(metrics::tileSpacing);
        const char* const layoutNames[] = {tr("Dock"), tr("Panel")};
        const char* const layoutPictures[] = {"images/layout_dock.svg", "images/layout_panel.svg"};
        for (int i = 0; i < 2; ++i) {
            ImageSelectionItem* tile = layoutTiles->addChild<ImageSelectionItem>(dataPath(layoutPictures[i]),
                                                                                 layoutNames[i], SizeF{80, 80});
            tile->onClicked = [this, i] {
                m_settings.setLayout(i);
                updateLayout();
            };
            m_layouts.push_back(tile);
        }
        content()->addChild<Footnote>(tr("Takes effect right away."));

        // The panel's.
        Section* panel = content()->addChild<Section>(tr("Panel"));
        m_panelViews.push_back(panel);
        Card* panelCard = panel->addChild<Card>(Card::Style::List);
        SwitchRow* floating = panelCard->addChild<SwitchRow>(tr("Float when no window is open"));
        floating->setName("panel:floating");
        floating->setChecked(m_settings.panelFloating());
        floating->onToggled = [this](bool checked) {
            m_settings.setPanelFloating(checked);
            updateLayout();
        };
        ListRow* iconSize = panelCard->addChild<ListRow>(tr("Icon size"));
        iconSize->setInteractive(false);
        auto* sizes = iconSize->addContent<SegmentedControl>(std::vector<std::string>{tr("Small"), tr("Medium")});
        sizes->setName("panel:icon-size");
        // Both words whole, beside the row's label.
        sizes->setLayoutHints({.preferredWidth = std::max(220.0, sizes->sizeHint().width)});
        sizes->setCurrentIndex(m_settings.panelIconSize() == 32 ? 0 : 1);
        sizes->onCurrentChanged = [this](int index) { m_settings.setPanelIconSize(index == 0 ? 32 : 36); };
        ListRow* openApps = panelCard->addChild<ListRow>(tr("Open applications"));
        openApps->setInteractive(false);
        auto* styles = openApps->addContent<SegmentedControl>(std::vector<std::string>{tr("Icons"), tr("Titles")});
        styles->setName("panel:window-titles");
        styles->setLayoutHints({.preferredWidth = std::max(220.0, styles->sizeHint().width)});
        styles->setCurrentIndex(m_settings.panelWindowTitles() ? 1 : 0);
        styles->onCurrentChanged = [this](int index) { m_settings.setPanelWindowTitles(index == 1); };
        SliderRow* panelGlass = panelCard->addChild<SliderRow>(tr("Glass effect"));
        panelGlass->setName("panel:glass");
        panelGlass->setCaptions(tr("More clear"), tr("More tinted"));
        panelGlass->setRange(0, 1);
        panelGlass->setValue(m_settings.panelGlassTint());
        panelGlass->onMoved = [this, panelGlass](double value) {
            m_settings.setPanelGlassTint(value);
            panelGlass->setValue(m_settings.panelGlassTint());
        };
        m_floatingNote = content()->addChild<Footnote>();
        m_panelViews.push_back(m_floatingNote);
        Card* panelCorners = content()->addChild<Card>(Card::Style::List);
        m_panelViews.push_back(panelCorners);
        NavigationItem* panelHotCorners = panelCorners->addChild<NavigationItem>(tr("Hot Corners"));
        panelHotCorners->onClicked = [&window] { window.pushPage("hotcorners"); };
        NavigationItem* panelTiling = panelCorners->addChild<NavigationItem>(tr("Tiling"));
        panelTiling->onClicked = [&window] { window.pushPage("tiling"); };
        m_panelViews.push_back(content()->addChild<Footnote>(
                tr("The panel has no top bar, so applications show their menus in their windows. Applications "
                   "already open may need to be opened again.")));

        // The dock's.
        Section* position = content()->addChild<Section>(tr("Position on screen"));
        m_dockViews.push_back(position);
        Card* positions = position->addChild<Card>();
        RowLayout* tiles = positions->addChild<RowLayout>();
        tiles->setSpacing(metrics::tileSpacing);
        const char* const names[] = {tr("Left"), tr("Bottom"), tr("Right")};
        const char* const pictures[] = {"images/dock_left.svg", "images/dock_bottom.svg", "images/dock_right.svg"};
        for (int i = 0; i < 3; ++i) {
            ImageSelectionItem* tile = tiles->addChild<ImageSelectionItem>(dataPath(pictures[i]), names[i],
                                                                           SizeF{80, 80});
            tile->onClicked = [this, i] {
                m_settings.setDirection(i);
                updateDirection();
            };
            m_directions.push_back(tile);
        }
        updateDirection();

        Card* sliders = content()->addChild<Card>(Card::Style::List);
        m_dockViews.push_back(sliders);
        SliderRow* size = sliders->addChild<SliderRow>(tr("Size"));
        size->setName("dock:size");
        size->setCaptions(tr("Small"), tr("Huge"));
        // The value is the icon size itself; the dock derives its paddings
        // from it (54 is the design baseline).
        size->setRange(32, 128);
        size->setStepSize(1);
        size->setValue(m_settings.iconSize());
        // The dock reports no changes back, so the row learns the new value
        // here; otherwise it returns to the old one once the move settles.
        size->onMoved = [this, size](double value) {
            m_settings.setIconSize(int(std::round(value)));
            size->setValue(m_settings.iconSize());
        };
        SliderRow* glass = sliders->addChild<SliderRow>(tr("Glass effect"));
        glass->setName("dock:glass");
        glass->setCaptions(tr("More clear"), tr("More tinted"));
        glass->setRange(0, 1);
        glass->setValue(m_settings.glassTint());
        glass->onMoved = [this, glass](double value) {
            m_settings.setGlassTint(value);
            glass->setValue(m_settings.glassTint());
        };

        Section* mode = content()->addChild<Section>(tr("Display mode"));
        m_dockViews.push_back(mode);
        auto* visibility = mode->addChild<SegmentedControl>(
                std::vector<std::string>{tr("Always show"), tr("Always hide"), tr("Smart hide")});
        visibility->setLayoutHints({.fillWidth = true});
        visibility->setCurrentIndex(m_settings.visibility());
        visibility->onCurrentChanged = [this](int index) { m_settings.setVisibility(index); };

        Card* corners = content()->addChild<Card>(Card::Style::List);
        m_dockViews.push_back(corners);
        NavigationItem* hotCorners = corners->addChild<NavigationItem>(tr("Hot Corners"));
        hotCorners->onClicked = [&window] { window.pushPage("hotcorners"); };
        NavigationItem* tiling = corners->addChild<NavigationItem>(tr("Tiling"));
        tiling->onClicked = [&window] { window.pushPage("tiling"); };

        Card* minimized = content()->addChild<Card>(Card::Style::List);
        m_dockViews.push_back(minimized);
        SwitchRow* area = minimized->addChild<SwitchRow>(tr("Minimized windows area"));
        area->setChecked(m_settings.minimizedArea());
        area->onToggled = [this](bool checked) { m_settings.setMinimizedArea(checked); };

        updateLayout();
    }

private:
    void updateLayout()
    {
        const bool panel = m_settings.layout() == 1;
        for (size_t i = 0; i < m_layouts.size(); ++i)
            m_layouts[i]->setChecked(m_settings.layout() == int(i));
        for (View* view : m_panelViews)
            view->setVisible(panel);
        for (View* view : m_dockViews)
            view->setVisible(!panel);
        m_floatingNote->setText(m_settings.panelFloating()
                                        ? tr("The panel stands 16 px off the screen's edge with round corners while "
                                             "the desktop is empty, and lies on the edge once a window is open.")
                                        : tr("The panel always lies on the bottom edge of the screen."));
    }

    void updateDirection()
    {
        for (size_t i = 0; i < m_directions.size(); ++i)
            m_directions[i]->setChecked(m_settings.direction() == int(i));
    }

    DockSettings m_settings;
    std::vector<ImageSelectionItem*> m_directions;
    std::vector<ImageSelectionItem*> m_layouts;
    std::vector<View*> m_panelViews;
    std::vector<View*> m_dockViews;
    Footnote* m_floatingNote = nullptr;
};

} // namespace

static const PageRegistration registration("dock", [](SettingsWindow& window) -> std::unique_ptr<Page> {
    return std::make_unique<DockPage>(window);
});

} // namespace settings
