// SPDX-License-Identifier: BSD-3-Clause

#include "pages/hardware/hardware.h"

#include <algorithm>
#include <cstdio>
#include <dirent.h>
#include <filesystem>
#include <map>
#include <tuple>
#include <unistd.h>

#include "base/file.h"
#include "base/i18n.h"
#include "base/strings.h"

using namespace bayu;
using bayu::i18n::trNoop;

namespace settings::hardware {

namespace {

// "Intel Corporation [8086]" into its name and id.
std::pair<std::string, std::string> splitId(const std::string& value)
{
    const size_t open = value.rfind(" [");
    if (open == std::string::npos || value.back() != ']')
        return {value, {}};
    return {value.substr(0, open), value.substr(open + 2, value.size() - open - 3)};
}

std::string readTrimmed(const std::string& path)
{
    return std::string(base::strings::trimmed(base::file::read(path).value_or("")));
}

std::string commandOutput(const char* command)
{
    std::string output;
    if (FILE* pipe = popen(command, "r")) {
        char buffer[4096];
        while (const size_t size = fread(buffer, 1, sizeof buffer, pipe))
            output.append(buffer, size);
        pclose(pipe);
    }
    return output;
}

// The chipset's own parts, that users have nothing to do with: bridges,
// memory and system peripherals, SMBus, signal processing.
bool isChipset(const std::string& classId)
{
    return classId.starts_with("05") || classId.starts_with("06") || classId.starts_with("08")
        || classId == "0c05" || classId.starts_with("11");
}

} // namespace

std::vector<Device> parseLspci(const std::string& output)
{
    std::vector<Device> devices;
    Device device;
    const auto finish = [&] {
        if (!device.slot.empty() && !isChipset(device.classId))
            devices.push_back(device);
        device = Device();
    };
    for (const std::string_view raw : base::strings::splitViews(output, '\n')) {
        const std::string line(raw);
        if (base::strings::trimmed(line).empty()) {
            finish();
            continue;
        }
        const size_t colon = line.find(':');
        if (colon == std::string::npos)
            continue;
        const std::string key = line.substr(0, colon);
        const std::string value(base::strings::trimmed(std::string_view(line).substr(colon + 1)));
        if (key == "Slot") {
            device.slot = value;
        } else if (key == "Class") {
            std::tie(device.kind, device.classId) = splitId(value);
        } else if (key == "Vendor") {
            std::tie(device.vendor, device.vendorId) = splitId(value);
        } else if (key == "Device") {
            std::tie(device.name, device.deviceId) = splitId(value);
        } else if (key == "Driver") {
            device.driver = value;
        }
    }
    finish();
    return devices;
}

std::vector<Device> pciDevices()
{
    return parseLspci(commandOutput("lspci -vmmnnk 2>/dev/null"));
}

std::vector<Device> usbDevices(const std::string& sysfs)
{
    std::vector<Device> devices;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(sysfs, error)) {
        const std::string path = entry.path().string();
        const std::string port = entry.path().filename().string();
        // Devices, not their interfaces (1-2:1.0) or the root hubs (usb1).
        if (port.find(':') != std::string::npos || port.starts_with("usb"))
            continue;
        Device device;
        device.bus = Device::Bus::Usb;
        device.slot = port;
        device.vendorId = readTrimmed(path + "/idVendor");
        device.deviceId = readTrimmed(path + "/idProduct");
        if (device.vendorId.empty() || readTrimmed(path + "/bDeviceClass") == "09")
            continue;
        device.vendor = readTrimmed(path + "/manufacturer");
        device.name = readTrimmed(path + "/product");
        if (device.name.empty())
            device.name = device.vendorId + ":" + device.deviceId;
        device.kind = "USB";
        std::vector<std::string> classes;
        for (const auto& interface : std::filesystem::directory_iterator(path, error)) {
            const std::string name = interface.path().filename().string();
            if (!name.starts_with(port + ":"))
                continue;
            classes.push_back(readTrimmed(interface.path().string() + "/bInterfaceClass"));
            if (device.driver.empty() && std::filesystem::exists(interface.path() / "driver"))
                device.driver = std::filesystem::read_symlink(interface.path() / "driver").filename().string();
        }
        std::sort(classes.begin(), classes.end());
        classes.erase(std::unique(classes.begin(), classes.end()), classes.end());
        device.classId = base::strings::join(classes, " ");
        devices.push_back(device);
    }
    std::sort(devices.begin(), devices.end(), [](const Device& a, const Device& b) { return a.slot < b.slot; });
    return devices;
}

