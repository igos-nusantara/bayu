// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "dialogs/propertiesdialog.h"
#include "core/permissions.h"
#include "core/standardfolders.h"

#include "base/i18n.h"
#include "base/locale.h"
#include "ui/button.h"
#include "ui/check_box.h"
#include "ui/combo_box.h"
#include "ui/icon.h"
#include "ui/label.h"
#include "ui/cell_layout.h"
#include "ui/theme.h"

#include <algorithm>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

using bayu::i18n::format;
using bayu::i18n::tr;
using bayu::i18n::trn;
using bayu::i18n::trNoop;

namespace fm {

static constexpr double kSmallSpacing = 6;
static constexpr double kLargeSpacing = 12;

// "Fri Oct 16 09:30:00 2026", local time.
static std::string textDate(int64_t milliseconds)
{
    if (milliseconds <= 0)
        return {};
    const base::DateTime time = base::DateTime::fromSecondsSinceEpoch(milliseconds / 1000);
    return base::Locale("C").toString(time, "ddd MMM d HH:mm:ss yyyy");
}

// What a copy of everything would take: files' sizes, folders walked.
static uint64_t totalSize(const std::string& path, const std::atomic_bool& cancelled)
{
    struct stat st;
    if (::lstat(path.c_str(), &st) != 0)
        return 0;
    if (!S_ISDIR(st.st_mode))
        return uint64_t(st.st_size);
    uint64_t total = 0;
    if (DIR* directory = ::opendir(path.c_str())) {
        while (dirent* entry = ::readdir(directory)) {
            if (cancelled.load())
                break;
            const std::string name = entry->d_name;
            if (name == "." || name == "..")
                continue;
            total += totalSize(path + '/' + name, cancelled);
        }
        ::closedir(directory);
    }
    return total;
}

PropertiesDialog::PropertiesDialog(FileItemList items)
    : FramelessDialog(tr("Properties"), true)
    , m_items(std::move(items))
{
    const bool multiple = m_items.size() > 1;
    const FileItem& first = *m_items.front();
    if (!multiple) {
        m_fileName = first.name;
        m_iconName = first.isDir ? "folder" : first.iconName;
        m_mimeType = first.mimeType;
        m_bytes = first.size;
        m_size = formatSize(first.size);
        m_location = parentPath(realPath(first.path));
        m_created = textDate(first.created);
        m_modified = textDate(first.modified);
        m_accessed = textDate(first.accessed);
        m_path = first.path;
        struct stat st;
        if (first.isLocalFile() && ::lstat(first.path.c_str(), &st) == 0 && !S_ISLNK(st.st_mode))
            m_stat = st;
        m_administrator = isAdministratorPath(first.path);
        m_canChange = m_stat && (m_administrator || ::getuid() == 0 || m_stat->st_uid == ::getuid());
    } else {
        m_fileName = trn("{n} file", "{n} files", m_items.size());
        m_location = parentPath(realPath(first.path));
        m_iconName = "unknown";
    }

    std::vector<std::string> paths;
    for (const FileItemPtr& item : m_items)
        paths.push_back(item->path);
    m_sizeJob = base::ThreadPool::global().run(
            [paths, cancelled = m_cancelled] {
                uint64_t total = 0;
                for (const std::string& path : paths)
                    total += totalSize(path, *cancelled);
                return total;
            },
            [this](uint64_t total) {
                m_bytes = total;
                m_size = formatSize(total);
                if (m_sizeLabel) {
                    m_sizeLabel->setText(m_size);
                    fit();
                }
            });

    setHeader(40, 4);
    setPadding(kLargeSpacing * 1.5, kSmallSpacing, kLargeSpacing * 2 - kSmallSpacing);
    setContentFactory([this] { return createContent(); }, 0, 0);
    onCloseRequested = [this] {
        hide();
        if (onDone)
            onDone();
    };
}

PropertiesDialog::~PropertiesDialog()
{
    m_cancelled->store(true);
}

void PropertiesDialog::reread()
{
    struct stat st;
    if (::lstat(m_path.c_str(), &st) == 0)
        m_stat = st;
    rebuildContent();
    fit();
}

void PropertiesDialog::changeMode(mode_t mode)
{
    m_permissionNote.clear();
    if (::chmod(m_path.c_str(), mode) != 0)
        m_permissionNote = format(tr("The permissions could not be changed: {error}"), {{"error", std::strerror(errno)}});
    reread();
}

void PropertiesDialog::changeOwner(uid_t uid, gid_t gid)
{
    m_permissionNote.clear();
    if (::lchown(m_path.c_str(), uid, gid) != 0)
        m_permissionNote = format(tr("The owner could not be changed: {error}"), {{"error", std::strerror(errno)}});
    reread();
}

void PropertiesDialog::applyToContents()
{
    using permissions::Who;
    const std::array<permissions::Access, 3> access = {permissions::access(m_stat->st_mode, Who::Owner),
                                                       permissions::access(m_stat->st_mode, Who::Group),
                                                       permissions::access(m_stat->st_mode, Who::Others)};
    m_permissionNote = tr("Changing the folder's contents…");
    rebuildContent();
    m_contentsJob = base::ThreadPool::global().run([path = m_path, access] { return permissions::applyToContents(path, access); },
                                                   [this](int error) {
                                                       m_permissionNote = error ? format(tr("Some items could not be changed: {error}"),
                                                                                         {{"error", std::strerror(error)}})
                                                                                : std::string(tr("The folder's contents were changed."));
                                                       reread();
                                                   });
}

void PropertiesDialog::addPermissions(ui::GridLayout* form, double labelWidth)
{
    using permissions::Access;
    using permissions::Who;
    const struct stat st = *m_stat;
    const bool folder = S_ISDIR(st.st_mode);
    const auto label = [&](const char* text) {
        auto* view = form->addChild<ui::Label>(tr(text));
        view->setColor(ui::ColorId::DisabledText);
        view->setTextAlignment(ui::Label::TextAlignment::Right);
        view->setLayoutHints({.preferredWidth = labelWidth, .horizontalAlignment = ui::Alignment::End});
    };

    // An administrator window gives the item to anyone; elsewhere the names show.
    label(trNoop("Owner:"));
    if (m_administrator) {
        const auto users = permissions::users();
        std::vector<std::string> names;
        int current = -1;
        for (size_t i = 0; i < users.size(); ++i) {
            names.push_back(users[i].second);
            if (users[i].first == st.st_uid)
                current = int(i);
        }
        auto* owner = form->addChild<ui::ComboBox>(names);
        owner->setCurrentIndex(current);
        owner->onActivated = [this, users, gid = st.st_gid](int index) {
            if (index >= 0 && size_t(index) < users.size())
                changeOwner(users[size_t(index)].first, gid);
        };
    } else {
        form->addChild<ui::Label>(permissions::userName(st.st_uid));
    }
    label(trNoop("Group:"));
    if (m_administrator) {
        const auto groups = permissions::groups();
        std::vector<std::string> names;
        int current = -1;
        for (size_t i = 0; i < groups.size(); ++i) {
            names.push_back(groups[i].second);
            if (groups[i].first == st.st_gid)
                current = int(i);
        }
        auto* group = form->addChild<ui::ComboBox>(names);
        group->setCurrentIndex(current);
        group->onActivated = [this, groups, uid = st.st_uid](int index) {
            if (index >= 0 && size_t(index) < groups.size())
                changeOwner(uid, groups[size_t(index)].first);
        };
    } else {
        form->addChild<ui::Label>(permissions::groupName(st.st_gid));
    }

    const std::vector<std::string> choices = {tr("None"), tr("Read only"), tr("Read and write")};
    const std::pair<const char*, Who> rows[] = {{trNoop("Owner access:"), Who::Owner},
                                                {trNoop("Group access:"), Who::Group},
                                                {trNoop("Others access:"), Who::Others}};
    for (const auto& [text, who] : rows) {
        label(text);
        auto* access = form->addChild<ui::ComboBox>(choices);
        access->setCurrentIndex(int(permissions::access(st.st_mode, who)));
        access->setEnabled(m_canChange);
        access->onActivated = [this, who, folder](int index) {
            changeMode(permissions::withAccess(m_stat->st_mode & 07777, who, Access(index), folder));
        };
    }

    if (!folder) {
        form->addChild<ui::View>();
        auto* executable = form->addChild<ui::CheckBox>(tr("Allow executing as a program"));
        executable->setChecked(permissions::isExecutable(st.st_mode));
        executable->setEnabled(m_canChange);
        executable->onToggled = [this](bool checked) {
            changeMode(permissions::withExecutable(m_stat->st_mode & 07777, checked));
        };
    } else {
        form->addChild<ui::View>();
        auto* contents = form->addChild<ui::Button>(tr("Apply to the Folder's Contents"));
        contents->setEnabled(m_canChange);
        contents->onClicked = [this] { applyToContents(); };
    }

    std::string note = m_permissionNote;
    if (note.empty() && !m_canChange)
        note = tr("Only the owner can change these. Open the folder as an administrator to change them.");
    if (!note.empty()) {
        form->addChild<ui::View>();
        auto* hint = form->addChild<ui::Label>(note);
        hint->setColor(ui::ColorId::DisabledText);
        hint->setWrap(true);
        hint->setLayoutHints({.fillWidth = true});
    }

}

void PropertiesDialog::retranslate()
{
    setTitle(tr("Properties"));
    if (m_items.size() > 1)
        m_fileName = trn("{n} file", "{n} files", m_items.size());
    if (m_bytes)
        m_size = formatSize(*m_bytes);
    rebuildContent();
}

std::unique_ptr<ui::View> PropertiesDialog::createContent()
{
    auto layout = std::make_unique<ui::ColumnLayout>();
    layout->setSpacing(kLargeSpacing);

    struct Row
    {
        std::string label;
        std::string value;
        bool wrap = false;
        bool size = false;
    };
    std::vector<Row> rows = {
        {trNoop("Type:"), m_mimeType},
        {trNoop("Location:"), m_location, true},
        {trNoop("Size:"), m_size.empty() ? std::string(tr("Calculating...")) : m_size, false, true},
        {trNoop("Created:"), m_created},
        {trNoop("Modified:"), m_modified},
        {trNoop("Accessed:"), m_accessed},
    };
    // The Permissions rows' labels share the column.
    const std::vector<std::string> permissionLabels = m_stat
            ? std::vector<std::string>{"Owner:", "Group:", "Owner access:", "Group access:", "Others access:"}
            : std::vector<std::string>{};

    // Hidden rows leave the grid, so the label column is measured over all.
    double labelWidth = 48;
    for (const Row& row : rows) {
        ui::Label probe(tr(row.label.c_str()));
        if (window())
            probe.setTextLayoutMode(window()->textLayoutMode());
        labelWidth = std::max(labelWidth, probe.sizeHint().width);
    }
    for (const std::string& text : permissionLabels) {
        ui::Label probe(tr(text.c_str()));
        if (window())
            probe.setTextLayoutMode(window()->textLayoutMode());
        labelWidth = std::max(labelWidth, probe.sizeHint().width);
    }

    // The icon ends where the labels do and the name starts where the values do.
    auto* header = layout->addChild<ui::RowLayout>();
    header->setSpacing(kLargeSpacing);
    auto* iconBox = header->addChild<ui::View>();
    iconBox->setLayoutHints({.preferredWidth = labelWidth, .preferredHeight = 48});
    auto* icon = iconBox->addChild<ui::ThemeIcon>(m_iconName, 48);
    icon->setGeometry({std::ceil(labelWidth) - 48, 0, 48, 48});
    auto* name = header->addChild<ui::Label>(m_fileName);
    name->setElide(ui::Label::Elide::Middle);
    name->setLayoutHints({.fillWidth = true});

    auto* form = layout->addChild<ui::GridLayout>(2);
    form->setColumnSpacing(kLargeSpacing);
    form->setRowSpacing(kLargeSpacing);
    form->setLayoutHints({.fillWidth = true, .verticalAlignment = ui::Alignment::Start});
    for (const Row& row : rows) {
        // A row without a value is left out; location and size always show.
        const bool shown = !row.value.empty() || row.label == "Location:" || row.size;
        if (!shown)
            continue;
        auto* label = form->addChild<ui::Label>(tr(row.label.c_str()));
        label->setColor(ui::ColorId::DisabledText);
        label->setTextAlignment(ui::Label::TextAlignment::Right);
        label->setLayoutHints({.preferredWidth = labelWidth, .horizontalAlignment = ui::Alignment::End});
        auto* value = form->addChild<ui::Label>(row.value);
        if (row.wrap) {
            value->setWrap(true);
            value->setLayoutHints({.fillWidth = true});
        }
        if (row.size)
            m_sizeLabel = value;
    }
    if (m_stat)
        addPermissions(form, labelWidth);
    return layout;
}

} // namespace fm
