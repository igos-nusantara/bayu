// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <string>
#include <vector>

namespace settings::hardware {

// A device on PCI or USB and the driver that has it.
struct Device
{
    enum class Bus { Pci, Usb } bus = Bus::Pci;
    std::string slot;     // 00:02.0, or the USB port path
    std::string kind;     // "VGA compatible controller", "Network controller", ...
    std::string vendor;   // its name
    std::string name;     // its name
    std::string classId;  // PCI: 4 hex digits, as 0280; USB: the interfaces' classes, as "e0 ff"
    std::string vendorId; // 4 hex digits
    std::string deviceId; // 4 hex digits
    std::string driver;   // empty when no driver has it
};

// `lspci -vmmnnk`'s records, without bridges and the chipset's own parts.
std::vector<Device> parseLspci(const std::string& output);
std::vector<Device> pciDevices();
// USB devices from sysfs, without hubs; the driver of their first interface.
std::vector<Device> usbDevices(const std::string& sysfs = "/sys/bus/usb/devices");

// A firmware or driver package a device needs, from what IGOS Nusantara's
// repositories have.
struct Package
{
    std::string name;   // as dnf installs it
    std::string reason; // what it is for, translated
};
std::vector<Package> packagesFor(const std::vector<Device>& devices);

// The devices as text, to paste into a bug report.
std::string report(const std::vector<Device>& devices);

} // namespace settings::hardware
