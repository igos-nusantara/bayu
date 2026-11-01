// SPDX-License-Identifier: GPL-3.0-or-later
// Step 7: everything chosen, in one place, and the last check of the disk
// layout before anything is written.

#include <algorithm>

#include "base/i18n.h"
#include "ui/dialog_window.h"
#include "ui/label.h"
#include "ui/scroll_view.h"
#include "ui/page.h"
#include "ui/pages.h"
#include "ui/installer.h"
#include "ui/widgets.h"

using namespace bayu::ui;
using bayu::i18n::format;
using bayu::i18n::tr;

namespace installer {

using style::Role;

namespace {

std::string keyboardText(const std::string& layout)
{
    if (layout.empty())
        return "us";
    return layout;
}

class ReviewPage : public Page
{
public:
    explicit ReviewPage(Installer& installer)
        : Page(installer)
    {
        Row* body = addChild<Row>();
        body->setSpacing(style::columnSpacing);
        body->setLayoutHints({.fillWidth = true, .fillHeight = true});

        ScrollView* leftScroll = body->addChild<ScrollView>();
        leftScroll->setVerticalBarPolicy(ScrollView::BarPolicy::AlwaysOn);
        leftScroll->setLayoutHints({.fillWidth = true, .fillHeight = true, .stretch = 1.05, .preferredWidth = 0});
        Column* left = leftScroll->setContent<Column>();
        left->setSpacing(10);
        addSectionLabel(left, tr("SUMMARY"));
        m_localization = left->addChild<DetailsCard>();
        m_localization->setHeading("languages", tr("Localization"), [this] { this->installer().edit(LanguageStep); });
        m_network = left->addChild<DetailsCard>();
        m_network->setHeading("wifi", tr("Network"), [this] { this->installer().edit(NetworkStep); });
        m_account = left->addChild<DetailsCard>();
        m_account->setHeading("user", tr("Account"), [this] { this->installer().edit(AccountStep); });

        ScrollView* rightScroll = body->addChild<ScrollView>();
        rightScroll->setVerticalBarPolicy(ScrollView::BarPolicy::AlwaysOn);
        rightScroll->setLayoutHints({.fillWidth = true, .fillHeight = true, .preferredWidth = 0});
        Column* right = rightScroll->setContent<Column>();
        right->setSpacing(10);
        addSectionLabel(right, tr("TARGET"));
        m_storage = right->addChild<DetailsCard>();
        m_storage->setHeading("hard-drive", tr("Storage"), [this] { this->installer().edit(DestinationStep); });
        m_software = right->addChild<DetailsCard>();
        m_software->setHeading("package", tr("Software"), [this] { this->installer().edit(SoftwareStep); });
        m_warning = right->addChild<Banner>(Banner::Kind::Warning, std::string(), std::string());
    }

    std::string stepName() const override { return tr("Review"); }
    std::string title() const override
    {
        if (m_checking)
            return tr("Checking the plan…");
        return m_report.ok() ? tr("Everything checks out.") : tr("Something needs a look.");
    }
    std::string description() const override
    {
        if (!m_report.ok())
            return tr("The disk layout cannot be installed as it is. Edit the storage settings and come back here.");
        bool deletes = false;
        if (installer().choices().storage.method == StoragePlan::Method::Custom) {
            for (const PendingAction& action : installer().backend().pendingActions())
                deletes = deletes || action.destructive;
        }
        if (deletes)
            return tr("Nothing has been written yet. Take one last look — after you begin, the partitions you "
                      "deleted are erased.");
        if (installer().choices().erasedDisks.empty())
            return tr("Nothing has been written yet. Take one last look — what else is on the disk is kept.");
        return tr("Nothing has been written yet. Take one last look — after you begin, the selected disk is "
                  "reformatted.");
    }
    std::string summary() const override { return bayu::i18n::trn("{n} item", "{n} items", 8); }
    std::string continueText() const override { return tr("Begin installation"); }
    std::string continueGlyph() const override { return "drive-download"; }
    bool isReview() const override { return true; }
    bool canContinue() const override { return !m_checking && m_report.ok(); }

    void enter() override
    {
        fill();
        // The layout is applied for real now, with the passphrase from the
        // account step, and checked once more.
        m_checking = true;
        m_report = {};
        changed();
        const Choices& choices = installer().choices();
        installer().backend().applyStorage(choices.storage, choices.account.password,
                                           [this](const ValidationReport& report) {
                                               m_checking = false;
                                               m_report = report;
                                               fill();
                                               changed();
                                               if (report.espFull)
                                                   offerNewEsp();
                                           });
    }

private:
    // The EFI partition kept from another system is full: a new one, in the
    // free space, only if the user agrees.
    void offerNewEsp()
    {
        StoragePlan& plan = installer().choices().storage;
        if (plan.method != StoragePlan::Method::Automatic || plan.newEsp)
            return;
        m_confirm = std::make_unique<ConfirmDialog>(
                installer().window(), tr("Create a new EFI partition?"),
                tr("The EFI partition on this disk is too full for IGOS Nusantara's boot loader. The installer can "
                   "create an EFI partition of its own, 600 MB, in the space for IGOS Nusantara. The existing one "
                   "and the system that starts from it are left as they are. Nothing has been written yet."));
        m_confirm->setConfirmText(tr("Create EFI partition"));
        m_confirm->onAccepted = [this] {
            installer().choices().storage.newEsp = true;
            enter();
        };
        m_confirm->show();
    }

