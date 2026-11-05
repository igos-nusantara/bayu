// SPDX-License-Identifier: BSD-3-Clause

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <unistd.h>

#include "pages/accounts/autologin.h"

TEST(AccountsPage, ReadsTheAutomaticLogin)
{
    char path[] = "/tmp/bayu-dm-conf-XXXXXX";
    const int fd = mkstemp(path);
    ASSERT_GE(fd, 0);
    close(fd);
    std::ofstream(path) << "[Session]\nVt=2\n\n[Autologin]\nUser=budi\n";
    EXPECT_EQ(settings::autologinUser(path), "budi");
    std::ofstream(path) << "[Session]\nVt=2\n";
    EXPECT_EQ(settings::autologinUser(path), "");
    std::remove(path);
    EXPECT_EQ(settings::autologinUser("/nonexistent/bayu-dm.conf"), "");
}
