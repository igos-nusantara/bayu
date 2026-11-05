// SPDX-License-Identifier: BSD-3-Clause

#include <gtest/gtest.h>

#include "pagetest.h"
#include "pages/memory/zram.h"

using namespace settings::memory;

TEST(MemoryPage, ShowsTheCompressionSwitch)
{
    PageFixture page("memory");
    EXPECT_NE(page.dump().find("\"memory:zram\""), std::string::npos);
}

TEST(MemoryPage, ReadsTheZramSize)
{
    EXPECT_EQ(zramModeOf(""), ZramMode::Off);
    EXPECT_EQ(zramModeOf("# Settings > Memory: off\n"), ZramMode::Off);
    EXPECT_EQ(zramModeOf("[zram0]\nzram-size = min(ram / 2, 8192)\n"), ZramMode::Auto);
    EXPECT_EQ(zramModeOf("[zram0]\ncompression-algorithm = zstd\n"), ZramMode::Auto);
    EXPECT_EQ(zramModeOf("[zram0]\nzram-size = ram / 4\n"), ZramMode::Quarter);
    EXPECT_EQ(zramModeOf("[zram0]\nzram-size=ram / 2\n"), ZramMode::Half);
    EXPECT_EQ(zramModeOf("[zram0]\nzram-size = 4096\n"), ZramMode::Custom);
    EXPECT_EQ(zramModeOf("[zram1]\nzram-size = ram / 4\n"), ZramMode::Off);
    EXPECT_EQ(zramArgument(ZramMode::Quarter), "25");
    EXPECT_EQ(zramArgument(ZramMode::Off), "off");
    EXPECT_EQ(zramArgument(ZramMode::Custom), "auto");
}

TEST(MemoryPage, ReadsTheZramUsage)
{
    const ZramUsage usage = zramUsageOf("8589934592\n", "  4096000  1024000  1200000        0  1200000      12      0      0      0\n");
    EXPECT_EQ(usage.size, 8589934592u);
    EXPECT_EQ(usage.stored, 4096000u);
    EXPECT_EQ(usage.memoryUsed, 1200000u);
    EXPECT_EQ(zramUsageOf("", "").size, 0u);
}
