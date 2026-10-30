// SPDX-License-Identifier: BSD-3-Clause

#include <chrono>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/i18n.h"
#include "base/timer.h"
#include "pages/about/aboutinfo.h"
#include "pages/memory/zram.h"
#include "widgets/card.h"
#include "widgets/listrow.h"
#include "widgets/rowcombobox.h"
#include "widgets/sections.h"
#include "window/admin.h"
#include "window/page.h"
#include "window/pagelist.h"
#include "window/settingswindow.h"

using namespace bayu;
using namespace bayu::ui;
using bayu::i18n::format;
using bayu::i18n::tr;
using settings::memory::ZramMode;

namespace settings {

namespace {

// The sizes in the order the list shows them.
constexpr ZramMode kSizes[] = {ZramMode::Auto, ZramMode::Quarter, ZramMode::Half};
constexpr auto kRefresh = std::chrono::seconds(2);

class MemoryPage : public Page
{
public:
    explicit MemoryPage(SettingsWindow& window)
        : Page(window, tr("Memory"))
    {
        Section* compression = content()->addChild<Section>(tr("Memory compression"));
        Card* card = compression->addChild<Card>(Card::Style::List);
        m_enabled = card->addChild<SwitchRow>(tr("Compress memory"));
        m_enabled->setName("memory:zram");
        m_enabled->onToggled = [this](bool checked) {
            apply(checked ? kSizes[std::max(0, m_size->currentIndex())] : ZramMode::Off);
        };
        m_sizeRow = card->addChild<ListRow>(tr("Size"));
        m_sizeRow->setName("memory:size");
        m_sizeRow->setInteractive(false);
        m_size = m_sizeRow->addContent<RowComboBox>(
                std::vector<std::string>{tr("Automatic"), tr("25% of memory"), tr("50% of memory")});
        m_size->onActivated = [this](int index) { apply(kSizes[index]); };
        m_note = compression->addChild<Footnote>();
        m_note->setName("memory:note");

        m_usage = content()->addChild<Section>(tr("Usage"));
        Card* usage = m_usage->addChild<Card>(Card::Style::List);
        m_total = usage->addChild<InfoRow>(tr("Memory"));
        m_swap = usage->addChild<InfoRow>(tr("Compressed swap"));
        m_stored = usage->addChild<InfoRow>(tr("Swapped out"));
        m_used = usage->addChild<InfoRow>(tr("Memory it takes"));
        m_ratio = usage->addChild<InfoRow>(tr("Compression ratio"));

        m_timer.onTimeout = [this] { updateUsage(); };
        m_timer.start(kRefresh);
        update();
    }

    ~MemoryPage() override { *m_alive = false; }

private:
    void update(const std::string& error = {})
    {
        const ZramMode mode = memory::zramMode();
        m_enabled->setChecked(mode != ZramMode::Off);
        m_sizeRow->setVisible(mode != ZramMode::Off);
        for (int i = 0; i < int(std::size(kSizes)); ++i) {
            if (kSizes[i] == mode)
                m_size->setCurrentIndex(i);
        }
        m_note->setText(!error.empty()
                                ? error
                                : tr("Memory that is not in use is kept compressed in memory, so more applications "
                                     "and Android apps fit before the computer slows down, as on phones. Changes "
                                     "apply at once."));
        updateUsage();
    }

    void updateUsage()
    {
        const memory::ZramUsage usage = memory::zramUsage();
        m_total->setValue(about::memorySize());
        m_usage->setVisible(usage.size > 0);
        m_swap->setValue(about::formatByteSize(double(usage.size)));
        m_stored->setValue(about::formatByteSize(double(usage.stored)));
        m_used->setValue(about::formatByteSize(double(usage.memoryUsed)));
        char ratio[32] = "–";
        // Below a megabyte, what zram takes is mostly its own bookkeeping.
        if (usage.memoryUsed > 0 && usage.stored >= 1024 * 1024)
            std::snprintf(ratio, sizeof ratio, "%.1f×", double(usage.stored) / double(usage.memoryUsed));
        m_ratio->setValue(ratio);
    }

    void apply(ZramMode mode)
    {
        m_enabled->setEnabled(false);
        m_size->setEnabled(false);
        admin::run({"zram", memory::zramArgument(mode)}, m_alive, [this](const std::optional<std::string>& error) {
            m_enabled->setEnabled(true);
            m_size->setEnabled(true);
            update(error.value_or(std::string()));
        });
    }

    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
    base::Timer m_timer;
    SwitchRow* m_enabled;
    ListRow* m_sizeRow;
    RowComboBox* m_size;
    Footnote* m_note;
    Section* m_usage;
    InfoRow* m_total;
    InfoRow* m_swap;
    InfoRow* m_stored;
    InfoRow* m_used;
    InfoRow* m_ratio;
};

} // namespace

static const PageRegistration registration("memory", [](SettingsWindow& window) -> std::unique_ptr<Page> {
    return std::make_unique<MemoryPage>(window);
});

} // namespace settings
