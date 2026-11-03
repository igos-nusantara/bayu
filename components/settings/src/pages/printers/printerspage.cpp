// SPDX-License-Identifier: BSD-3-Clause

#include <algorithm>
#include <chrono>
#include <map>
#include <set>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "base/event_loop.h"
#include "base/i18n.h"
#include "base/timer.h"
#include "pages/printers/printers.h"
#include "ui/button.h"
#include "ui/combo_box.h"
#include "ui/cell_layout.h"
#include "ui/dialog_window.h"
#include "ui/icon.h"
#include "ui/label.h"
#include "ui/layout.h"
#include "ui/progress_bar.h"
#include "ui/text_field.h"
#include "ui/theme.h"
#include "widgets/card.h"
#include "widgets/formdialog.h"
#include "widgets/listrow.h"
#include "widgets/metrics.h"
#include "widgets/sections.h"
#include "widgets/standardbutton.h"
#include "window/page.h"
#include "window/pagelist.h"
#include "window/settingswindow.h"

using namespace bayu::ui;
using bayu::i18n::format;
using bayu::i18n::tr;
using bayu::i18n::trn;
using settings::printers::Printer;
using settings::printers::Printers;

namespace settings {

namespace {

// How long a look for printers on the network takes, and how often the page looks.
constexpr int kSearchMs = 2000;
constexpr auto kRefresh = std::chrono::seconds(4);
constexpr char kTestPage[] = "/usr/share/cups/data/testprint";

// A label in the theme's secondary text colour.
class SecondaryLabel : public Label
{
public:
    using Label::Label;

protected:
    void layout() override
    {
        setColor(Theme::current().disabledText);
        setFontSize(std::max(1.0, Theme::current().fontSize() - 1));
        Label::layout();
    }
};

// A printer: its name and state; clicked, its actions unfold below it.
class PrinterRow : public ListRow
{
public:
    explicit PrinterRow(bool found)
    {
        setLeading<ThemeIcon>("printer", 24);
        ColumnLayout* texts = addContent<ColumnLayout>();
        texts->setSpacing(metrics::textSpacing);
        texts->setLayoutHints({.fillWidth = true, .verticalAlignment = Alignment::Center});
        m_name = texts->addChild<Label>();
        m_name->setElide(Label::Elide::Right);
        m_name->setLayoutHints({.fillWidth = true});
        m_status = texts->addChild<SecondaryLabel>();
        m_status->setElide(Label::Elide::Right);
        m_status->setLayoutHints({.fillWidth = true});
        setMinimumRowHeight(metrics::twoLineRowHeight);
        m_busy = addContent<BusyIndicator>();
        m_busy->setLayoutHints({.preferredWidth = 18, .preferredHeight = 18});
        m_busy->setVisible(false);
        if (found) {
            // Found ones are added with one click.
            setHighlightOnPress(false);
            m_add = addContent<Button>(tr("Add"));
            return;
        }
        RowLayout* actions = details()->addChild<RowLayout>();
        actions->setSpacing(metrics::controlSpacing);
        LayoutHints hints;
        hints.margins.bottom = metrics::m;
        actions->setLayoutHints(hints);
        m_test = actions->addChild<StandardButton>(tr("Print Test Page"));
        m_default = actions->addChild<StandardButton>(tr("Set as Default"));
        m_cancel = actions->addChild<StandardButton>(tr("Cancel All Jobs"));
        m_remove = actions->addChild<StandardButton>(tr("Remove"));
        onClicked = [this] { setExpanded(!isExpanded()); };
    }

