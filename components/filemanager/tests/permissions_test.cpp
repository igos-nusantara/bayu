// SPDX-License-Identifier: BSD-3-Clause

#include "testutil.h"

#include "core/permissions.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <sys/stat.h>
#include <unistd.h>

using namespace fm::permissions;
using fmtest::TemporaryDir;
using fmtest::writeFile;

static mode_t modeOf(const std::string& path)
{
    struct stat st;
    return ::lstat(path.c_str(), &st) == 0 ? st.st_mode & 07777 : 0;
}

// Each choice in its place, a folder's reading with entering it (#146).
TEST(PermissionsTest, AccessSetsReadAndWriteForEachWho)
{
    EXPECT_EQ(access(0640, Who::Owner), Access::ReadWrite);
    EXPECT_EQ(access(0640, Who::Group), Access::Read);
    EXPECT_EQ(access(0640, Who::Others), Access::None);

    EXPECT_EQ(withAccess(0644, Who::Others, Access::None, false), mode_t(0640));
    EXPECT_EQ(withAccess(0600, Who::Group, Access::ReadWrite, false), mode_t(0660));
    EXPECT_EQ(withAccess(0755, Who::Owner, Access::Read, false), mode_t(0555));
    // A file's x stays; a folder's comes and goes with reading.
    EXPECT_EQ(withAccess(0754, Who::Others, Access::Read, false), mode_t(0754));
    EXPECT_EQ(withAccess(0700, Who::Group, Access::Read, true), mode_t(0750));
    EXPECT_EQ(withAccess(0755, Who::Others, Access::None, true), mode_t(0750));
}

TEST(PermissionsTest, ExecutableFollowsWhoMayRead)
{
    EXPECT_FALSE(isExecutable(0644));
    EXPECT_TRUE(isExecutable(0744));
    EXPECT_EQ(withExecutable(0644, true), mode_t(0755));
    EXPECT_EQ(withExecutable(0640, true), mode_t(0750));
    EXPECT_EQ(withExecutable(0755, false), mode_t(0644));
}

TEST(PermissionsTest, AppliesToAFoldersContentsAsFoldersAndFiles)
{
    TemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const std::string root = directory.filePath("tree");
    std::filesystem::create_directories(root + "/inner");
    ASSERT_TRUE(writeFile(root + "/a.txt"));
    ASSERT_TRUE(writeFile(root + "/inner/b.sh"));
    ::chmod((root + "/a.txt").c_str(), 0600);
    ::chmod((root + "/inner/b.sh").c_str(), 0700);
    ::chmod((root + "/inner").c_str(), 0700);
    std::filesystem::create_symlink("/etc/hostname", root + "/link");

    EXPECT_EQ(applyToContents(root, {Access::ReadWrite, Access::Read, Access::Read}), 0);
    EXPECT_EQ(modeOf(root + "/a.txt"), mode_t(0644));
    EXPECT_EQ(modeOf(root + "/inner"), mode_t(0755));
    // A program stays one.
    EXPECT_EQ(modeOf(root + "/inner/b.sh"), mode_t(0744));
    EXPECT_TRUE(std::filesystem::is_symlink(root + "/link"));
}

TEST(PermissionsTest, NamesOwnersAndGroups)
{
    EXPECT_EQ(userName(0), "root");
    EXPECT_EQ(groupName(0), "root");
    EXPECT_FALSE(userName(::getuid()).empty());
    bool root = false;
    for (const auto& [uid, name] : users())
        root = root || (uid == 0 && name == "root");
    EXPECT_TRUE(root);
}
