// SPDX-License-Identifier: GPL-3.0-or-later
// What the installer shows and asks for, independent of the backend that
// provides it (Anaconda over D-Bus, or sample data).

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace installer {

struct SystemInfo
{
    // "IGOS Nusantara 26", from the product the backend installs.
    std::string productName;
    std::string productShortName;
    std::string productVersion;
    std::string backendVersion;
    std::string architecture;
    bool uefi = false;
    bool secureBoot = false;
};

struct Language
{
    // "en", "id", "zh"...
    std::string id;
    std::string nativeName;
    std::string englishName;
    // The territory its default locale is for, in the language: "Indonesia".
    std::string territory;
};

struct Locale
{
    // "en_US.UTF-8".
    std::string id;
    // "United States".
    std::string territoryName;
    // "US".
    std::string territoryCode;
};

struct KeyboardLayout
{
    // As X names it: "us", "us (intl)", "id".
    std::string id;
    // "English (US)".
    std::string name;
    // "qwerty · default", "with dead keys".
    std::string detail;
    // Two letters for the badge.
    std::string code;
};

struct TimeSettings
{
    // "Asia/Jakarta".
    std::string timezone;
    bool ntp = true;
    bool geolocation = false;
    // pool.ntp.org, when NTP is on.
    std::string ntpServer;
};

struct NetworkConnection
{
    enum class Kind {
        Wired,
        Wireless,
        Hotspot,
    };
    // NetworkManager's device or access point path.
    std::string id;
    Kind kind = Kind::Wired;
    // "Wired · enp3s0", "Kantor-5G".
    std::string name;
    // "Connected · 1 Gbps", "WPA2 · secured", "Open network".
    std::string detail;
    // 0 to 100 for Wi-Fi.
    int strength = 0;
    bool secured = false;
    bool connected = false;
    bool connecting = false;
};

struct NetworkDetails
{
    std::string ipv4;
    std::string gateway;
    std::string dns;
    std::string linkSpeed;
    std::string hardwareAddress;
};

// A piece of a disk as it is now, or as it will be: a partition, or space
// between them.
struct DiskSegment
{
    // The backend's device name, empty for free space.
    std::string device;
    uint64_t start = 0;
    uint64_t size = 0;
    bool free = false;
    // "ntfs", "ext4", "efi", "swap", "luks"...
    std::string format;
    std::string label;
    std::string mountPoint;
    // "Windows Boot Manager", "Microsoft basic data", "IGOS 25"...
    std::string description;
    // Data in it the plan keeps, reuses or loses.
    bool existing = true;
    bool encrypted = false;
};

struct Disk
{
    // "nvme0n1".
    std::string id;
    // "/dev/nvme0n1".
    std::string path;
    // "Samsung 990 PRO".
    std::string model;
    // "NVMe", "SATA SSD", "USB 3.2".
    std::string bus;
    uint64_t size = 0;
    uint64_t freeSpace = 0;
    uint64_t reclaimableSpace = 0;
    // "Windows 11", ... the operating systems found on it.
    std::vector<std::string> systems;
    std::vector<DiskSegment> segments;
    bool removable = false;
    // The medium this installer runs from: never offered.
    bool installationMedia = false;
};

// One mount point of the resulting layout, for the legend and the review.
struct LayoutPart
{
    std::string mountPoint;
    uint64_t size = 0;
    std::string format;
};

struct ValidationReport
{
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    // The EFI partition kept from another system has no room for the boot
    // loader: the review offers a new one (StoragePlan::newEsp).
    bool espFull = false;
    bool ok() const { return errors.empty(); }
};

struct StoragePlan
{
    enum class Method {
        Automatic,
        Custom,
    };
    // How Automatic treats what is on the disk already.
    enum class ExistingData {
        // Erase the disk.
        Replace,
        // Install into its free space only.
        UseFreeSpace,
        // Shrink one partition (Windows) and install beside it.
        Alongside,
    };
    std::vector<std::string> disks;
    Method method = Method::Automatic;
    ExistingData existing = ExistingData::Replace;
    // Alongside: the partition to shrink, and its new size.
    std::string shrinkDevice;
    uint64_t shrinkTo = 0;
    bool encrypt = false;
    // "btrfs", "lvm", "plain".
    std::string scheme = "btrfs";
    // Automatic: a new EFI partition in the free space, instead of the full
    // one kept from another system.
    bool newEsp = false;
};

// One change the custom editor has scheduled, for its summary.
struct PendingAction
{
    // "destroy", "create", "resize", "format".
    std::string kind;
    std::string description;
    bool destructive = false;
};

struct SoftwareEnvironment
{
    std::string id;
    std::string name;
    std::string description;
    // "monitor", "server", "box", "layers"...
    std::string icon;
    uint64_t installedSize = 0;
    bool available = true;
};

struct SoftwareAddon
{
    std::string id;
    std::string name;
    std::string description;
    bool selected = false;
    // Needs a network connection (packages come from the repositories).
    bool needsNetwork = true;
};

struct SoftwareSummary
{
    int packages = 0;
    uint64_t downloadSize = 0;
    uint64_t installedSize = 0;
    int estimatedMinutes = 0;
};

struct Account
{
    std::string fullName;
    std::string userName;
    std::string password;
    bool administrator = true;
    bool requirePassword = true;
    bool lockRoot = true;
    std::string rootPassword;
    // A picture file for the account, empty for the initials.
    std::string picture;
};

struct InstallProgress
{
    // 0 to 1.
    double fraction = 0;
    // "Installing 864 of 1,842 · gnome-shell-45.2-1.el10.x86_64".
    std::string status;
    // Seconds left, negative while unknown.
    int remainingSeconds = -1;
};

struct InstallLogLine
{
    enum class State {
        Done,
        Running,
        Failed,
    };
    State state = State::Done;
    std::string text;
};

struct InstallResult
{
    bool succeeded = false;
    std::string error;
    int seconds = 0;
    int packages = 0;
    bool encrypted = false;
};

} // namespace installer