    void setPrinter(const Printer& printer)
    {
        m_printer = printer;
        setName("printer:" + printer.name);
        m_name->setText(printer.info);
        if (m_add) {
            const std::string kind = printer.uri.starts_with("usb://") ? std::string("USB") : std::string();
            const std::string detail = printer.model.empty() ? printer.uri : printer.model;
            m_status->setText(kind.empty() || detail == printer.info ? (kind.empty() ? detail : kind)
                                                                      : detail + " · " + kind);
            return;
        }
        std::string status = printer.state == 4 ? tr("Printing") : printer.state == 5 ? tr("Stopped") : tr("Ready");
        if (printer.jobs > 0)
            status += " · " + trn("{n} job", "{n} jobs", uint64_t(printer.jobs));
        if (printer.isDefault)
            status += std::string(" · ") + tr("Default");
        m_status->setText(status);
        m_default->setVisible(!printer.isDefault);
        m_cancel->setVisible(printer.jobs > 0);
    }

    void setBusy(bool busy)
    {
        m_busy->setVisible(busy);
        if (m_add)
            m_add->setVisible(!busy);
    }

    const Printer& printer() const { return m_printer; }
    Button* addButton() const { return m_add; }
    StandardButton* testButton() const { return m_test; }
    StandardButton* defaultButton() const { return m_default; }
    StandardButton* cancelButton() const { return m_cancel; }
    StandardButton* removeButton() const { return m_remove; }

private:
    Printer m_printer;
    Label* m_name = nullptr;
    Label* m_status = nullptr;
    BusyIndicator* m_busy = nullptr;
    Button* m_add = nullptr;
    StandardButton* m_test = nullptr;
    StandardButton* m_default = nullptr;
    StandardButton* m_cancel = nullptr;
    StandardButton* m_remove = nullptr;
};

class PrintersPage : public Page
{
public:
    explicit PrintersPage(SettingsWindow& window)
        : Page(window, tr("Printers"))
        , m_window(window)
    {
        Section* queues = content()->addChild<Section>(tr("Printers"));
        m_queues = queues->addChild<Card>(Card::Style::List);
        m_none = m_queues->addChild<ListRow>(tr("No printers yet"));
        m_none->setInteractive(false);
        m_message = queues->addChild<Footnote>();
        m_message->setVisible(false);

        Section* found = content()->addChild<Section>(tr("Printers Found"));
        m_found = found->addChild<Card>(Card::Style::List);
        m_searching = m_found->addChild<ListRow>(tr("Looking for printers…"));
        m_searching->setInteractive(false);

        Card* other = content()->addChild<Card>(Card::Style::List);
        NavigationItem* byAddress = other->addChild<NavigationItem>(tr("Add a Printer by Address"));
        byAddress->onClicked = [this] { showAddressDialog(); };
        content()->addChild<Footnote>(
                tr("Printers on this network and those plugged in by USB are found by themselves. USB printers "
                   "are usually set up as soon as they are plugged in."));

        m_timer.setSingleShot(true);
        m_timer.onTimeout = [this] { refresh(); };
        refresh();
    }

    ~PrintersPage() override { *m_alive = false; }

private:
    // Reads the printers off the UI thread, then shows them and looks again later.
    void refresh()
    {
        background<Printers>([](std::shared_ptr<Printers> result) { *result = printers::read(kSearchMs); },
                   [this](const Printers& printers) {
                       show(printers);
                       m_timer.start(kRefresh);
                   });
    }

    // Runs `work` on its own thread and `done` with what it made on this one,
    // while the page is still there.
    template<typename T>
    void background(std::function<void(std::shared_ptr<T>)> work, std::function<void(const T&)> done)
    {
        auto result = std::make_shared<T>();
        std::thread([loop = bayu::base::EventLoop::current()->handle(), alive = std::weak_ptr<bool>(m_alive),
                     work = std::move(work), done = std::move(done), result] {
            work(result);
            loop.post([alive, done, result] {
                if (const auto page = alive.lock(); page && *page)
                    done(*result);
            });
        }).detach();
    }

    // A CUPS command for `row`; the page reads the printers again after it.
    void command(PrinterRow* row, std::vector<std::string> arguments)
    {
        change(row, [arguments = std::move(arguments)] { return printers::run(arguments); });
    }

