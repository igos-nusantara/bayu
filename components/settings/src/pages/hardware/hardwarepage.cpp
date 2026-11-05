// SPDX-License-Identifier: BSD-3-Clause

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "base/event_loop.h"
#include "base/file.h"
#include "base/i18n.h"
#include "base/strings.h"
#include "pages/about/aboutinfo.h"
#include "pages/hardware/hardware.h"
#include "ui/button.h"
#include "ui/cell_layout.h"
#include "ui/clipboard.h"
#include "ui/icon.h"
#include "ui/label.h"
#include "ui/layout.h"
#include "ui/progress_bar.h"
#include "ui/theme.h"
#include "widgets/card.h"
#include "widgets/listrow.h"
#include "widgets/metrics.h"
#include "widgets/sections.h"
#include "widgets/standardbutton.h"
#include "window/admin.h"
#include "window/page.h"
#include "window/pagelist.h"
#include "window/settingswindow.h"

using namespace bayu;
using namespace bayu::ui;
using bayu::i18n::format;
using bayu::i18n::tr;
using settings::hardware::Device;
using settings::hardware::Package;

namespace settings {

namespace {

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

// A row of two lines: the name, then details in secondary text.
class DetailRow : public ListRow
{
public:
    DetailRow(const std::string& icon, const std::string& name, const std::string& details)
    {
        setLeading<ThemeIcon>(icon, 24);
        ColumnLayout* texts = addContent<ColumnLayout>();
        texts->setSpacing(metrics::textSpacing);
        texts->setLayoutHints({.fillWidth = true, .verticalAlignment = Alignment::Center});
        Label* title = texts->addChild<Label>(name);
        title->setElide(Label::Elide::Right);
        title->setLayoutHints({.fillWidth = true});
        Label* secondary = texts->addChild<SecondaryLabel>(details);
        secondary->setElide(Label::Elide::Right);
        secondary->setLayoutHints({.fillWidth = true});
        setMinimumRowHeight(metrics::twoLineRowHeight);
        setInteractive(false);
    }
};

std::string iconFor(const Device& device)
{
    if (device.bus == Device::Bus::Usb)
        return "drive-removable-media";
    if (device.classId.starts_with("01"))
        return "drive-harddisk";
    if (device.classId.starts_with("02") || device.classId.starts_with("0d"))
        return device.classId == "0200" ? "network-wired" : "network-wireless";
    if (device.classId.starts_with("03"))
        return "video-display";
    if (device.classId.starts_with("04"))
        return "audio-card";
    return "computer";
}

bool installed(const std::vector<std::string>& packages)
{
    std::string command = "rpm -q --quiet";
    for (const std::string& package : packages)
        command += " '" + package + "'";
    return std::system((command + " 2>/dev/null").c_str()) == 0;
}

// The firmware's Secure Boot, which loads no module the kernel's key has not
// signed, as those akmods builds.
bool secureBoot()
{
    const auto value = base::file::read(
            "/sys/firmware/efi/efivars/SecureBoot-8be4df61-93ca-11d2-aa0d-00e098032b8c");
    return value && value->size() >= 5 && (*value)[4] == 1;
}

struct Scan
{
    std::vector<Device> devices;
    std::vector<Package> missing;
};

class HardwarePage : public Page
{
public:
    explicit HardwarePage(SettingsWindow& window)
        : Page(window, tr("Drivers & Hardware"))
    {
        m_recommended = content()->addChild<Section>(tr("Recommended"));
        m_recommended->setName("hardware:recommended");
        m_packages = m_recommended->addChild<Card>(Card::Style::List);
        m_packagesNote = m_recommended->addChild<Footnote>();
        m_recommended->setVisible(false);

        Section* devices = content()->addChild<Section>(tr("Devices"));
        m_devices = devices->addChild<Card>(Card::Style::List);
        m_devices->setName("hardware:devices");
        m_busy = devices->addChild<BusyIndicator>();
        m_busy->setLayoutHints({.preferredWidth = 18, .preferredHeight = 18});
        StandardButton* copy = content()->addChild<StandardButton>(tr("Copy Hardware Information"));
        copy->setName("hardware:copy");
        copy->onClicked = [this] { Clipboard::setText(m_report); };
        content()->addChild<Footnote>(tr("Paste it into a bug report, so that others can see this hardware."));

        scan();
    }

