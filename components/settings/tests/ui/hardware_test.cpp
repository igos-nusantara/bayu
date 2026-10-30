// SPDX-License-Identifier: BSD-3-Clause

#include <gtest/gtest.h>

#include "pagetest.h"
#include "pages/hardware/hardware.h"

using namespace settings::hardware;

namespace {

const char kLspci[] = R"(Slot:	00:00.0
Class:	Host bridge [0600]
Vendor:	Intel Corporation [8086]
Device:	12th Gen Core Processor Host Bridge [4621]

Slot:	00:02.0
Class:	VGA compatible controller [0300]
Vendor:	Intel Corporation [8086]
Device:	Alder Lake-P GT2 [Iris Xe Graphics] [46a6]
Driver:	i915
Module:	i915

Slot:	02:00.0
Class:	Network controller [0280]
Vendor:	Realtek Semiconductor Co., Ltd. [10ec]
Device:	RTL8852BE PCIe 802.11ax Wireless Network Controller [b852]
Module:	rtw89_8852be
)";

} // namespace

TEST(HardwarePage, ShowsTheDevices)
{
    PageFixture page("hardware");
    EXPECT_NE(page.dump().find("\"hardware:copy\""), std::string::npos);
}

TEST(HardwarePage, ReadsLspciWithoutTheChipset)
{
    const std::vector<Device> devices = parseLspci(kLspci);
    ASSERT_EQ(devices.size(), 2u);
    EXPECT_EQ(devices[0].slot, "00:02.0");
    EXPECT_EQ(devices[0].classId, "0300");
    EXPECT_EQ(devices[0].vendorId, "8086");
    EXPECT_EQ(devices[0].name, "Alder Lake-P GT2 [Iris Xe Graphics]");
    EXPECT_EQ(devices[0].driver, "i915");
    EXPECT_EQ(devices[1].vendor, "Realtek Semiconductor Co., Ltd.");
    EXPECT_EQ(devices[1].driver, "");
}

TEST(HardwarePage, RecommendsTheFirmwareOfTheDevices)
{
    std::vector<Device> devices = parseLspci(kLspci);
    Device usbWifi;
    usbWifi.bus = Device::Bus::Usb;
    usbWifi.vendorId = "0e8d";
    usbWifi.classId = "e0 ff";
    devices.push_back(usbWifi);
    Device cardReader;
    cardReader.bus = Device::Bus::Usb;
    cardReader.vendorId = "0bda";
    cardReader.classId = "08";
    devices.push_back(cardReader);

    std::vector<std::string> names;
    for (const Package& package : packagesFor(devices))
        names.push_back(package.name);
    EXPECT_EQ(names, (std::vector<std::string>{"intel-gpu-firmware", "realtek-firmware", "mt7xxx-firmware"}));
    EXPECT_NE(report(devices).find("(no driver)"), std::string::npos);
}
