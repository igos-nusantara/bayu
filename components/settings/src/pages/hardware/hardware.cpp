// SPDX-License-Identifier: BSD-3-Clause

#include "pages/hardware/hardware.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fnmatch.h>
#include <set>
#include <sys/utsname.h>
#include <tuple>
#include <unistd.h>

#include "base/file.h"
#include "base/i18n.h"
#include "base/strings.h"

using namespace bayu;
using bayu::i18n::format;
using bayu::i18n::tr;

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

// The kernel module behind a sysfs device's driver, as modules name it.
std::string moduleOf(const std::filesystem::path& device)
{
    std::error_code error;
    const auto module = std::filesystem::read_symlink(device / "driver" / "module", error);
    return error ? std::string() : module.filename().string();
}

// The chipset's own parts, that users have nothing to do with: bridges,
// memory and system peripherals, SMBus, signal processing.
bool isChipset(const std::string& classId)
{
    return classId.starts_with("05") || classId.starts_with("06") || classId.starts_with("08")
        || classId == "0c05" || classId.starts_with("11");
}

bool matches(const std::string& pattern, const std::string& modalias)
{
    return fnmatch(pattern.c_str(), modalias.c_str(), 0) == 0;
}

std::string displayName(const Device& device)
{
    return device.vendor.empty() ? device.name : device.vendor + " " + device.name;
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

std::vector<Device> pciDevices(const std::string& sysfs)
{
    std::vector<Device> devices = parseLspci(commandOutput("lspci -D -vmmnnk 2>/dev/null"));
    for (Device& device : devices) {
        const std::filesystem::path path = std::filesystem::path(sysfs) / device.slot;
        if (const std::string modalias = readTrimmed(path.string() + "/modalias"); !modalias.empty())
            device.modaliases.push_back(modalias);
        device.module = moduleOf(path);
        // 0000:00:02.0 is 00:02.0 to people.
        if (device.slot.starts_with("0000:"))
            device.slot = device.slot.substr(5);
    }
    return devices;
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
        std::set<std::string> classes;
        for (const auto& interface : std::filesystem::directory_iterator(path, error)) {
            if (!interface.path().filename().string().starts_with(port + ":"))
                continue;
            const std::string dir = interface.path().string();
            classes.insert(readTrimmed(dir + "/bInterfaceClass"));
            if (const std::string modalias = readTrimmed(dir + "/modalias"); !modalias.empty())
                device.modaliases.push_back(modalias);
            if (device.driver.empty() && std::filesystem::exists(interface.path() / "driver")) {
                device.driver = std::filesystem::read_symlink(interface.path() / "driver").filename().string();
                device.module = moduleOf(interface.path());
            }
        }
        device.classId = base::strings::join(std::vector<std::string>(classes.begin(), classes.end()), " ");
        devices.push_back(device);
    }
    std::sort(devices.begin(), devices.end(), [](const Device& a, const Device& b) { return a.slot < b.slot; });
    return devices;
}

DriverDatabase DriverDatabase::parse(const std::string& text)
{
    DriverDatabase database;
    for (const std::string_view line : base::strings::splitViews(text, '\n')) {
        if (line.empty() || line.front() == '#')
            continue;
        const std::vector<std::string> fields = base::strings::split(std::string(line), '\t');
        if (fields[0] == "firmware" && fields.size() == 4)
            database.firmware.emplace(fields[1], Firmware{fields[2], fields[3]});
        else if (fields[0] == "module" && fields.size() == 3)
            database.modules[fields[1]] = fields[2];
        else if (fields[0] == "alias" && fields.size() == 3)
            database.aliases.push_back({fields[1], fields[2]});
        else if (fields[0] == "proprietary" && fields.size() == 4)
            database.proprietary.push_back({fields[1], fields[2], base::strings::splitWhitespace(fields[3])});
    }
    return database;
}

DriverDatabase DriverDatabase::load(const std::string& path)
{
    return parse(base::file::read(path).value_or(""));
}