    // A change to the printers, off the UI thread: nothing when it worked,
    // else why not.
    void change(PrinterRow* row, std::function<std::optional<std::string>()> work)
    {
        if (row)
            row->setBusy(true);
        const std::string name = row ? row->printer().name : std::string();
        background<std::optional<std::string>>(
                [work = std::move(work)](std::shared_ptr<std::optional<std::string>> result) { *result = work(); },
                [this, name](const std::optional<std::string>& error) {
                    showMessage(error);
                    m_busy.erase(name);
                    m_timer.start(std::chrono::milliseconds(0));
                });
        if (row)
            m_busy.insert(name);
    }

    void showMessage(const std::optional<std::string>& error)
    {
        m_message->setText(error ? format(tr("Could not change the printers: {error}"), {{"error", *error}})
                                 : std::string());
        m_message->setVisible(error.has_value());
        requestLayout();
    }

    void show(const Printers& printers)
    {
        sync(m_queues, m_queueRows, printers.queues, false);
        sync(m_found, m_foundRows, printers.found, true);
        m_none->setVisible(printers.queues.empty());
        m_searching->setText(tr("No other printers found"));
        m_searching->setVisible(printers.found.empty());
        requestLayout();
    }

    // Keeps one row per printer, so an unfolded row stays unfolded.
    void sync(Card* card, std::map<std::string, PrinterRow*>& rows, const std::vector<Printer>& printers, bool found)
    {
        std::map<std::string, PrinterRow*> kept;
        for (const Printer& printer : printers) {
            PrinterRow* row = nullptr;
            if (const auto it = rows.find(printer.name); it != rows.end()) {
                row = it->second;
                rows.erase(it);
            } else {
                row = card->addChild<PrinterRow>(found);
                connect(row);
            }
            row->setPrinter(printer);
            row->setBusy(m_busy.contains(printer.name));
            kept[printer.name] = row;
        }
        for (const auto& [name, row] : rows)
            card->takeChild(row);
        rows = std::move(kept);
    }

    void connect(PrinterRow* row)
    {
        if (Button* add = row->addButton()) {
            add->onClicked = [this, row] { addFound(row); };
            return;
        }
        row->testButton()->onClicked = [this, row] { command(row, {"lp", "-d", row->printer().name, kTestPage}); };
        // The user's own default, as print dialogs read it.
        row->defaultButton()->onClicked = [this, row] { command(row, {"lpoptions", "-d", row->printer().name}); };
        row->cancelButton()->onClicked = [this, row] { command(row, {"cancel", "-a", row->printer().name}); };
        row->removeButton()->onClicked = [this, row] { command(row, {"lpadmin", "-x", row->printer().name}); };
    }

    // A found printer: with its matching driver, or one chosen when none matches.
    void addFound(PrinterRow* row)
    {
        struct Choice
        {
            std::string driver;
            std::vector<printers::Driver> drivers;
        };
        row->setBusy(true);
        const Printer printer = row->printer();
        m_busy.insert(printer.name);
        background<Choice>(
                [printer](std::shared_ptr<Choice> choice) {
                    choice->driver = printers::matchingDriver(printer);
                    if (choice->driver.empty())
                        choice->drivers = printers::drivers(printer);
                },
                [this, printer](const Choice& choice) {
                    m_busy.erase(printer.name);
                    if (const auto it = m_foundRows.find(printer.name); it != m_foundRows.end())
                        it->second->setBusy(false);
                    if (choice.driver.empty()) {
                        showDriverDialog(printer, choice.drivers);
                        return;
                    }
                    PrinterRow* row = m_foundRows.contains(printer.name) ? m_foundRows[printer.name] : nullptr;
                    change(row, [printer, driver = choice.driver] { return printers::add(printer, driver); });
                });
    }

