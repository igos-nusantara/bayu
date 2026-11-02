// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <vector>

#include "base/application_registry.h"
#include "base/desktop_entry.h"
#include "base/i18n.h"
#include "pages/tiling/tiling.h"
#include "ui/button.h"
#include "ui/icon.h"
#include "ui/label.h"
#include "ui/theme.h"
#include "widgets/card.h"
#include "widgets/listrow.h"
#include "widgets/rowcombobox.h"
#include "widgets/sections.h"
#include "window/page.h"
#include "window/pagelist.h"

using namespace bayu::ui;
using bayu::base::ApplicationRegistry;
using bayu::i18n::format;
using bayu::i18n::tr;

namespace settings {

namespace {

// The gaps on offer, in pixels.
constexpr std::array<int, 5> kGaps = {0, 4, 8, 12, 16};

// An application that always floats: its name and icon, and Remove.
class FloatingAppRow : public ListRow
{
public:
    FloatingAppRow(const std::string& id, std::function<void()> remove)
    {
        setInteractive(false);
        const auto entry = ApplicationRegistry::instance().byId(id);
        setLeading<ThemeIcon>(entry && !entry->icon.empty() ? entry->icon : std::string("application-x-executable"),
                              24);
        Label* name = addContent<Label>(entry && !entry->name.empty() ? entry->name : id);
        name->setElide(Label::Elide::Right);
        name->setLayoutHints({.fillWidth = true});
        Button* button = addContent<Button>(tr("Remove"));
        button->onClicked = std::move(remove);
    }
};

class TilingPage : public Page
{
public:
    explicit TilingPage(SettingsWindow& window)
        : Page(window, tr("Tiling"))
    {
        m_unavailable = content()->addChild<Card>();
        m_unavailableLabel = m_unavailable->addChild<Label>(
                tr("Tiling is managed by the Bayu compositor, which is not running."));
        m_unavailableLabel->setWrap(true);
        m_unavailableLabel->setLayoutHints({.fillWidth = true});

        m_options = content()->addChild<Card>(Card::Style::List);
        m_tile = m_options->addChild<SwitchRow>(tr("Tile windows on every desk"));
        m_tile->onToggled = [this](bool checked) { m_tiling.setTilesByDefault(checked); };

        ListRow* gap = m_options->addChild<ListRow>(tr("Gap between windows"));
        gap->setInteractive(false);
        std::vector<std::string> gaps;
        for (const int pixels : kGaps)
            gaps.push_back(pixels == 0 ? tr("None") : format(tr("{pixels} px"), {{"pixels", std::to_string(pixels)}}));
        m_gap = gap->addContent<RowComboBox>(gaps);
        m_gap->onActivated = [this](int index) { m_tiling.setGap(kGaps[size_t(index)]); };

        m_keys = content()->addChild<Footnote>(
                tr("Meta+Y tiles or untiles the current desk, as does the tiling button in the status bar. On a tiled "
                   "desk, Meta+arrow keys move between windows, Meta+Shift+arrow keys swap them, Meta+Alt+arrow "
                   "keys resize them and Meta+G lets a window float. Drag a window onto another to swap them."));

        m_floating = content()->addChild<Section>(tr("Always float"));
        m_list = m_floating->addChild<Card>(Card::Style::List);
        ListRow* add = m_list->addChild<ListRow>(tr("Add an app"));
        add->setInteractive(false);
        m_add = add->addContent<RowComboBox>();
        m_add->onActivated = [this](int index) {
            if (index <= 0 || index > int(m_choices.size()))
                return;
            std::vector<std::string> applications = m_tiling.floatingApplications();
            applications.push_back(m_choices[size_t(index - 1)]);
            m_tiling.setFloatingApplications(applications);
        };
        m_note = m_floating->addChild<Footnote>(
                tr("Windows of these apps never tile, nor do dialogs and windows too large for a tile."));

        m_subscription = m_tiling.changed.connect([this] { update(); });
        // Names and icons come once the registry has read the desktop entries.
        m_registry = ApplicationRegistry::instance().changed.connect([this] { update(); });
        update();
    }

protected:
    void layout() override
    {
        m_unavailableLabel->setColor(Theme::current().disabledText);
        Page::layout();
    }

private:
    void update()
    {
        const bool available = m_tiling.isAvailable();
        m_unavailable->setVisible(!available);
        m_options->setVisible(available);
        m_keys->setVisible(available);
        m_floating->setVisible(available);
        if (!available)
            return;

        m_tile->setChecked(m_tiling.tilesByDefault());
        const auto gap = std::find(kGaps.begin(), kGaps.end(), m_tiling.gap());
        m_gap->setCurrentIndex(gap == kGaps.end() ? 2 : int(gap - kGaps.begin()));

        const std::vector<std::string> floating = m_tiling.floatingApplications();
        for (ListRow* row : m_rows)
            m_list->takeChild(row);
        m_rows.clear();
        for (const std::string& id : floating) {
            m_rows.push_back(m_list->addChild<FloatingAppRow>(id, [this, id] {
                std::vector<std::string> applications = m_tiling.floatingApplications();
                applications.erase(std::remove(applications.begin(), applications.end(), id), applications.end());
                m_tiling.setFloatingApplications(applications);
            }));
        }

        // Every app not floating yet, by name.
        m_choices.clear();
        std::vector<RowComboBox::Item> items {{tr("Choose…"), {}}};
        for (const auto& entry : ApplicationRegistry::instance().applications()) {
            if (std::find(floating.begin(), floating.end(), entry->id) != floating.end())
                continue;
            m_choices.push_back(entry->id);
            items.push_back({entry->name.empty() ? entry->id : entry->name, entry->icon});
        }
        m_add->setItems(items);
        m_add->setCurrentIndex(0);
    }

    Tiling m_tiling;
    bayu::base::Subscription m_subscription;
    bayu::base::Subscription m_registry;
    Card* m_unavailable;
    Label* m_unavailableLabel;
    Card* m_options;
    SwitchRow* m_tile;
    RowComboBox* m_gap;
    Footnote* m_keys;
    Section* m_floating;
    Card* m_list;
    RowComboBox* m_add;
    Footnote* m_note;
    std::vector<ListRow*> m_rows;
    std::vector<std::string> m_choices;
};

} // namespace

static const PageRegistration registration("tiling", [](SettingsWindow& window) -> std::unique_ptr<Page> {
    return std::make_unique<TilingPage>(window);
});

} // namespace settings
