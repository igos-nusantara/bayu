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

namespace {

const char kDatabase[] = "# test\n"
                         "firmware\tamdgpu\tamdgpu/navi10_pfp.bin\tamd-gpu-firmware\n"
                         "firmware\tiwlwifi\tiwlwifi-ty-a0-gf-a0-89.ucode\tiwlwifi-mvm-firmware\n"
                         "firmware\tiwlwifi\tiwlwifi-7260-17.ucode\tiwlwifi-dvm-firmware\n"
                         "module\tbtusb_extra\tkernel-modules-extra\n"
                         "alias\tusb:v0BDAp8771d*\tbtusb_extra\n"
                         "proprietary\tpci:v000010DEd*sv*sd*bc03sc*i*\tNVIDIA\takmod-nvidia-580xx xorg-x11-drv-nvidia-580xx\n";

Device pciDevice(const char* vendor, const char* name, const char* modalias, const char* module)
{
    Device device;
    device.vendor = vendor;
    device.name = name;
    device.modaliases = {modalias};
    device.module = module;
    device.driver = module;
    return device;
}

std::vector<std::vector<std::string>> namesOf(const std::vector<Package>& packages)
{
    std::vector<std::vector<std::string>> names;
    for (const Package& package : packages)
        names.push_back(package.names);
    return names;
}

} // namespace

TEST(HardwarePage, ReadsTheDriverDatabase)
{
    const DriverDatabase database = DriverDatabase::parse(kDatabase);
    EXPECT_EQ(database.firmware.count("iwlwifi"), 2u);
    EXPECT_EQ(database.modules.at("btusb_extra"), "kernel-modules-extra");
    ASSERT_EQ(database.proprietary.size(), 1u);
    EXPECT_EQ(database.proprietary[0].packages,
              (std::vector<std::string>{"akmod-nvidia-580xx", "xorg-x11-drv-nvidia-580xx"}));
    EXPECT_EQ(parseModulesAlias("alias pci:v00008086d*sv*sd*bc02sc80i* iwlwifi\n# x\n")[0].module, "iwlwifi");
}

TEST(HardwarePage, OffersWhatTheDevicesNeed)
{
    const DriverDatabase database = DriverDatabase::parse(kDatabase);
    System system;
    // Only the dvm firmware is there.
    system.hasFirmware = [](const std::string& file) { return file == "iwlwifi-7260-17.ucode"; };

    // NVIDIA, with nouveau: its proprietary driver.
    Device nvidia = pciDevice("NVIDIA", "GA106", "pci:v000010DEd00002504sv00001462sd0000397Dbc03sc00i00", "nouveau");
    // AMD graphics, firmware missing.
    Device amd = pciDevice("AMD", "Navi 10", "pci:v00001002d0000731Fsv*sd*bc03sc00i00", "amdgpu");
    // Intel Wi-Fi: one of its firmware files is there.
    Device intel = pciDevice("Intel", "Wi-Fi 6E", "pci:v00008086d00002725sv*sd*bc02sc80i00", "iwlwifi");
    // A USB device without a driver, whose module is in kernel-modules-extra.
    Device usb;
    usb.bus = Device::Bus::Usb;
    usb.name = "Bluetooth";
    usb.modaliases = {"usb:v0BDAp8771d0200dcE0dsc01dp01icE0isc01ip01in00"};

    const std::vector<Package> packages = packagesFor({nvidia, amd, intel, usb}, database, system);
    EXPECT_EQ(namesOf(packages), (std::vector<std::vector<std::string>>{
                                         {"akmod-nvidia-580xx", "xorg-x11-drv-nvidia-580xx"},
                                         {"amd-gpu-firmware"},
                                         {"kernel-modules-extra"}}));
    EXPECT_TRUE(packages[0].proprietary);
    EXPECT_FALSE(packages[1].proprietary);

    // A device without a driver whose installed module needs firmware.
    System installed = system;
    installed.installedAliases = {{"pci:v00001002d*sv*sd*bc03sc*i*", "amdgpu"}};
    Device unbound = amd;
    unbound.driver.clear();
    unbound.module.clear();
    EXPECT_EQ(namesOf(packagesFor({unbound}, database, installed)),
              (std::vector<std::vector<std::string>>{{"amd-gpu-firmware"}}));
    EXPECT_NE(report({unbound}).find("(no driver)"), std::string::npos);
}