std::vector<Package> packagesFor(const std::vector<Device>& devices)
{
    struct Rule
    {
        Device::Bus bus;
        const char* classPrefix; // PCI class, or a USB interface class it has; empty for any
        const char* vendorId;
        const char* package;
        const char* reason;
    };
    static const Rule rules[] = {
        {Device::Bus::Pci, "0280", "8086", "iwlwifi-mvm-firmware", trNoop("Intel Wi-Fi")},
        {Device::Bus::Pci, "0280", "10ec", "realtek-firmware", trNoop("Realtek Wi-Fi")},
        {Device::Bus::Pci, "0280", "168c", "atheros-firmware", trNoop("Qualcomm Atheros Wi-Fi")},
        {Device::Bus::Pci, "0280", "17cb", "atheros-firmware", trNoop("Qualcomm Atheros Wi-Fi")},
        {Device::Bus::Pci, "0280", "14c3", "mt7xxx-firmware", trNoop("MediaTek Wi-Fi")},
        {Device::Bus::Pci, "0280", "14e4", "brcmfmac-firmware", trNoop("Broadcom Wi-Fi")},
        {Device::Bus::Pci, "03", "1002", "amd-gpu-firmware", trNoop("AMD graphics")},
        {Device::Bus::Pci, "03", "8086", "intel-gpu-firmware", trNoop("Intel graphics")},
        {Device::Bus::Pci, "03", "10de", "nvidia-gpu-firmware", trNoop("NVIDIA graphics")},
        {Device::Bus::Pci, "04", "8086", "alsa-sof-firmware", trNoop("Intel sound")},
        {Device::Bus::Usb, "", "0bda", "realtek-firmware", trNoop("Realtek USB Wi-Fi or Bluetooth")},
        {Device::Bus::Usb, "", "0cf3", "atheros-firmware", trNoop("Qualcomm Atheros USB Wi-Fi or Bluetooth")},
        {Device::Bus::Usb, "", "0e8d", "mt7xxx-firmware", trNoop("MediaTek USB Wi-Fi or Bluetooth")},
    };
    std::vector<Package> packages;
    for (const Device& device : devices) {
        for (const Rule& rule : rules) {
            if (rule.bus != device.bus || device.vendorId != rule.vendorId)
                continue;
            if (device.bus == Device::Bus::Pci && !device.classId.starts_with(rule.classPrefix))
                continue;
            // USB: only what has a wireless (e0) or vendor (ff) interface, as
            // Wi-Fi adapters, not a card reader of the same maker.
            if (device.bus == Device::Bus::Usb && device.classId.find("e0") == std::string::npos
                && device.classId.find("ff") == std::string::npos)
                continue;
            const bool known = std::any_of(packages.begin(), packages.end(),
                                           [&](const Package& package) { return package.name == rule.package; });
            if (!known)
                packages.push_back({rule.package, i18n::tr(rule.reason)});
        }
    }
    return packages;
}

std::string report(const std::vector<Device>& devices)
{
    std::string text;
    for (const Device& device : devices) {
        text += device.bus == Device::Bus::Pci ? "PCI " : "USB ";
        text += device.slot + "  " + device.kind + ": " + device.vendor + " " + device.name;
        text += " [" + device.vendorId + ":" + device.deviceId + "]";
        text += device.driver.empty() ? std::string("  (no driver)") : "  driver: " + device.driver;
        text += '\n';
    }
    return text;
}

} // namespace settings::hardware
