// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "core/fileitem.h"
#include "dialogs/framelessdialog.h"

#include "base/thread_pool.h"

#include <sys/stat.h>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace bayu::ui {
class GridLayout;
class Label;
}

namespace fm {

// The item's icon and
// name, then type, location, size (counted on a worker for folders and
// several items), and its times. For one item, its owner, group and who may
// read and write it, changed where it is shown (#146).
class PropertiesDialog : public FramelessDialog
{
public:
    explicit PropertiesDialog(FileItemList items);
    ~PropertiesDialog() override;

    void retranslate() override;

    std::function<void()> onDone;

private:
    std::unique_ptr<ui::View> createContent();
    void addPermissions(ui::GridLayout* form, double labelWidth);
    // Through chmod and chown; the item is read again after, and a failure shown.
    void changeMode(mode_t mode);
    void changeOwner(uid_t uid, gid_t gid);
    void applyToContents();
    void reread();

    FileItemList m_items;
    std::string m_fileName;
    std::string m_iconName;
    std::string m_mimeType;
    std::string m_location;
    std::string m_size;
    // What m_size reads, once known.
    std::optional<uint64_t> m_bytes;
    std::string m_created;
    std::string m_modified;
    std::string m_accessed;
    ui::Label* m_sizeLabel = nullptr;
    // One item, not a link: what its Permissions rows show.
    std::string m_path;
    std::optional<struct stat> m_stat;
    // The owner, root, or an administrator window, whose mount is root's.
    bool m_canChange = false;
    bool m_administrator = false;
    std::string m_permissionNote;
    base::Job m_contentsJob;
    base::Job m_sizeJob;
    std::shared_ptr<std::atomic_bool> m_cancelled = std::make_shared<std::atomic_bool>(false);
};

} // namespace fm