std::vector<DriverDatabase::Alias> parseModulesAlias(const std::string& text)
{
    std::vector<DriverDatabase::Alias> aliases;
    for (const std::string_view line : base::strings::splitViews(text, '\n')) {
        const std::vector<std::string> fields = base::strings::splitWhitespace(std::string(line));
        if (fields.size() == 3 && fields[0] == "alias")
            aliases.push_back({fields[1], fields[2]});
    }
    return aliases;
}

System currentSystem()
{
    utsname kernel{};
    uname(&kernel);
    System system;
    system.installedAliases = parseModulesAlias(
            base::file::read(std::string("/lib/modules/") + kernel.release + "/modules.alias").value_or(""));
    system.hasFirmware = [](const std::string& file) {
        const std::string path = "/lib/firmware/" + file;
        return access(path.c_str(), F_OK) == 0 || access((path + ".xz").c_str(), F_OK) == 0
            || access((path + ".zst").c_str(), F_OK) == 0;
    };
    return system;
}

std::vector<Package> packagesFor(const std::vector<Device>& devices, const DriverDatabase& database,
                                 const System& system)
{
    std::vector<Package> packages;
    std::set<std::string> offered;
    const auto offer = [&](std::vector<std::string> names, std::string reason, bool proprietary) {
        names.erase(std::remove_if(names.begin(), names.end(),
                                   [&](const std::string& name) { return offered.count(name) > 0; }),
                    names.end());
        if (names.empty())
            return;
        offered.insert(names.begin(), names.end());
        packages.push_back({std::move(names), std::move(reason), proprietary});
    };

    for (const Device& device : devices) {
        const std::string name = displayName(device);
        for (const auto& driver : database.proprietary) {
            if (std::any_of(device.modaliases.begin(), device.modaliases.end(),
                            [&](const std::string& modalias) { return matches(driver.pattern, modalias); }))
                offer(driver.packages, format(tr("{driver} driver for {device}"), {{"driver", driver.name}, {"device", name}}),
                      true);
        }

        // The modules that would drive it: its own, or for a device without one,
        // those whose aliases match it.
        std::set<std::string> modules;
        if (!device.module.empty())
            modules.insert(device.module);
        if (device.driver.empty()) {
            for (const std::string& modalias : device.modaliases) {
                for (const auto& alias : system.installedAliases) {
                    if (matches(alias.pattern, modalias))
                        modules.insert(alias.module);
                }
                // Not installed: in a package of their own, as kernel-modules-extra.
                for (const auto& alias : database.aliases) {
                    const auto package = database.modules.find(alias.module);
                    if (package != database.modules.end() && matches(alias.pattern, modalias))
                        offer({package->second}, format(tr("Driver for {device}"), {{"device", name}}), false);
                }
            }
        }

        // Firmware a module loads, none of which is there.
        for (const std::string& module : modules) {
            const auto [begin, end] = database.firmware.equal_range(module);
            if (begin == end)
                continue;
            const bool present = std::any_of(begin, end, [&](const auto& entry) {
                return system.hasFirmware && system.hasFirmware(entry.second.file);
            });
            if (present)
                continue;
            std::vector<std::string> names;
            for (auto it = begin; it != end; ++it) {
                if (std::find(names.begin(), names.end(), it->second.package) == names.end())
                    names.push_back(it->second.package);
            }
            offer(names, format(tr("Firmware for {device}"), {{"device", name}}), false);
        }
    }
    return packages;
}

std::string report(const std::vector<Device>& devices)
{
    std::string text;
    for (const Device& device : devices) {
        text += device.bus == Device::Bus::Pci ? "PCI " : "USB ";
        text += device.slot + "  " + device.kind + ": " + displayName(device);
        text += " [" + device.vendorId + ":" + device.deviceId + "]";
        text += device.driver.empty() ? std::string("  (no driver)") : "  driver: " + device.driver;
        text += '\n';
    }
    return text;
}

} // namespace settings::hardware