    void fill()
    {
        Backend& backend = installer().backend();
        const Choices& choices = installer().choices();

        m_localization->clearLines();
        std::string language = choices.locale;
        for (const Language& entry : backend.languages()) {
            if (choices.locale.starts_with(entry.id + "_")) {
                const size_t underscore = choices.locale.find('_');
                language = entry.englishName + " (" + choices.locale.substr(underscore + 1, 2) + ")";
            }
        }
        m_localization->addLine(tr("Language"), language);
        m_localization->addLine(tr("Keyboard"), keyboardText(choices.keyboardLayout));
        m_localization->addLine(tr("Time zone"), choices.time.timezone);

        m_network->clearLines();
        m_network->addLine(tr("Host name"), choices.hostname);
        std::string connection = tr("not connected");
        std::string address;
        for (const NetworkConnection& entry : backend.connections()) {
            if (entry.connected) {
                connection = entry.name;
                address = backend.details(entry.id).ipv4;
            }
        }
        m_network->addLine(tr("Connection"), connection);
        if (!address.empty())
            m_network->addLine(tr("IPv4"), address);

        m_account->clearLines();
        m_account->addLine(tr("User"), choices.account.userName);
        m_account->addLine(tr("Administrator"), choices.account.administrator ? tr("yes · wheel") : tr("no"));
        m_account->addLine(tr("Root account"), choices.account.lockRoot ? tr("locked") : tr("password set"),
                           choices.account.lockRoot ? std::optional<Role>(Role::Success) : std::nullopt);

        m_storage->clearLines();
        std::string device;
        uint64_t size = 0;
        for (const Disk& disk : backend.disks()) {
            if (std::find(choices.storage.disks.begin(), choices.storage.disks.end(), disk.id)
                != choices.storage.disks.end()) {
                device = disk.id;
                size = disk.size;
            }
        }
        m_storage->addLine(tr("Device"), device + " · " + style::formatSize(size));
        std::string scheme = choices.storage.method == StoragePlan::Method::Automatic ? tr("Automatic") : tr("Custom");
        // The file system of / as planned: the backend picks it.
        for (const LayoutPart& part : choices.layout) {
            if (part.mountPoint == "/" && !part.format.empty())
                scheme += " · " + part.format;
        }
        m_storage->addLine(tr("Scheme"), scheme);
        // Custom layouts encrypt mount points one by one.
        bool encrypted = choices.storage.encrypt && choices.storage.method == StoragePlan::Method::Automatic;
        int deleted = 0;
        if (choices.storage.method == StoragePlan::Method::Custom) {
            for (const Disk& disk : backend.customDisks()) {
                for (const DiskSegment& segment : disk.segments)
                    encrypted = encrypted || (segment.encrypted && !segment.existing);
            }
            for (const PendingAction& action : backend.pendingActions())
                deleted += action.destructive ? 1 : 0;
        }
        m_storage->addLine(tr("Encryption"), encrypted ? tr("LUKS2 enabled") : tr("off"),
                           encrypted ? std::optional<Role>(Role::Success) : std::nullopt);
        std::vector<LayoutPart> layout = backend.appliedLayout();
        if (layout.empty())
            layout = choices.layout;
        std::string mounts;
        for (const LayoutPart& part : layout)
            mounts += (mounts.empty() ? "" : " · ") + part.mountPoint;
        m_storage->addLine(tr("Layout"), mounts.empty() ? "—" : mounts);

        m_software->clearLines();
        std::string environment = choices.environment;
        for (const SoftwareEnvironment& entry : backend.environments()) {
            if (entry.id == choices.environment)
                environment = entry.name;
        }
        m_software->addLine(tr("Environment"), environment);
        m_software->addLine(tr("Add-ons"), bayu::i18n::trn("{n} selected", "{n} selected", choices.addons.size()));
        const SoftwareSummary software = backend.softwareSummary();
        if (software.packages)
            m_software->addLine(tr("Packages"), style::formatCount(software.packages));
        m_software->addLine(tr("Download"), software.downloadSize ? style::formatSize(software.downloadSize)
                                                                  : tr("nothing"));

        if (!m_report.ok()) {
            std::string text;
            for (const std::string& line : m_report.errors)
                text += (text.empty() ? "" : "\n") + line;
            m_warning->setKind(Banner::Kind::Error);
            m_warning->setText(tr("The disk layout cannot be installed"), text);
        } else if (!choices.erasedDisks.empty()) {
            m_warning->setKind(Banner::Kind::Warning);
            m_warning->setText(format(tr("{disk} will be erased"), {{"disk", choices.erasedDisks.front()}}),
                               format(tr("All {size} on this device is reformatted. Other disks are left untouched."),
                                      {{"size", style::formatSize(size)}}));
        } else if (deleted) {
            m_warning->setKind(Banner::Kind::Warning);
            m_warning->setText(tr("Partitions will be deleted"),
                               tr("What the custom layout deletes is lost once the installation starts. Edit the "
                                  "storage to check the pending changes."));
        } else if (!m_report.warnings.empty()) {
            std::string text;
            for (const std::string& line : m_report.warnings)
                text += (text.empty() ? "" : "\n") + line;
            m_warning->setKind(Banner::Kind::Warning);
            m_warning->setText(tr("Check before installing"), text);
        } else {
            m_warning->setKind(Banner::Kind::Info);
            m_warning->setText(tr("Your existing data is kept"),
                               tr("Only the space chosen on the destination step is changed."));
        }
    }

    std::unique_ptr<ConfirmDialog> m_confirm;
    DetailsCard* m_localization;
    DetailsCard* m_network;
    DetailsCard* m_account;
    DetailsCard* m_storage;
    DetailsCard* m_software;
    Banner* m_warning;
    ValidationReport m_report;
    bool m_checking = false;
};

} // namespace

std::unique_ptr<Page> createReviewPage(Installer& installer)
{
    return std::make_unique<ReviewPage>(installer);
}

} // namespace installer