    ~HardwarePage() override { *m_alive = false; }

private:
    void scan()
    {
        m_busy->setVisible(true);
        std::thread([loop = base::EventLoop::current()->handle(), alive = std::weak_ptr<bool>(m_alive), this] {
            auto result = std::make_shared<Scan>();
            result->devices = hardware::pciDevices();
            const std::vector<Device> usb = hardware::usbDevices();
            result->devices.insert(result->devices.end(), usb.begin(), usb.end());
            const auto database = hardware::DriverDatabase::load();
            for (const Package& package : hardware::packagesFor(result->devices, database, hardware::currentSystem())) {
                if (!installed(package.names))
                    result->missing.push_back(package);
            }
            loop.post([alive, this, result] {
                if (const auto page = alive.lock(); page && *page)
                    show(*result);
            });
        }).detach();
    }

    void show(const Scan& scan)
    {
        m_busy->setVisible(false);
        while (!m_devices->children().empty())
            m_devices->takeChild(m_devices->children().front().get());
        for (const Device& device : scan.devices) {
            std::string details = device.kind;
            details += " · ";
            details += device.driver.empty() ? tr("No driver") : format(tr("Driver: {driver}"),
                                                                         {{"driver", device.driver}});
            const std::string name = device.vendor.empty() ? device.name : device.vendor + " " + device.name;
            m_devices->addChild<DetailRow>(iconFor(device), name, details);
        }

        while (!m_packages->children().empty())
            m_packages->takeChild(m_packages->children().front().get());
        bool proprietary = false;
        for (const Package& package : scan.missing) {
            std::string details = base::strings::join(package.names, " ");
            if (package.proprietary)
                details += std::string(" · ") + tr("Proprietary");
            proprietary = proprietary || package.proprietary;
            auto* row = m_packages->addChild<DetailRow>("system-software-install", package.reason, details);
            row->setName("hardware:package:" + package.names.front());
            Button* install = row->addContent<Button>(tr("Install"));
            install->onClicked = [this, install, package] {
                install->setEnabled(false);
                m_busy->setVisible(true);
                m_note.clear();
                std::vector<std::string> arguments{"install"};
                arguments.insert(arguments.end(), package.names.begin(), package.names.end());
                admin::run(arguments, m_alive, [this, package](const std::optional<std::string>& error) {
                    if (error && !error->empty())
                        m_note = *error;
                    else if (!error && package.proprietary)
                        m_note = tr("The driver is installed. Restart the computer to use it.");
                    this->scan();
                });
            };
        }
        std::string note = tr("Drivers and firmware for the devices of this computer that are not installed "
                              "yet. Without them, the devices may not work, or work only in part.");
        if (proprietary && secureBoot())
            note += std::string(" ") + tr("Secure Boot is on: it keeps proprietary drivers built on this computer from "
                             "loading. Turn it off in the computer's firmware settings to use them.");
        if (!m_note.empty())
            note = m_note;
        m_packagesNote->setText(note);
        m_recommended->setVisible(!scan.missing.empty() || !m_note.empty());

        m_report = about::prettyProductName() + "\n";
        if (const std::string model = about::computerModel(); !model.empty())
            m_report += model + "\n";
        m_report += std::string(base::strings::trimmed(about::processor())) + ", " + about::memorySize() + "\n";
        m_report += "Linux " + about::kernelVersion() + "\n\n" + hardware::report(scan.devices);
    }

    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
    Section* m_recommended;
    Card* m_packages;
    Footnote* m_packagesNote;
    Card* m_devices;
    BusyIndicator* m_busy;
    std::string m_report;
    // What the last install said, until the next one.
    std::string m_note;
};

} // namespace

static const PageRegistration registration("hardware", [](SettingsWindow& window) -> std::unique_ptr<Page> {
    return std::make_unique<HardwarePage>(window);
});

} // namespace settings
