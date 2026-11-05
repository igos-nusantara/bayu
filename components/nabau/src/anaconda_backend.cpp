// SPDX-License-Identifier: GPL-3.0-or-later
// The installer's backend on a live system: Anaconda's modules on their
// private bus, started as root by nabau-backend, plus
// NetworkManager for the network and the session for the keyboard.

#include <crypt.h>
#include <expat.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <deque>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include "backend.h"
#include "base/dbus.h"
#include "base/event_loop.h"
#include "base/i18n.h"
#include "base/process.h"
#include "dbus_util.h"
#include "network_manager.h"
#include "ui/application.h"

using bayu::base::dbus::Connection;
using bayu::base::dbus::Match;
using bayu::base::dbus::Message;
using bayu::base::dbus::ObjectPath;
using bayu::base::dbus::Proxy;
using bayu::base::dbus::Reply;
using bayu::base::dbus::Variant;
using bayu::base::dbus::VariantMap;
using bayu::i18n::format;
using bayu::i18n::tr;
using installer::dbus::errorText;
using installer::dbus::get;

namespace installer {

namespace {

constexpr uint64_t MiB = 1024ull * 1024;
// What the boot loader takes on the EFI partition (the live image's /boot/efi
// is about 9 MB), with room to spare.
constexpr uint64_t kEspNeeded = 20 * MiB;
constexpr const char* kHelper = "/usr/libexec/nabau/nabau-backend";
constexpr const char* kConfig = "/etc/nabau/nabau.conf";
constexpr const char* kRules = "/usr/share/X11/xkb/rules/evdev.xml";

constexpr const char* kBoss = "org.fedoraproject.Anaconda.Boss";
constexpr const char* kStorage = "org.fedoraproject.Anaconda.Modules.Storage";
constexpr const char* kStoragePath = "/org/fedoraproject/Anaconda/Modules/Storage";
constexpr const char* kAppliedTree = "/org/fedoraproject/Anaconda/Modules/Storage/DeviceTree";
constexpr const char* kTask = "org.fedoraproject.Anaconda.Task";

// Device types of a factory request.
constexpr int32_t kPartition = 2;
// Partitioning schemes of an automatic request.
constexpr int32_t kBtrfs = 1;
constexpr int32_t kLvm = 2;

using Strings = std::vector<std::string>;
using StringMap = std::map<std::string, std::string>;

std::string command(const std::string& line)
{
    std::string out;
    if (FILE* pipe = popen(line.c_str(), "r")) {
        char buffer[4096];
        size_t read;
        while ((read = fread(buffer, 1, sizeof(buffer), pipe)) > 0)
            out.append(buffer, read);
        pclose(pipe);
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == ' '))
        out.pop_back();
    return out;
}

std::string readFile(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    std::stringstream out;
    out << file.rdbuf();
    return out.str();
}

// KEY=value lines, the way the shell writes them.
StringMap readShellConfig(const std::string& path)
{
    StringMap values;
    std::istringstream in(readFile(path));
    for (std::string line; std::getline(in, line);) {
        const size_t equals = line.find('=');
        if (line.empty() || line[0] == '#' || equals == std::string::npos)
            continue;
        std::string value = line.substr(equals + 1);
        if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') && value.back() == value.front())
            value = value.substr(1, value.size() - 2);
        values[line.substr(0, equals)] = value;
    }
    return values;
}

// "Deutsch (Österreich)" -> "Österreich".
std::string parenthesized(const std::string& text)
{
    const size_t open = text.find('(');
    const size_t close = text.rfind(')');
    if (open == std::string::npos || close == std::string::npos || close < open)
        return {};
    return text.substr(open + 1, close - open - 1);
}

// The territory of "de_AT.UTF-8": "AT".
std::string territoryOf(const std::string& locale)
{
    const size_t underscore = locale.find('_');
    if (underscore == std::string::npos)
        return {};
    const size_t end = locale.find_first_of(".@", underscore);
    return locale.substr(underscore + 1, end == std::string::npos ? std::string::npos : end - underscore - 1);
}

std::string lower(std::string text)
{
    for (char& c : text)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

std::string upper(std::string text)
{
    for (char& c : text)
        c = char(std::toupper(static_cast<unsigned char>(c)));
    return text;
}

// The number at the end of a partition's name: 3 for nvme0n1p3.
int partitionNumber(const std::string& name)
{
    size_t digits = name.size();
    while (digits > 0 && std::isdigit(static_cast<unsigned char>(name[digits - 1])))
        --digits;
    return digits == name.size() ? 0 : std::stoi(name.substr(digits));
}

std::string cryptPassword(const std::string& password)
{
    char setting[CRYPT_GENSALT_OUTPUT_SIZE];
    if (!crypt_gensalt_rn("$y$", 0, nullptr, 0, setting, sizeof(setting)))
        return {};
    auto data = std::make_unique<crypt_data>();
    const char* hash = crypt_r(password.c_str(), setting, data.get());
    return hash && hash[0] != '*' ? hash : std::string();
}

std::string valueOf(const StringMap& map, const std::string& key)
{
    const auto it = map.find(key);
    return it == map.end() ? std::string() : it->second;
}

ValidationReport reportFrom(const VariantMap& map)
{
    return {get<Strings>(map, "error-messages"), get<Strings>(map, "warning-messages")};
}

ValidationReport errorReport(const std::string& error)
{
    ValidationReport report;
    report.errors.push_back(error);
    return report;
}

// Keyboard layouts as xkeyboard-config lists them.
struct XkbLayout
{
    // Anaconda's form: "us (intl)".
    std::string id;
    // Bayu's form: "us-intl".
    std::string bayuId;
    std::string description;
    std::string shortDescription;
    std::string layout;
};

std::vector<XkbLayout> readXkbLayouts()
{
    struct Reader
    {
        std::vector<XkbLayout> layouts;
        bool inList = false;
        bool inVariant = false;
        int depth = 0;
        std::string* reading = nullptr;
        std::string layout;
        std::string layoutShort;
        std::string name;
        std::string shortDescription;
        std::string description;
    } reader;

    const std::string contents = readFile(kRules);
    std::unique_ptr<XML_ParserStruct, decltype(&XML_ParserFree)> parser(XML_ParserCreate("UTF-8"), XML_ParserFree);
    XML_SetUserData(parser.get(), &reader);
    XML_SetElementHandler(
        parser.get(),
        [](void* data, const XML_Char* element, const XML_Char**) {
            auto* r = static_cast<Reader*>(data);
            const std::string_view tag = element;
            ++r->depth;
            if (tag == "layoutList") {
                r->inList = true;
            } else if (!r->inList) {
                return;
            } else if (tag == "variant") {
                r->inVariant = true;
            } else if (tag == "configItem") {
                r->name.clear();
                r->shortDescription.clear();
                r->description.clear();
            } else if (tag == "name" && r->name.empty()) {
                r->reading = &r->name;
            } else if (tag == "shortDescription") {
                r->reading = &r->shortDescription;
            } else if (tag == "description") {
                r->reading = &r->description;
            }
        },
        [](void* data, const XML_Char* element) {
            auto* r = static_cast<Reader*>(data);
            const std::string_view tag = element;
            --r->depth;
            r->reading = nullptr;
            if (!r->inList)
                return;
            if (tag == "layoutList") {
                r->inList = false;
            } else if (tag == "variant") {
                r->inVariant = false;
            } else if (tag == "configItem" && !r->name.empty()) {
                if (!r->inVariant) {
                    r->layout = r->name;
                    r->layoutShort = r->shortDescription;
                }
                if (r->layout == "custom")
                    return;
                XkbLayout entry;
                entry.layout = r->layout;
                entry.id = r->inVariant ? r->layout + " (" + r->name + ")" : r->layout;
                entry.bayuId = r->inVariant ? r->layout + "-" + r->name : r->layout;
                entry.description = r->description;
                entry.shortDescription = r->shortDescription.empty() ? r->layoutShort : r->shortDescription;
                r->layouts.push_back(entry);
            }
        });
    XML_SetCharacterDataHandler(parser.get(), [](void* data, const XML_Char* text, int length) {
        auto* r = static_cast<Reader*>(data);
        if (r->reading)
            r->reading->append(text, size_t(length));
    });
    XML_Parse(parser.get(), contents.data(), int(contents.size()), XML_TRUE);
    return reader.layouts;
}

std::string toBayuLayout(const std::string& id)
{
    const size_t open = id.find(" (");
    if (open == std::string::npos)
        return id;
    return id.substr(0, open) + "-" + id.substr(open + 2, id.size() - open - 3);
}

class AnacondaBackend : public Backend
{
public:
    AnacondaBackend()
    {
        m_network.onChanged = [this] {
            if (onNetworkChanged)
                onNetworkChanged();
        };
    }

    ~AnacondaBackend() override { quit(); }

    // Startup.

    void start(std::function<void(const std::string&)> status, std::function<void(const std::string&)> ready) override
    {
        m_status = std::move(status);
        m_ready = std::move(ready);
        m_status(tr("Starting the installation backend"));
        m_helper.setStandardInputOpen(true);
        m_helper.onStandardOutput = [this](std::string_view data) {
            m_buffer += data;
            for (size_t end; (end = m_buffer.find('\n')) != std::string::npos;) {
                const std::string line = m_buffer.substr(0, end);
                m_buffer.erase(0, end + 1);
                helperLine(line);
            }
        };
        m_helper.onFinished = [this](int code, bool) {
            if (m_ready) {
                // pkexec: 126 when the password was refused or the dialog closed.
                fail(code == 126 || code == 127
                         ? tr("The installer needs administrator rights, which were not given.")
                         : (m_startError.empty() ? tr("The installation backend stopped unexpectedly. Its log is in "
                                                      "/tmp/anaconda.log.")
                                                 : m_startError));
            } else if (m_postDone) {
                finishPostInstall(1);
            }
        };
        if (!m_helper.start("pkexec", {kHelper, std::to_string(getpid())}))
            fail(tr("Cannot start the installation backend (pkexec is missing)."));
    }

    SystemInfo systemInfo() const override { return m_info; }

    // Localization.

    std::vector<Language> languages() const override { return m_languages; }

    std::vector<Locale> locales(const std::string& language) const override
    {
        std::vector<Locale> out;
        const Proxy localization = module("Localization");
        Strings ids;
        localization.call("GetLocales", language).read(ids);
        for (const std::string& id : ids) {
            if (!isInstalled(id))
                continue;
            VariantMap data;
            localization.call("GetLocaleData", id).read(data);
            std::string name = parenthesized(get<std::string>(data, "native-name"));
            if (name.empty())
                name = get<std::string>(data, "english-name");
            out.push_back({id, name, territoryOf(id)});
        }
        return out;
    }

    std::string locale() const override { return m_locale; }

    void setLocale(const std::string& locale) override
    {
        m_locale = locale;
        module("Localization").setProperty("Language", locale);
    }

    std::vector<KeyboardLayout> keyboardLayouts(const std::string& language) const override
    {
        std::string english;
        for (const Language& entry : m_languages) {
            if (entry.id == language)
                english = entry.englishName;
        }
        const std::string territory = lower(territoryOf(m_locale));
        std::vector<const XkbLayout*> first;
        std::vector<const XkbLayout*> rest;
        for (const XkbLayout& layout : m_xkb) {
            const bool forTerritory = layout.layout == territory;
            const bool forLanguage = !english.empty() && layout.description.starts_with(english);
            if (forTerritory && layout.id == layout.layout)
                first.insert(first.begin(), &layout);
            else if (forTerritory || forLanguage || layout.id == "us" || layout.id == m_layout)
                (forTerritory ? first : rest).push_back(&layout);
        }
        std::stable_sort(rest.begin(), rest.end(),
                         [](const XkbLayout* a, const XkbLayout* b) { return a->description < b->description; });
        first.insert(first.end(), rest.begin(), rest.end());
        std::vector<KeyboardLayout> out;
        for (const XkbLayout* layout : first) {
            std::string code = upper(layout->shortDescription.empty() ? layout->layout : layout->shortDescription);
            out.push_back({layout->id, layout->description, layout->id, code.substr(0, 3)});
        }
        return out;
    }

    std::string keyboardLayout() const override { return m_layout; }

    void setKeyboardLayout(const std::string& layout) override
    {
        m_layout = layout;
        module("Localization").setProperty("XLayouts", Strings{layout});
        // The session switches at once, for the test field.
        Proxy(Connection::session(), "org.bayu.Keyboard1", "/org/bayu/Keyboard1", "org.bayu.Keyboard1")
            .send("setLayout", toBayuLayout(layout));
    }

    // Time.

    std::vector<std::pair<std::string, std::vector<std::string>>> timezones() const override { return m_zones; }

    TimeSettings timeSettings() const override { return m_time; }

    void setTimeSettings(const TimeSettings& settings) override
    {
        m_time = settings;
        const Proxy timezone = module("Timezone");
        timezone.call("SetTimezoneWithPriority", settings.timezone, uint16_t(90));
        timezone.setProperty("NTPEnabled", settings.ntp);
    }

    // Network.

    std::vector<NetworkConnection> connections() const override { return m_network.connections(); }
    NetworkDetails details(const std::string& connection) const override { return m_network.details(connection); }
    void connect(const std::string& connection, const std::string& password,
                 std::function<void(const std::string&)> done) override
    {
        m_network.connect(connection, password, std::move(done));
    }
    std::string hostname() const override { return m_hostname; }
    void setHostname(const std::string& hostname) override
    {
        m_hostname = hostname;
        module("Network").setProperty("Hostname", hostname);
    }
    bool connectAutomatically() const override { return m_autoconnect; }
    void setConnectAutomatically(bool automatic) override { m_autoconnect = automatic; }
    bool isOnline() const override { return m_network.isOnline(); }

    // Storage.

    std::vector<Disk> disks() const override { return m_disks; }

    void rescan(std::function<void()> done) override
    {
        ObjectPath task;
        storage().call("ScanDevicesWithTask").read(task);
        runTask(kStorage, task.path, {}, [this, done](const std::string&) {
            storage().call("ResetPartitioning");
            m_automatic = {};
            m_probe = {};
            m_custom = {};
            loadDisks();
            done();
        });
    }

    void planStorage(const StoragePlan& plan,
                     std::function<void(const std::vector<LayoutPart>&, const ValidationReport&)> done) override
    {
        serialize([this, plan, done](std::function<void()> next) {
            auto finish = [this, done, next](const Partitioning& partitioning, const ValidationReport& report) {
                done(partitioning.tree.empty() ? std::vector<LayoutPart>{} : readLayout(partitioning.tree), report);
                next();
            };
            if (plan.method == StoragePlan::Method::Custom)
                configureCustom(m_placeholder, finish);
            else
                configureAutomatic(plan, m_placeholder, finish);
        });
    }

    uint64_t minimumSize(const std::string& device) const override { return sizeLimits(device).first; }

    void applyStorage(const StoragePlan& plan, const std::string& passphrase,
                      std::function<void(const ValidationReport&)> done) override
    {
        m_encrypted = plan.method == StoragePlan::Method::Automatic && plan.encrypt;
        serialize([this, plan, passphrase, done](std::function<void()> next) {
            auto apply = [this, done, next](const Partitioning& partitioning, ValidationReport report) {
                if (report.ok())
                    checkEsp(partitioning.tree, report);
                if (report.ok()) {
                    const Reply reply = storage().call("ApplyPartitioning", ObjectPath{partitioning.path});
                    if (reply.isError())
                        report.errors.push_back(errorText(reply));
                }
                m_applied = report.ok() ? readLayout(kAppliedTree) : std::vector<LayoutPart>{};
                done(report);
                next();
            };
            if (plan.method == StoragePlan::Method::Custom)
                configureCustom(passphrase, apply);
            else if (plan.newEsp)
                configureNewEsp(plan, passphrase, apply);
            else
                configureAutomatic(plan, passphrase, apply);
        });
    }

    std::vector<LayoutPart> appliedLayout() const override { return m_applied; }
    uint64_t requiredSpace() const override { return m_required; }

    // The custom editor.

    void beginCustom(const std::vector<std::string>& disks) override
    {
        if (!m_custom.path.empty() && m_customDisks == disks)
            return;
        m_customDisks = disks;
        m_luks.clear();
        m_custom = createPartitioning("INTERACTIVE", disks);
        // A disk without a partition table gets one, so there is free space
        // to create in. Nothing is lost: the disk is blank.
        const Proxy view = viewer(m_custom.tree);
        for (const std::string& disk : disks) {
            const VariantMap data = deviceData(view, disk);
            if (get<Strings>(data, "children").empty() && formatData(view, disk).count("type")
                && get<std::string>(formatData(view, disk), "type").empty())
                scheduler().call("DestroyDevice", disk);
        }
    }

    std::vector<Disk> customDisks() const override
    {
        std::vector<Disk> out;
        if (m_custom.tree.empty())
            return out;
        const std::set<std::string> created = createdDevices(m_custom.tree);
        for (const std::string& disk : m_customDisks)
            out.push_back(readDisk(m_custom.tree, disk, created));
        return out;
    }

    std::string addMount(const MountRequest& request) override
    {
        std::string mountPoint = request.mountPoint;
        std::string fileSystem = request.format;
        if (mountPoint == "swap") {
            mountPoint.clear();
            fileSystem = "swap";
        }
        if (!mountPoint.empty()) {
            if (std::string error = validateMountPoint(mountPoint); !error.empty())
                return error;
        }
        static const std::map<std::string, int32_t> types{
            {"partition", kPartition}, {"lvm", 0}, {"lvm-thin", 5}, {"btrfs", 3}};
        const auto type = types.find(request.type);
        const VariantMap factory{{"device-type", type == types.end() ? kPartition : type->second},
                                 {"disks", Strings{request.disk}},
                                 {"mount-point", mountPoint},
                                 {"format-type", fileSystem},
                                 {"device-size", request.size},
                                 {"device-encrypted", request.encrypt},
                                 {"label", request.label},
                                 {"reformat", true}};
        const std::string error = errorText(scheduler().call("AddDevice", factory));
        if (error.empty() && request.encrypt)
            m_encryptedMounts = true;
        return error;
    }

    std::string changeMount(const MountRequest& request) override
    {
        const std::string leaf = leafOf(m_custom.tree, request.device);
        const std::string type = get<std::string>(formatData(viewer(m_custom.tree), leaf), "type");
        if (type == "lvmpv" || type == "btrfs")
            return tr("This partition holds LVM or Btrfs volumes. Delete it to use its space.");
        VariantMap original;
        const Reply reply = scheduler().call("GenerateDeviceFactoryRequest", leaf);
        if (!reply.read(original))
            return errorText(reply);
        std::string mountPoint = request.mountPoint;
        std::string fileSystem = request.format;
        if (mountPoint == "swap") {
            mountPoint.clear();
            fileSystem = "swap";
        }
        if (!mountPoint.empty() && mountPoint != get<std::string>(original, "mount-point")) {
            if (std::string error = validateMountPoint(mountPoint); !error.empty())
                return error;
        }
        VariantMap changed = original;
        changed["mount-point"] = mountPoint;
        changed["reformat"] = request.reformat;
        if (request.reformat) {
            changed["format-type"] = fileSystem;
            changed["device-encrypted"] = request.encrypt;
            changed["label"] = request.label;
        }
        return errorText(scheduler().call("ChangeDevice", changed, original));
    }

    std::string resize(const std::string& device, uint64_t size) override
    {
        const std::string leaf = leafOf(m_custom.tree, device);
        VariantMap original;
        const Reply reply = scheduler().call("GenerateDeviceFactoryRequest", leaf);
        if (!reply.read(original))
            return errorText(reply);
        VariantMap permissions;
        scheduler().call("GenerateDeviceFactoryPermissions", original).read(permissions);
        if (!get<bool>(permissions, "device-size"))
            return tr("This partition cannot be resized.");
        VariantMap changed = original;
        changed["device-size"] = size;
        return errorText(scheduler().call("ChangeDevice", changed, original));
    }

    // What is inside goes first: Anaconda destroys leaves only (the volumes
    // of an LVM group, then the group, then the partition holding it).
    std::string remove(const std::string& device) override
    {
        VariantMap data;
        if (!viewer(m_custom.tree).call("GetDeviceData", device).read(data))
            return {};
        for (const std::string& child : get<Strings>(data, "children")) {
            if (std::string error = remove(child); !error.empty())
                return error;
        }
        // A group goes with its last volume.
        if (!viewer(m_custom.tree).call("GetDeviceData", device).read(data))
            return {};
        return errorText(scheduler().call("DestroyDevice", device));
    }

    std::string createRecommended(bool encrypt) override
    {
        if (m_customDisks.empty())
            return tr("Choose a disk first.");
        const std::string disk = m_customDisks.front();
        const Proxy view = viewer(m_custom.tree);
        StringMap mounts;
        view.call("GetMountPoints").read(mounts);
        if (mounts.count("/"))
            return tr("This disk already has a root (/) mount point.");
        auto add = [&](const std::string& mountPoint, const std::string& fileSystem, uint64_t size, bool encrypted) {
            const VariantMap factory{{"device-type", kPartition},  {"disks", Strings{disk}},
                                     {"mount-point", mountPoint},  {"format-type", fileSystem},
                                     {"device-size", size},        {"device-encrypted", encrypted},
                                     {"reformat", true}};
            return errorText(scheduler().call("AddDevice", factory));
        };
        if (m_info.uefi && !mounts.count("/boot/efi")) {
            // Beside Windows, its EFI partition takes the new boot loader too.
            std::string existing;
            const std::set<std::string> created = createdDevices(m_custom.tree);
            for (const std::string& child : get<Strings>(deviceData(view, disk), "children")) {
                if (!created.count(child) && get<std::string>(formatData(view, child), "type") == "efi")
                    existing = child;
            }
            if (m_newEsp)
                existing.clear();
            std::string error = existing.empty() ? std::string("none") : std::string();
            if (!existing.empty()) {
                VariantMap original;
                scheduler().call("GenerateDeviceFactoryRequest", existing).read(original);
                VariantMap changed = original;
                changed["mount-point"] = std::string("/boot/efi");
                changed["reformat"] = false;
                error = errorText(scheduler().call("ChangeDevice", changed, original));
            }
            if (!error.empty()) {
                if (std::string added = add("/boot/efi", "efi", 600 * MiB, false); !added.empty())
                    return added;
            }
        }
        if (!m_info.uefi && !hasFormat(m_custom.tree, disk, "biosboot")) {
            // Only GPT disks need it; on an MBR disk this fails and is not needed.
            add({}, "biosboot", MiB, false);
        }
        if (!mounts.count("/boot")) {
            if (std::string error = add("/boot", "ext4", 1024 * MiB, false); !error.empty())
                return error;
        }
        uint64_t free = 0;
        view.call("GetDiskFreeSpace", Strings{disk}).read(free);
        // A little under all of it, for the partitions' alignment.
        if (free < 8 * 1024 * MiB)
            return tr("There is not enough free space left for the system.");
        if (std::string error = add("/", "", free - 4 * MiB, encrypt); !error.empty())
            return error;
        if (encrypt)
            m_encryptedMounts = true;
        return {};
    }

    void resetCustom() override
    {
        m_custom = {};
        const std::vector<std::string> disks = m_customDisks;
        m_customDisks.clear();
        beginCustom(disks);
    }

    std::vector<PendingAction> pendingActions() const override
    {
        std::vector<PendingAction> out;
        if (m_custom.tree.empty())
            return out;
        std::vector<VariantMap> actions;
        viewer(m_custom.tree).call("GetActions").read(actions);
        std::set<std::string> destroyed;
        std::set<std::string> created;
        for (const VariantMap& action : actions) {
            if (get<std::string>(action, "object-type") != "device")
                continue;
            const std::string type = get<std::string>(action, "action-type");
            (type == "destroy" ? destroyed : created).insert(get<std::string>(action, "device-name"));
        }
        for (const VariantMap& action : actions) {
            const std::string type = get<std::string>(action, "action-type");
            const std::string object = get<std::string>(action, "object-type");
            const std::string device = get<std::string>(action, "device-name");
            // A device destroyed or created says it for its format too.
            if (object == "format" && ((type == "destroy" && destroyed.count(device))
                                       || (type == "create" && created.count(device))))
                continue;
            PendingAction pending;
            pending.kind = type == "create" && object == "format" ? "format" : type;
            if (pending.kind != "destroy" && pending.kind != "resize" && pending.kind != "format")
                pending.kind = "create";
            pending.destructive = type == "destroy";
            std::string description = get<std::string>(action, "action-description");
            if (!description.empty())
                description[0] = char(std::toupper(static_cast<unsigned char>(description[0])));
            pending.description = description + " · " + get<std::string>(action, "device-description");
            const StringMap attrs = get<StringMap>(action, "attrs");
            if (const auto it = attrs.find("mount-point"); it != attrs.end() && !it->second.empty())
                pending.description += " · " + it->second;
            out.push_back(pending);
        }
        return out;
    }

    std::vector<std::string> fileSystems() const override { return m_fileSystems; }

    std::vector<std::string> deviceTypes() const override
    {
        Strings types{"partition", "lvm", "lvm-thin"};
        if (m_scheme == kBtrfs)
            types.push_back("btrfs");
        return types;
    }

    std::vector<DiskSegment> volumes(const std::string& device) const override
    {
        std::vector<DiskSegment> out;
        if (m_custom.tree.empty())
            return out;
        const Proxy view = viewer(m_custom.tree);
        const std::set<std::string> created = createdDevices(m_custom.tree);
        // Partition, then LUKS, then group or volume, then the volumes.
        std::vector<std::string> queue{device};
        for (size_t i = 0; i < queue.size() && i < 64; ++i) {
            const VariantMap data = deviceData(view, queue[i]);
            const std::string type = get<std::string>(data, "type");
            if (type == "lvmlv" || type == "lvmthinlv" || type == "btrfs subvolume") {
                const VariantMap format = formatData(view, queue[i]);
                DiskSegment volume;
                volume.device = queue[i];
                volume.size = get<uint64_t>(data, "size");
                volume.format = get<std::string>(format, "type");
                volume.mountPoint = volume.format == "swap" ? "swap"
                                                            : valueOf(get<StringMap>(format, "attrs"), "mount-point");
                volume.existing = !created.count(queue[i]);
                volume.description = get<std::string>(data, "name");
                out.push_back(volume);
            }
            for (const std::string& child : get<Strings>(data, "children"))
                queue.push_back(child);
        }
        return out;
    }

    std::vector<std::string> suggestedMountPoints() const override
    {
        Strings points{"/", "/home", "/boot"};
        if (m_info.uefi)
            points.push_back("/boot/efi");
        points.insert(points.end(), {"swap", "/var", "/srv", "/opt", "/tmp"});
        return points;
    }

    std::pair<uint64_t, uint64_t> sizeLimits(const std::string& device) const override
    {
        if (!m_custom.tree.empty() && createdDevices(m_custom.tree).count(device)) {
            // A new partition grows into the free space of its disk.
            const Proxy view = viewer(m_custom.tree);
            const VariantMap data = deviceData(view, device);
            uint64_t free = 0;
            const Strings parents = get<Strings>(data, "parents");
            if (!parents.empty())
                view.call("GetDiskFreeSpace", parents).read(free);
            return {MiB, get<uint64_t>(data, "size") + free};
        }
        const Partitioning& probe = probePartitioning();
        const Proxy resizable(m_bus.get(), kStorage, probe.tree,
                              std::string(kStorage) + ".DeviceTree.Resizable");
        std::tuple<uint64_t, uint64_t> limits{0, 0};
        if (!resizable.call("GetDeviceSizeLimits", device).read(limits)) {
            const uint64_t size = get<uint64_t>(deviceData(viewer(kAppliedTree), device), "size");
            return {size, size};
        }
        return {std::get<0>(limits), std::get<1>(limits)};
    }

    // Software. The live image is copied as it is; add-ons are package
    // groups installed into the copy from the repositories afterwards.

    std::vector<SoftwareEnvironment> environments() const override
    {
        return {{"live", m_info.productShortName.empty() ? tr("This live system") : m_info.productShortName + " Desktop",
                 tr("The desktop on this media, as you see it now: Files, Terminal, Settings"), "monitor", m_required,
                 true}};
    }

    std::vector<SoftwareAddon> addons(const std::string&) const override
    {
        return {{"development", tr("Development Tools"), tr("gcc, make, git, headers"), false},
                {"container-management", tr("Container Tools"), tr("podman, buildah, skopeo"), false},
                {"rpm-development-tools", tr("RPM Development"), tr("rpmbuild, mock"), false},
                {"headless-management", tr("Headless Management"), tr("cockpit, remote administration"), false},
                {"scientific", tr("Scientific Support"), tr("Math and science libraries"), false}};
    }

    void setSoftware(const std::string&, const std::vector<std::string>& addons) override { m_addons = addons; }

    SoftwareSummary softwareSummary() const override
    {
        // ponytail: add-ons' sizes are not known until dnf resolves them; the
        // estimate counts the image copy plus two minutes per add-on.
        const int minutes = int(std::max<uint64_t>(3, m_required / (60 * 1000 * 1000) / 60)) + 2 * int(m_addons.size());
        return {m_packageCount, 0, m_required, minutes};
    }

    // Account.

    void setAccount(const Account& account) override
    {
        m_account = account;
        VariantMap user{{"name", account.userName},
                        {"gecos", account.fullName},
                        {"groups", account.administrator ? Strings{"wheel"} : Strings{}},
                        {"password", cryptPassword(account.password)},
                        {"is-crypted", true}};
        const Proxy users = module("Users");
        users.setProperty("Users", std::vector<VariantMap>{user});
        if (account.lockRoot) {
            users.call("ClearRootPassword");
        } else {
            users.call("SetCryptedRootPassword", cryptPassword(account.rootPassword));
            users.setProperty("IsRootAccountLocked", false);
        }
    }

    // Installation.

    void install(std::function<void(const InstallProgress&)> progress, std::function<void(const InstallLogLine&)> log,
                 std::function<void(const InstallResult&)> done) override
    {
        m_progress = std::move(progress);
        m_log = std::move(log);
        m_done = std::move(done);
        m_started = std::chrono::steady_clock::now();
        // The medium first: a damaged copy of the image would fail while the
        // system is copied, after the disks have changed (issue #65).
        logStep(tr("Checking the installation medium"));
        report(0, tr("Checking the installation medium"));
        m_checkingMedia = true;
        m_helper.writeStandardInput("checkmedia\n");
    }

    void mediaChecked(const std::string& verdict)
    {
        m_checkingMedia = false;
        if (verdict == "bad") {
            finish(false, tr("The installation medium is damaged: part of it cannot be read as it was made. "
                             "Nothing on the disks has changed. Copy the image to the USB drive again, check "
                             "its SHA-256, or use another USB drive."));
            return;
        }
        startInstallation();
    }

    void startInstallation()
    {
        // Anaconda would start initial-setup on the first boot: the account
        // exists already.
        module("Services").setProperty("SetupOnBoot", int32_t(0));
        // Anaconda looks for display managers it knows; bayu-dm is not one.
        module("Services").setProperty("DefaultTarget", std::string("graphical.target"));
        std::vector<ObjectPath> tasks;
        const Reply reply =
            Proxy(m_bus.get(), kBoss, "/org/fedoraproject/Anaconda/Boss", kBoss).call("InstallWithTasks");
        if (!reply.read(tasks) || tasks.empty()) {
            finish(false, errorText(reply));
            return;
        }
        m_steps = 0;
        const std::string path = tasks.front().path;
        runTask(
            kBoss, path,
            [this, path](int step, const std::string& message) {
                if (m_steps <= 0)
                    m_steps = Proxy(m_bus.get(), kBoss, path, kTask).property<int32_t>("Steps").value_or(0);
                logStep(message);
                report(installFraction(step, message), message);
            },
            [this](const std::string& error) {
                if (!error.empty()) {
                    finish(false, error);
                    return;
                }
                postInstall();
            });
    }

    bool canCancelInstall() const override { return false; }
    void cancelInstall() override {}

    std::string logText() const override
    {
        std::string text;
        // The ends of the logs: a whole one would not fit a label.
        constexpr size_t kTail = 64 * 1024;
        for (const char* path : {"/tmp/nabau-install.log", "/tmp/anaconda.log", "/tmp/storage.log", "/tmp/nabau-post.log"}) {
            std::string contents = readFile(path);
            if (contents.size() > kTail)
                contents = "…" + contents.substr(contents.find('\n', contents.size() - kTail) + 1);
            text += format(tr("==> {path} <=="), {{"path", path}}) + "\n";
            text += contents.empty() ? std::string(tr("(cannot be read)")) + "\n" : contents;
            text += "\n";
        }
        return text;
    }

    void reboot() override
    {
        m_helper.writeStandardInput("reboot\n");
        quit();
        // Anaconda leaves once this process is gone; the helper then restarts.
        bayu::ui::Application::instance()->quit();
    }

    void quit() override
    {
        if (m_helper.isRunning())
            m_helper.closeStandardInput();
    }

private:
    struct Partitioning
    {
        std::string path;
        std::string tree;
    };

    struct TaskRun
    {
        Proxy task;
        Match progress;
        Match stopped;
        bool finished = false;
    };

    void fail(const std::string& error)
    {
        if (!m_ready)
            return;
        auto ready = std::move(m_ready);
        m_ready = nullptr;
        ready(error);
    }

    void helperLine(const std::string& line)
    {
        const size_t space = line.find(' ');
        const std::string word = line.substr(0, space);
        const std::string rest = space == std::string::npos ? std::string() : line.substr(space + 1);
        if (word == "status" && m_ready) {
            m_status(rest);
        } else if (word == "error") {
            m_startError = rest;
        } else if (word == "ready") {
            connectBus(rest);
        } else if (word == "mediaprogress" && m_checkingMedia) {
            const int percent = std::clamp(std::atoi(rest.c_str()), 0, 100);
            // The check takes the first 5% of the bar.
            report(0.05 * percent / 100, format(tr("Checking the installation medium · {percent}%"),
                                               {{"percent", std::to_string(percent)}}));
        } else if (word == "media" && m_checkingMedia) {
            mediaChecked(rest);
        } else if (word == "progress" && m_postDone) {
            logStep(rest);
            report(std::min(0.99, m_fraction + 0.01), rest);
        } else if (word == "packages") {
            m_packageCount = std::atoi(rest.c_str());
        } else if (word == "done" && m_postDone) {
            finishPostInstall(std::atoi(rest.c_str()));
        }
    }

    void connectBus(const std::string& address)
    {
        m_bus = Connection::openAddress(address);
        if (!m_bus) {
            fail(format(tr("Cannot connect to the installation backend at {address}."), {{"address", address}}));
            return;
        }
        m_status(tr("Reading the disks"));
        // Let the status paint before the reads that follow.
        bayu::base::EventLoop::current()->post([this] {
            load();
            fail({});
        });
    }

    void load()
    {
        const StringMap config = readShellConfig(kConfig);
        const VariantMap product =
            Proxy(m_bus.get(), "org.fedoraproject.Anaconda.Modules.Runtime",
                  "/org/fedoraproject/Anaconda/Modules/Runtime/UserInterface",
                  "org.fedoraproject.Anaconda.Modules.Runtime.UserInterface")
                .property<VariantMap>("ProductData")
                .value_or(VariantMap{});
        auto value = [&config](const char* key, const std::string& fallback) {
            const auto it = config.find(key);
            return it == config.end() || it->second.empty() ? fallback : it->second;
        };
        m_info.productName = value("PRODUCT_NAME", get<std::string>(product, "name"));
        m_info.productVersion = get<std::string>(product, "version");
        m_info.productShortName = value("SHORT_NAME", m_info.productName);
        m_info.backendVersion = command("rpm -q --qf '%{VERSION}' anaconda-core");
        // "40.22", without the distribution's own numbers after it.
        if (const size_t dot = m_info.backendVersion.find('.', m_info.backendVersion.find('.') + 1);
            dot != std::string::npos)
            m_info.backendVersion.resize(dot);
        utsname system{};
        uname(&system);
        m_info.architecture = system.machine;
        bool efi = false;
        storageObject("Bootloader").call("IsEFI").read(efi);
        m_info.uefi = efi;
        const std::string secureBoot =
            readFile("/sys/firmware/efi/efivars/SecureBoot-8be4df61-93ca-11d2-aa0d-00e098032b8c");
        m_info.secureBoot = !secureBoot.empty() && secureBoot.back() == 1;

        loadLocalization();

        std::map<std::string, Strings> zones;
        module("Timezone").call("GetAllValidTimezones").read(zones);
        m_zones.assign(zones.begin(), zones.end());
        const Proxy timezone = module("Timezone");
        m_time.timezone = timezone.property<std::string>("Timezone").value_or(std::string());
        if (m_time.timezone.empty())
            m_time.timezone = "Asia/Jakarta";
        m_time.ntp = timezone.property<bool>("NTPEnabled").value_or(true);
        m_time.geolocation =
            !get<std::string>(timezone.property<VariantMap>("GeolocationResult").value_or(VariantMap{}), "timezone")
                 .empty();
        std::istringstream chrony(readFile("/etc/chrony.conf"));
        for (std::string line; std::getline(chrony, line) && m_time.ntpServer.empty();) {
            std::istringstream words(line);
            std::string keyword, host;
            words >> keyword >> host;
            if (keyword == "pool" || keyword == "server")
                m_time.ntpServer = host;
        }

        m_hostname = module("Network").property<std::string>("Hostname").value_or(std::string());
        if (m_hostname.empty() || m_hostname.starts_with("localhost")) {
            // The live system's name, else one from the product's.
            m_hostname = command("cat /etc/hostname");
        }
        if (m_hostname.empty() || m_hostname.starts_with("localhost")) {
            m_hostname.clear();
            for (char c : lower(m_info.productShortName)) {
                if (std::isalnum(static_cast<unsigned char>(c)))
                    m_hostname += c;
                else if (!m_hostname.empty() && m_hostname.back() != '-')
                    m_hostname += '-';
            }
            while (!m_hostname.empty() && m_hostname.back() == '-')
                m_hostname.pop_back();
            if (m_hostname.empty())
                m_hostname = "localhost";
        }

        Strings supported;
        viewer(kAppliedTree).call("GetSupportedFileSystems").read(supported);
        std::string preferred;
        Proxy(m_bus.get(), kStorage, kStoragePath, kStorage).call("GetDefaultFileSystem").read(preferred);
        auto supports = [&supported](const std::string& type) {
            return std::find(supported.begin(), supported.end(), type) != supported.end();
        };
        m_scheme = supports("btrfs") ? kBtrfs : kLvm;
        for (const char* type : {"xfs", "ext4", "btrfs", "efi", "swap", "biosboot", "vfat"}) {
            if (supports(type))
                m_fileSystems.push_back(type);
        }
        if (const auto it = std::find(m_fileSystems.begin(), m_fileSystems.end(), preferred);
            it != m_fileSystems.end())
            std::rotate(m_fileSystems.begin(), it, it + 1);

        uint64_t payload = 0;
        module("Payloads").call("CalculateRequiredSpace").read(payload);
        viewer(kAppliedTree).call("GetRequiredDeviceSize", payload).read(m_required);
        // Anaconda counts the live image's compressed size (1.4 GB); the
        // copy takes 2.5 GB, and the new system room to start.
        m_required = std::max<uint64_t>(m_required, 4000 * MiB);
        m_packageCount = std::atoi(command("rpm -qa | wc -l").c_str());

        loadDisks();
    }

    // The system copied has the locales of the live media only: those
    // `locale -a` lists. All of them when it lists none.
    bool isInstalled(const std::string& locale) const
    {
        return m_installedLocales.empty() || m_installedLocales.count(locale.substr(0, locale.find_first_of(".@")));
    }

    void loadLocalization()
    {
        std::istringstream installed(command("locale -a"));
        for (std::string line; std::getline(installed, line);) {
            if (line.find('_') != std::string::npos)
                m_installedLocales.insert(line.substr(0, line.find_first_of(".@")));
        }
        const Proxy localization = module("Localization");
        Strings ids;
        localization.call("GetLanguages").read(ids);
        for (const std::string& id : ids) {
            Strings locales;
            localization.call("GetLocales", id).read(locales);
            std::erase_if(locales, [this](const std::string& locale) { return !isInstalled(locale); });
            if (locales.empty())
                continue;
            VariantMap data;
            localization.call("GetLanguageData", id).read(data);
            Language language{id, get<std::string>(data, "native-name"), get<std::string>(data, "english-name"), {}};
            {
                VariantMap locale;
                localization.call("GetLocaleData", locales.front()).read(locale);
                language.territory = parenthesized(get<std::string>(locale, "native-name"));
            }
            m_languages.push_back(language);
        }
        // English and Indonesian first, the rest by name.
        auto rank = [](const Language& language) { return language.id == "en" ? 0 : language.id == "id" ? 1 : 2; };
        std::stable_sort(m_languages.begin(), m_languages.end(), [&rank](const Language& a, const Language& b) {
            return rank(a) != rank(b) ? rank(a) < rank(b) : a.englishName < b.englishName;
        });

        m_locale = localization.property<std::string>("Language").value_or(std::string());
        if (m_locale.empty())
            m_locale = "en_US.UTF-8";
        // Anaconda's messages and descriptions in the installer's language.
        // ponytail: English until the installer is translated.
        Proxy(m_bus.get(), kBoss, "/org/fedoraproject/Anaconda/Boss", kBoss).call("SetLocale", std::string("en_US.UTF-8"));

        m_xkb = readXkbLayouts();
        // The layout the session types with now, else what Anaconda has.
        const std::string live = Proxy(Connection::session(), "org.bayu.Keyboard1", "/org/bayu/Keyboard1",
                                       "org.bayu.Keyboard1")
                                     .property<std::string>("layout")
                                     .value_or(std::string());
        for (const XkbLayout& layout : m_xkb) {
            if (!live.empty() && layout.bayuId == live)
                m_layout = layout.id;
        }
        if (m_layout.empty()) {
            const Strings layouts = localization.property<Strings>("XLayouts").value_or(Strings{});
            m_layout = layouts.empty() ? "us" : layouts.front();
        }
        localization.setProperty("XLayouts", Strings{m_layout});
    }

    void loadDisks()
    {
        const Proxy view = viewer(kAppliedTree);
        Strings disks;
        view.call("GetDisks").read(disks);
        Strings usable;
        storageObject("DiskSelection").call("GetUsableDisks").read(usable);
        m_usable = usable;

        // Linux systems, by the disks they are on.
        m_systems.clear();
        std::vector<VariantMap> systems;
        view.call("GetExistingSystems").read(systems);
        for (const VariantMap& system : systems) {
            Strings ancestors;
            view.call("GetAncestors", get<Strings>(system, "devices")).read(ancestors);
            for (const std::string& ancestor : ancestors)
                m_systems[ancestor].push_back(get<std::string>(system, "os-name"));
        }

        m_disks.clear();
        for (const std::string& name : disks) {
            const bool isUsable = std::find(usable.begin(), usable.end(), name) != usable.end();
            Disk disk = readDisk(kAppliedTree, name, {});
            if (!isUsable && !disk.installationMedia)
                continue;
            disk.installationMedia = disk.installationMedia || !isUsable;
            m_disks.push_back(disk);
        }
    }

    // Proxies.

    Proxy module(const std::string& name) const
    {
        const std::string service = "org.fedoraproject.Anaconda.Modules." + name;
        return Proxy(m_bus.get(), service, "/org/fedoraproject/Anaconda/Modules/" + name, service);
    }

    Proxy storage() const { return Proxy(m_bus.get(), kStorage, kStoragePath, kStorage); }

    Proxy storageObject(const std::string& name) const
    {
        return Proxy(m_bus.get(), kStorage, std::string(kStoragePath) + "/" + name, std::string(kStorage) + "." + name);
    }

    Proxy viewer(const std::string& tree) const
    {
        return Proxy(m_bus.get(), kStorage, tree, std::string(kStorage) + ".DeviceTree.Viewer");
    }

    Proxy scheduler() const
    {
        return Proxy(m_bus.get(), kStorage, m_custom.tree, std::string(kStorage) + ".DeviceTree.Scheduler");
    }

    static VariantMap deviceData(const Proxy& view, const std::string& name)
    {
        VariantMap data;
        view.call("GetDeviceData", name).read(data);
        return data;
    }

    static VariantMap formatData(const Proxy& view, const std::string& name)
    {
        VariantMap data;
        view.call("GetFormatData", name).read(data);
        return data;
    }

    // Tasks run in Anaconda; Stopped says they are over, and Finish raises
    // what failed.
    void runTask(const std::string& service, const std::string& path,
                 std::function<void(int, const std::string&)> progress, std::function<void(const std::string&)> done)
    {
        auto run = std::make_shared<TaskRun>();
        run->task = Proxy(m_bus.get(), service, path, kTask);
        if (progress) {
            run->progress = run->task.onSignal("ProgressChanged", [progress](const Message& message) {
                int32_t step = 0;
                std::string text;
                message.read(step, text);
                progress(step, text);
            });
        }
        std::weak_ptr<TaskRun> weak = run;
        run->stopped = run->task.onSignal("Stopped", [this, weak, done](const Message&) {
            const std::shared_ptr<TaskRun> run = weak.lock();
            if (!run || run->finished)
                return;
            run->finished = true;
            const std::string error = errorText(run->task.call("Finish"));
            fprintf(stderr, "nabau: task %s finished%s%s\n", run->task.path().c_str(),
                    error.empty() ? "" : ": ", error.c_str());
            // Dropped once this callback is over.
            bayu::base::EventLoop::current()->post([this, run] { m_tasks.erase(run); });
            done(error);
        });
        m_tasks.insert(run);
        fprintf(stderr, "nabau: task %s starting\n", path.c_str());
        const Reply reply = run->task.call("Start");
        if (reply.isError()) {
            m_tasks.erase(run);
            done(errorText(reply));
        }
    }

    // Storage work one at a time: Anaconda's partitionings share the disk
    // selection and the initialization mode.
    void serialize(std::function<void(std::function<void()> next)> job)
    {
        m_storageQueue.push_back(std::move(job));
        runStorageQueue();
    }

    void runStorageQueue()
    {
        if (m_storageBusy || m_storageQueue.empty())
            return;
        m_storageBusy = true;
        auto job = std::move(m_storageQueue.front());
        m_storageQueue.pop_front();
        job([this] {
            m_storageBusy = false;
            bayu::base::EventLoop::current()->post([this] { runStorageQueue(); });
        });
    }

    void runTaskOf(const Proxy& object, const std::string& method,
                   std::function<void(const std::string& error, const std::string& task)> done)
    {
        ObjectPath task;
        const Reply reply = object.call(method);
        if (!reply.read(task)) {
            done(errorText(reply), {});
            return;
        }
        runTask(object.service(), task.path, {}, [done, task](const std::string& error) { done(error, task.path); });
    }

    // Partitioning.

    Partitioning createPartitioning(const std::string& method, const Strings& disks)
    {
        storageObject("DiskSelection").setProperty("SelectedDisks", disks);
        storageObject("Bootloader").setProperty("Drive", std::string());
        Partitioning partitioning;
        ObjectPath path;
        storage().call("CreatePartitioning", method).read(path);
        partitioning.path = path.path;
        ObjectPath tree;
        Proxy(m_bus.get(), kStorage, path.path, std::string(kStorage) + ".Partitioning").call("GetDeviceTree").read(tree);
        partitioning.tree = tree.path;
        // The tree is a copy of the disks made on first use: make it now,
        // while the disks selected are these.
        viewer(partitioning.tree).call("GetDisks");
        return partitioning;
    }

    // An automatic partitioning of every usable disk, for the limits of
    // shrinking what is on them.
    const Partitioning& probePartitioning() const
    {
        if (m_probe.path.empty())
            m_probe = const_cast<AnacondaBackend*>(this)->createPartitioning("AUTOMATIC", m_usable);
        return m_probe;
    }

    void configureAutomatic(const StoragePlan& plan, const std::string& passphrase,
                            std::function<void(const Partitioning&, const ValidationReport&)> done)
    {
        const bool erase = plan.existing == StoragePlan::ExistingData::Replace;
        // A copy of the disks configured once keeps what it made: each plan
        // starts from a new one.
        // ponytail: Anaconda keeps every copy until it exits; fine for the
        // dozens a session makes.
        m_automatic = createPartitioning("AUTOMATIC", plan.disks);
        if (plan.existing == StoragePlan::ExistingData::Alongside && !plan.shrinkDevice.empty()) {
            const Reply reply = Proxy(m_bus.get(), kStorage, m_automatic.tree,
                                      std::string(kStorage) + ".DeviceTree.Resizable")
                                    .call("ShrinkDevice", plan.shrinkDevice, plan.shrinkTo);
            if (reply.isError()) {
                done({}, errorReport(errorText(reply)));
                return;
            }
        }
        const Partitioning partitioning = m_automatic;
        const Proxy automatic(m_bus.get(), kStorage, partitioning.path,
                              std::string(kStorage) + ".Partitioning.Automatic");
        VariantMap request{{"partitioning-scheme", m_scheme}, {"encrypted", plan.encrypt}};
        if (plan.encrypt)
            request["passphrase"] = passphrase;
        automatic.setProperty("Request", request);
        storageObject("DiskSelection").setProperty("SelectedDisks", plan.disks);
        storageObject("Bootloader").setProperty("Drive", std::string());
        const Proxy initialization = storageObject("DiskInitialization");
        initialization.setProperty("InitializationMode", int32_t(erase ? 1 : 0));
        initialization.setProperty("InitializeLabelsEnabled", true);
        validate(partitioning, done);
    }

    // Automatic's layout made by hand, with an EFI partition of its own:
    // the one there is too full (checkEsp).
    void configureNewEsp(const StoragePlan& plan, const std::string& passphrase,
                         std::function<void(const Partitioning&, const ValidationReport&)> done)
    {
        m_custom = {};
        m_customDisks.clear();
        beginCustom(plan.disks);
        if (plan.existing == StoragePlan::ExistingData::Alongside && !plan.shrinkDevice.empty()) {
            if (std::string error = resize(plan.shrinkDevice, plan.shrinkTo); !error.empty()) {
                done({}, errorReport(error));
                return;
            }
        }
        m_newEsp = true;
        const std::string error = createRecommended(plan.encrypt);
        m_newEsp = false;
        if (!error.empty()) {
            done({}, errorReport(error));
            return;
        }
        configureCustom(passphrase, done);
    }

    // An EFI partition kept from another system (Windows makes 100 MB ones,
    // which vendors fill) can be too small for the boot loader: copying the
    // system then fails a few percent in, after the disks have changed.
    void checkEsp(const std::string& tree, ValidationReport& report) const
    {
        if (!m_info.uefi)
            return;
        StringMap mounts;
        viewer(tree).call("GetMountPoints").read(mounts);
        const std::string device = valueOf(mounts, "/boot/efi");
        if (device.empty())
            return;
        // Made or formatted by the installer: empty.
        std::vector<VariantMap> actions;
        viewer(tree).call("GetActions").read(actions);
        for (const VariantMap& action : actions) {
            if (get<std::string>(action, "action-type") == "create" && get<std::string>(action, "device-name") == device)
                return;
        }
        const std::string mountPoint = "/tmp/nabau-esp";
        const Proxy handler(m_bus.get(), kStorage, tree, std::string(kStorage) + ".DeviceTree.Handler");
        if (handler.call("MountDevice", device, mountPoint, std::string("ro")).isError())
            return;
        struct statvfs fs {};
        const bool read = statvfs(mountPoint.c_str(), &fs) == 0;
        handler.call("UnmountDevice", device, mountPoint);
        const uint64_t free = uint64_t(fs.f_bavail) * fs.f_frsize;
        if (!read || free >= kEspNeeded)
            return;
        report.espFull = true;
        report.errors.push_back(format(tr("The EFI partition {device} has {free} MB free; the boot loader needs "
                                          "{needed} MB."),
                                       {{"device", device},
                                        {"free", std::to_string(free / MiB)},
                                        {"needed", std::to_string(kEspNeeded / MiB)}}));
    }

    void configureCustom(const std::string& passphrase,
                         std::function<void(const Partitioning&, const ValidationReport&)> done)
    {
        if (m_custom.path.empty()) {
            done({}, errorReport(tr("Lay out the disk first.")));
            return;
        }
        const Proxy handler(m_bus.get(), kStorage, m_custom.tree, std::string(kStorage) + ".DeviceTree.Handler");
        Strings unconfigured;
        handler.call("FindUnconfiguredLUKS").read(unconfigured);
        m_luks.insert(unconfigured.begin(), unconfigured.end());
        for (const std::string& device : m_luks)
            handler.call("SetDevicePassphrase", device, passphrase);
        storageObject("Bootloader").setProperty("Drive", std::string());
        validate(m_custom, done);
    }

    void validate(const Partitioning& partitioning, std::function<void(const Partitioning&, const ValidationReport&)> done)
    {
        const Proxy object(m_bus.get(), kStorage, partitioning.path, std::string(kStorage) + ".Partitioning");
        runTaskOf(object, "ConfigureWithTask", [this, object, partitioning, done](const std::string& error,
                                                                                const std::string&) {
            if (!error.empty()) {
                done(partitioning, errorReport(error));
                return;
            }
            runTaskOf(object, "ValidateWithTask", [this, partitioning, done](const std::string& error,
                                                                           const std::string& task) {
                if (!error.empty()) {
                    done(partitioning, errorReport(error));
                    return;
                }
                Variant result;
                Proxy(m_bus.get(), kStorage, task, kTask).call("GetResult").read(result);
                done(partitioning, reportFrom(result.get<VariantMap>().value_or(VariantMap{})));
            });
        });
    }

    std::vector<LayoutPart> readLayout(const std::string& tree) const
    {
        const Proxy view = viewer(tree);
        std::vector<LayoutPart> parts;
        StringMap mounts;
        view.call("GetMountPoints").read(mounts);
        std::set<std::string> volumes;
        for (const auto& [mountPoint, device] : mounts) {
            const VariantMap data = deviceData(view, device);
            uint64_t size = get<uint64_t>(data, "size");
            // Btrfs subvolumes share their volume: count it once.
            if (get<std::string>(data, "type") == "btrfs subvolume") {
                const Strings parents = get<Strings>(data, "parents");
                if (!parents.empty() && !volumes.insert(parents.front()).second)
                    size = 0;
            }
            parts.push_back({mountPoint, size, get<std::string>(formatData(view, device), "type")});
        }
        std::vector<VariantMap> actions;
        view.call("GetActions").read(actions);
        for (const VariantMap& action : actions) {
            if (get<std::string>(action, "action-type") != "create" || get<std::string>(action, "object-type") != "format")
                continue;
            const std::string device = get<std::string>(action, "device-name");
            if (get<std::string>(formatData(view, device), "type") == "swap")
                parts.push_back({"swap", get<uint64_t>(deviceData(view, device), "size"), "swap"});
        }
        auto rank = [](const LayoutPart& part) {
            static const Strings order{"/boot/efi", "/boot", "/", "/home"};
            const auto it = std::find(order.begin(), order.end(), part.mountPoint);
            return part.mountPoint == "swap" ? 9 : it == order.end() ? 5 : int(it - order.begin());
        };
        std::stable_sort(parts.begin(), parts.end(),
                         [&rank](const LayoutPart& a, const LayoutPart& b) { return rank(a) < rank(b); });
        return parts;
    }

    std::set<std::string> createdDevices(const std::string& tree) const
    {
        std::vector<VariantMap> actions;
        viewer(tree).call("GetActions").read(actions);
        std::set<std::string> created;
        for (const VariantMap& action : actions) {
            if (get<std::string>(action, "action-type") == "create" && get<std::string>(action, "object-type") == "device")
                created.insert(get<std::string>(action, "device-name"));
        }
        return created;
    }

    // The device holding the file system of a partition: the partition, or
    // the LUKS device opened from it.
    std::string leafOf(const std::string& tree, const std::string& device) const
    {
        const Proxy view = viewer(tree);
        if (get<std::string>(formatData(view, device), "type") != "luks")
            return device;
        const Strings children = get<Strings>(deviceData(view, device), "children");
        return children.size() == 1 ? children.front() : device;
    }

    bool hasFormat(const std::string& tree, const std::string& disk, const std::string& type) const
    {
        const Proxy view = viewer(tree);
        for (const std::string& child : get<Strings>(deviceData(view, disk), "children")) {
            if (get<std::string>(formatData(view, child), "type") == type)
                return true;
        }
        return false;
    }

    std::string validateMountPoint(const std::string& mountPoint) const
    {
        VariantMap report;
        scheduler().call("ValidateMountPoint", mountPoint).read(report);
        const Strings errors = get<Strings>(report, "error-messages");
        return errors.empty() ? std::string() : errors.front();
    }

    // Mount points below a device: the volumes of an LVM group or a Btrfs
    // volume on a partition.
    void collectMounts(const Proxy& view, const std::string& device, Strings& out, int depth = 0) const
    {
        if (depth > 4)
            return;
        const VariantMap format = formatData(view, device);
        const std::string type = get<std::string>(format, "type");
        const std::string mountPoint = valueOf(get<StringMap>(format, "attrs"), "mount-point");
        if (!mountPoint.empty())
            out.push_back(mountPoint);
        for (const std::string& child : get<Strings>(deviceData(view, device), "children"))
            collectMounts(view, child, out, depth + 1);
    }

    Disk readDisk(const std::string& tree, const std::string& name, const std::set<std::string>& created) const
    {
        const Proxy view = viewer(tree);
        const VariantMap data = deviceData(view, name);
        const StringMap attrs = get<StringMap>(data, "attrs");
        Disk disk;
        disk.id = name;
        disk.path = get<std::string>(data, "path");
        disk.size = get<uint64_t>(data, "size");
        disk.removable = get<bool>(data, "removable");
        disk.installationMedia = get<bool>(data, "protected");
        std::string model = valueOf(attrs, "vendor");
        const std::string product = valueOf(attrs, "model");
        if (!product.empty() && product.find(model) == std::string::npos)
            model = model.empty() ? product : model + " " + product;
        else if (!product.empty())
            model = product;
        std::replace(model.begin(), model.end(), '_', ' ');
        disk.model = model.empty() ? get<std::string>(data, "description") : model;
        const std::string bus = lower(valueOf(attrs, "bus"));
        if (name.starts_with("nvme"))
            disk.bus = "NVMe";
        else if (bus == "usb" || disk.removable)
            disk.bus = "USB";
        else if (name.starts_with("vd"))
            disk.bus = "VirtIO";
        else if (bus == "ata" || bus == "scsi" || name.starts_with("sd"))
            disk.bus = readFile("/sys/block/" + name + "/queue/rotational").starts_with("0") ? "SATA SSD" : "SATA HDD";
        else
            disk.bus = upper(bus);
        view.call("GetDiskFreeSpace", Strings{name}).read(disk.freeSpace);
        view.call("GetDiskReclaimableSpace", Strings{name}).read(disk.reclaimableSpace);
        if (const auto it = m_systems.find(name); it != m_systems.end())
            disk.systems = it->second;

        Strings children = get<Strings>(data, "children");
        std::stable_sort(children.begin(), children.end(), [](const std::string& a, const std::string& b) {
            return partitionNumber(a) < partitionNumber(b);
        });
        struct Piece
        {
            std::string name;
            VariantMap data;
            VariantMap format;
        };
        std::vector<Piece> pieces;
        uint64_t logical = 0;
        for (const std::string& child : children) {
            Piece piece{child, deviceData(view, child), formatData(view, child)};
            if (partitionNumber(child) >= 5)
                logical += get<uint64_t>(piece.data, "size");
            pieces.push_back(piece);
        }
        // A disk with a file system and no partition table is one piece.
        const VariantMap diskFormat = formatData(view, name);
        if (pieces.empty() && !get<std::string>(diskFormat, "type").empty()
            && get<std::string>(diskFormat, "type") != "disklabel")
            pieces.push_back({name, data, diskFormat});

        for (const Piece& piece : pieces) {
            const uint64_t size = get<uint64_t>(piece.data, "size");
            std::string type = get<std::string>(piece.format, "type");
            // The extended partition of an MBR disk spans its logical ones.
            if (type.empty() && logical && partitionNumber(piece.name) <= 4 && size >= logical)
                continue;
            DiskSegment segment;
            segment.device = piece.name;
            segment.size = size;
            segment.existing = !created.count(piece.name);
            // Where a partition on the disk starts (sysfs counts 512-byte
            // sectors); a planned one has no place until it is made.
            if (segment.existing && piece.name != name)
                segment.start = std::strtoull(readFile("/sys/class/block/" + piece.name + "/start").c_str(), nullptr, 10) * 512;
            VariantMap format = piece.format;
            if (type == "luks") {
                segment.encrypted = true;
                const Strings inner = get<Strings>(piece.data, "children");
                if (inner.size() == 1) {
                    format = formatData(view, inner.front());
                    type = get<std::string>(format, "type");
                }
            }
            const StringMap formatAttrs = get<StringMap>(format, "attrs");
            segment.format = type == "luks" ? "luks" : type;
            segment.label = valueOf(formatAttrs, "label");
            // Swap in use is swap made by the plan; old swap on the disk is not.
            segment.mountPoint = type == "swap" ? (segment.existing ? std::string() : "swap")
                                                : valueOf(formatAttrs, "mount-point");
            segment.description = get<std::string>(format, "description");
            if (type == "lvmpv" || type == "btrfs") {
                Strings mounts;
                collectMounts(view, piece.name, mounts);
                std::string joined;
                for (const std::string& mount : mounts)
                    joined += (joined.empty() ? "" : ", ") + mount;
                segment.mountPoint = joined;
            }
            if (type == "ntfs" && std::find(disk.systems.begin(), disk.systems.end(), "Windows") == disk.systems.end()
                && size > 20 * 1000 * MiB) {
                disk.systems.insert(disk.systems.begin(), "Windows");
                segment.description = "Windows";
            }
            if (get<bool>(piece.data, "protected"))
                disk.installationMedia = true;
            disk.segments.push_back(segment);
        }
        placeSegments(disk);
        return disk;
    }

    // Lays the pieces of a disk out in order, as they are on it: the free
    // space between them where it is. Planned partitions go to the first
    // space they fit, as blivet puts them.
    static void placeSegments(Disk& disk)
    {
        std::vector<DiskSegment> placed;
        std::vector<DiskSegment> planned;
        for (DiskSegment& segment : disk.segments)
            (segment.existing ? placed : planned).push_back(segment);
        std::stable_sort(placed.begin(), placed.end(),
                         [](const DiskSegment& a, const DiskSegment& b) { return a.start < b.start; });
        // The spaces: before, between and after the partitions there.
        struct Gap
        {
            uint64_t start, end;
        };
        std::vector<Gap> gaps;
        uint64_t cursor = 0;
        for (const DiskSegment& segment : placed) {
            if (segment.start > cursor)
                gaps.push_back({cursor, segment.start});
            cursor = std::max(cursor, segment.start + segment.size);
        }
        if (disk.size > cursor)
            gaps.push_back({cursor, disk.size});
        for (DiskSegment& segment : planned) {
            for (Gap& gap : gaps) {
                if (gap.end - gap.start >= segment.size) {
                    segment.start = gap.start;
                    gap.start += segment.size;
                    break;
                }
            }
            placed.push_back(segment);
        }
        // Spaces too small to use (alignment, the GPT's backup, what is left
        // after planned partitions) are left out.
        for (const Gap& gap : gaps) {
            if (gap.end - gap.start >= 16 * MiB)
                placed.push_back({.start = gap.start, .size = gap.end - gap.start, .free = true, .existing = false});
        }
        std::stable_sort(placed.begin(), placed.end(),
                         [](const DiskSegment& a, const DiskSegment& b) { return a.start < b.start; });
        disk.segments = std::move(placed);
    }

    // Installation.

    void report(double fraction, const std::string& status)
    {
        m_fraction = fraction;
        InstallProgress progress;
        progress.fraction = fraction;
        progress.status = status;
        const double elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - m_started).count();
        if (fraction > 0.05)
            progress.remainingSeconds = int(elapsed * (1 - fraction) / fraction);
        if (m_progress)
            m_progress(progress);
    }

    // Copying the system takes most of the time, though it is one of
    // Anaconda's steps: it gets 5–70 %, the steps before and after the rest
    // up to 90 % (the post-install has the last 10).
    double installFraction(int step, const std::string& message)
    {
        if (m_steps <= 0)
            return m_fraction;
        double percent = -1;
        if (message.starts_with("Installing software")) {
            if (m_softwareStep < 0)
                m_softwareStep = step;
            percent = std::atof(message.c_str() + message.find_last_of(' ') + 1);
        }
        double fraction;
        if (m_softwareStep < 0)
            fraction = 0.05 * std::min(1.0, double(step) / std::max(1, m_steps / 4));
        else if (percent >= 0)
            fraction = 0.05 + 0.65 * percent / 100;
        else
            fraction = 0.7 + 0.2 * double(step - m_softwareStep) / std::max(1, m_steps - m_softwareStep);
        return std::clamp(std::max(fraction, m_fraction), 0.0, 0.9);
    }

    // The step running is done once the next one starts. Steps that report
    // a percentage ("Installing software 42%") are one line.
    void logStep(const std::string& reported)
    {
        std::string message = reported;
        if (!message.empty() && message.back() == '%') {
            const size_t space = message.find_last_of(' ');
            if (space != std::string::npos)
                message.resize(space);
        }
        if (message.empty() || message == m_running)
            return;
        if (!m_running.empty())
            m_log({InstallLogLine::State::Done, m_running});
        m_running = message;
        m_log({InstallLogLine::State::Running, message});
    }

    void postInstall()
    {
        logStep(tr("Finishing the installed system"));
        report(0.9, tr("Finishing the installed system"));
        m_postDone = true;
        auto set = [this](const std::string& key, const std::string& value) {
            m_helper.writeStandardInput("set " + key + " " + value + "\n");
        };
        set("user", m_account.userName);
        set("autologin", m_account.requirePassword ? "0" : "1");
        set("picture", m_account.picture);
        set("autoconnect", m_autoconnect ? "1" : "0");
        std::string groups;
        for (const std::string& addon : m_addons)
            groups += (groups.empty() ? "" : " ") + addon;
        set("groups", isOnline() ? groups : std::string());
        m_helper.writeStandardInput("postinstall\n");
    }

    void finishPostInstall(int code)
    {
        m_postDone = false;
        // ponytail: a failed post-install step leaves a working system (the
        // live account or an add-on may remain), so it warns but succeeds.
        if (code != 0)
            m_log({InstallLogLine::State::Failed,
                   tr("Finishing the system did not complete; see /tmp/nabau-post.log")});
        finish(true, {});
    }

    void finish(bool succeeded, const std::string& error)
    {
        if (!m_running.empty())
            m_log({succeeded ? InstallLogLine::State::Done : InstallLogLine::State::Failed, m_running});
        m_running.clear();
        if (succeeded)
            report(1, tr("Complete"));
        InstallResult result;
        result.succeeded = succeeded;
        result.error = error;
        result.seconds =
            int(std::chrono::duration<double>(std::chrono::steady_clock::now() - m_started).count());
        result.packages = m_packageCount;
        result.encrypted = m_encrypted || m_encryptedMounts;
        if (m_done)
            m_done(result);
    }

    std::unique_ptr<Connection> m_bus;
    bayu::base::Process m_helper;
    std::string m_buffer;
    std::string m_startError;
    std::function<void(const std::string&)> m_status;
    std::function<void(const std::string&)> m_ready;
    NetworkManager m_network;
    std::set<std::shared_ptr<TaskRun>> m_tasks;

    SystemInfo m_info;
    std::vector<Language> m_languages;
    std::string m_locale;
    std::set<std::string> m_installedLocales;
    std::vector<XkbLayout> m_xkb;
    std::string m_layout;
    std::vector<std::pair<std::string, std::vector<std::string>>> m_zones;
    TimeSettings m_time;
    std::string m_hostname;
    bool m_autoconnect = true;

    std::vector<Disk> m_disks;
    Strings m_usable;
    std::map<std::string, Strings> m_systems;
    Strings m_fileSystems;
    int32_t m_scheme = kLvm;
    uint64_t m_required = 0;
    Partitioning m_automatic;
    std::deque<std::function<void(std::function<void()>)>> m_storageQueue;
    bool m_storageBusy = false;
    mutable Partitioning m_probe;
    Partitioning m_custom;
    Strings m_customDisks;
    std::set<std::string> m_luks;
    std::vector<LayoutPart> m_applied;
    bool m_encrypted = false;
    bool m_encryptedMounts = false;
    // createRecommended makes an EFI partition even beside an existing one.
    bool m_newEsp = false;
    // Planning checks an encrypted layout with this until the account step
    // gives the real passphrase.
    const std::string m_placeholder = "nabau-plan";

    std::vector<std::string> m_addons;
    int m_packageCount = 0;
    Account m_account;

    std::function<void(const InstallProgress&)> m_progress;
    std::function<void(const InstallLogLine&)> m_log;
    std::function<void(const InstallResult&)> m_done;
    std::chrono::steady_clock::time_point m_started;
    int32_t m_steps = 0;
    double m_fraction = 0;
    int m_softwareStep = -1;
    std::string m_running;
    bool m_postDone = false;
    bool m_checkingMedia = false;
};

} // namespace

std::unique_ptr<Backend> createAnacondaBackend()
{
    return std::make_unique<AnacondaBackend>();
}

} // namespace installer