    void showDriverDialog(const Printer& printer, const std::vector<printers::Driver>& drivers)
    {
        m_dialog = std::make_unique<FormDialog>(m_window, tr("Choose a Driver"), 440);
        ColumnLayout* content = m_dialog->content();
        Label* note = content->addChild<Label>(format(
                tr("No driver is known for {printer}. Choose one: that of a model of the same series usually "
                   "works, such as Epson LQ-850 for an Epson LQ dot-matrix printer. Raw is for applications "
                   "that send the printer's own commands, such as point of sale ones."),
                {{"printer", printer.info}}));
        note->setWrap(true);
        note->setLayoutHints({.fillWidth = true});
        std::vector<std::string> names;
        for (const printers::Driver& driver : drivers)
            names.push_back(driver.description.empty() ? driver.name : driver.description);
        ComboBox* box = content->addChild<ComboBox>(names);
        box->setEditable(true);
        box->setCurrentIndex(0);
        box->setLayoutHints({.fillWidth = true});
        RowLayout* buttons = m_dialog->addButtonRow();
        DialogButton* cancel = buttons->addChild<DialogButton>(tr("Cancel"));
        cancel->onClicked = [this] { m_dialog->close(); };
        DialogButton* add = buttons->addChild<DialogButton>(tr("Add"), true);
        // Typed in, the driver is the one named so.
        const auto chosen = [box, names] {
            const auto it = std::ranges::find(names, box->editText());
            return it == names.end() ? -1 : int(it - names.begin());
        };
        const auto accept = [this, chosen, printer, drivers] {
            const int index = chosen();
            if (index < 0)
                return;
            PrinterRow* row = m_foundRows.contains(printer.name) ? m_foundRows[printer.name] : nullptr;
            change(row, [printer, driver = drivers[size_t(index)].name] { return printers::add(printer, driver); });
            m_dialog->close();
        };
        box->onEditTextChanged = [add, chosen] { add->setEnabled(chosen() >= 0); };
        box->onAccepted = accept;
        add->onClicked = accept;
        m_dialog->show();
    }

    void showAddressDialog()
    {
        m_dialog = std::make_unique<FormDialog>(m_window, tr("Add a Printer by Address"), 400);
        ColumnLayout* content = m_dialog->content();
        Label* note = content->addChild<Label>(
                tr("The printer's host name or IP address, such as 192.168.1.20, or its IPP address. It has to "
                   "print without a driver (IPP Everywhere or AirPrint), as most printers made since 2012 do."));
        note->setWrap(true);
        note->setLayoutHints({.fillWidth = true});
        TextField* address = content->addChild<TextField>();
        address->setPlaceholder(tr("Address"));
        address->setLayoutHints({.fillWidth = true});
        RowLayout* buttons = m_dialog->addButtonRow();
        DialogButton* cancel = buttons->addChild<DialogButton>(tr("Cancel"));
        cancel->onClicked = [this] { m_dialog->close(); };
        DialogButton* add = buttons->addChild<DialogButton>(tr("Add"), true);
        add->setEnabled(false);
        const auto accept = [this, address] {
            const std::string uri = printers::uriForAddress(address->text());
            if (uri.empty())
                return;
            std::string host = uri.substr(uri.find("://") + 3);
            host = host.substr(0, host.find('/'));
            change(nullptr, [printer = Printer {.name = host, .info = host, .uri = uri}] {
                return printers::add(printer);
            });
            m_dialog->close();
        };
        address->onTextChanged = [address, add] { add->setEnabled(!printers::uriForAddress(address->text()).empty()); };
        address->onAccepted = accept;
        add->onClicked = accept;
        m_dialog->show();
    }

    SettingsWindow& m_window;
    Card* m_queues = nullptr;
    Card* m_found = nullptr;
    ListRow* m_none = nullptr;
    ListRow* m_searching = nullptr;
    Footnote* m_message = nullptr;
    std::map<std::string, PrinterRow*> m_queueRows;
    std::map<std::string, PrinterRow*> m_foundRows;
    // The printers a command is running for.
    std::set<std::string> m_busy;
    std::unique_ptr<FormDialog> m_dialog;
    bayu::base::Timer m_timer;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};

} // namespace

static const PageRegistration registration(
        "printers",
        [](SettingsWindow& window) -> std::unique_ptr<Page> { return std::make_unique<PrintersPage>(window); });

} // namespace settings
