// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <functional>
#include <map>
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
    std::string module;   // the driver's kernel module, as drivers.db names it
    // What drivers match: the device's, or its USB interfaces'.
    std::vector<std::string> modaliases;
};

// `lspci -vmmnnk`'s records, without bridges and the chipset's own parts.
std::vector<Device> parseLspci(const std::string& output);
// With their modaliases and modules from sysfs.
std::vector<Device> pciDevices(const std::string& sysfs = "/sys/bus/pci/devices");
// USB devices from sysfs, without hubs; the driver of their first interface.
std::vector<Device> usbDevices(const std::string& sysfs = "/sys/bus/usb/devices");

// /usr/share/bayu/drivers.db (packaging/kernel/driver-db.sh): what devices
// need from IGOS Nusantara's repositories.
struct DriverDatabase
{
    struct Firmware
    {
        std::string file;
        std::string package;
    };
    struct Alias
    {
        std::string pattern;
        std::string module;
    };
    struct Proprietary
    {
        std::string pattern;
        std::string name;
        std::vector<std::string> packages;
    };
    std::multimap<std::string, Firmware> firmware; // by module
    std::map<std::string, std::string> modules;    // module: its package, outside kernel-modules
    std::vector<Alias> aliases;                    // of those modules
    std::vector<Proprietary> proprietary;

    static DriverDatabase parse(const std::string& text);
    static DriverDatabase load(const std::string& path = "/usr/share/bayu/drivers.db");
};

// The aliases of the modules the running kernel has (modules.alias).
std::vector<DriverDatabase::Alias> parseModulesAlias(const std::string& text);

// What a device needs that is not installed.
struct Package
{
    std::vector<std::string> names; // as dnf installs them
    std::string reason;             // what it is for, translated
    bool proprietary = false;
};

// What to look at on the system, so tests can stand in for it.
struct System
{
    std::vector<DriverDatabase::Alias> installedAliases;
    std::function<bool(const std::string& file)> hasFirmware; // under /lib/firmware
};
System currentSystem();

// The packages the devices need: their proprietary drivers, the firmware their
// modules load and none of which is there, and the modules outside
// kernel-modules for devices without a driver.
std::vector<Package> packagesFor(const std::vector<Device>& devices, const DriverDatabase& database,
                                 const System& system);

// The devices as text, to paste into a bug report.
std::string report(const std::vector<Device>& devices);

} // namespace settings::hardware
