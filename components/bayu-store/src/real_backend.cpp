// SPDX-License-Identifier: GPL-3.0-or-later
// The store's backend on a real system: the catalog from Flathub's API and
// F-Droid's index (kept in ~/.cache/bayu-store), Flatpak to install Flathub
// apps and lapis for F-Droid's Android apps.

#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <deque>
#include <filesystem>
#include <future>
#include <fstream>
#include <map>
#include <set>
#include <thread>

#include "android_setup.h"
#include "backend.h"
#include "base/event_loop.h"
#include "base/hash.h"
#include "base/json.h"
#include "base/paths.h"
#include "base/process.h"
#include "base/settings.h"
#include "base/thread_pool.h"
#include "base/timer.h"
#include "cache.h"
#include "catalog.h"
#include "fdroid.h"
#include "flathub.h"
#include "igos.h"
#include "flatpak_client.h"
#include "lapis.h"
#include "net.h"

namespace store {

namespace {

using bayu::base::LoopHandle;
using bayu::base::ThreadPool;

constexpr int64_t kSyncAge = 6 * 3600;
constexpr int kIconDownloads = 6;
constexpr const char* kAndroidLaunch = "/usr/libexec/bayu-store/android-launch";

int64_t now()
{
    return int64_t(std::time(nullptr));
}

int64_t fileAge(const std::string& path)
{
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0 ? now() - int64_t(st.st_mtime) : INT64_MAX;
}

bool exists(const std::string& path)
{
    return ::access(path.c_str(), F_OK) == 0;
}

std::string sha256File(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    bayu::base::CryptographicHash hash(bayu::base::CryptographicHash::Algorithm::Sha256);
    char buffer[1 << 16];
    while (in.read(buffer, sizeof(buffer)) || in.gcount() > 0)
        hash.addData(buffer, size_t(in.gcount()));
    const std::string raw = hash.result();
    static const char* digits = "0123456789abcdef";
    std::string hex;
    for (unsigned char c : raw) {
        hex += digits[c >> 4];
        hex += digits[c & 15];
    }
    return hex;
}

uint64_t directorySize(const std::string& path)
{
    uint64_t total = 0;
    std::error_code error;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(path, error)) {
        if (entry.is_regular_file(error))
            total += entry.file_size(error);
    }
    return total;
}

// The F-Droid texts' language: the session's, as F-Droid names it.
std::string fdroidLocale()
{
    const char* lang = std::getenv("LC_ALL");
    if (!lang || !*lang)
        lang = std::getenv("LANG");
    std::string language = lang && *lang ? lang : "en_US";
    // "id_ID.UTF-8" -> "id"; English keeps its region: "en-US".
    language = language.substr(0, language.find('.'));
    if (language.starts_with("en"))
        return "en-US";
    return language.substr(0, language.find('_'));
}

class RealBackend : public Backend
{
public:
    RealBackend()
        : m_settings(bayu::base::paths::configHome() + "/bayu-store.conf")
        , m_pool(8)
        , m_loop(bayu::base::EventLoop::current()->handle())
    {
        m_enabled[0] = m_settings.boolValue("Sources/flathub", true);
        m_enabled[1] = m_settings.boolValue("Sources/fdroid", true);
        m_automatic = m_settings.boolValue("Updates/automatic", true);
        m_armOnly = m_settings.boolValue("Android/armOnly", false);
        m_lastChecked = m_settings.intValue("Updates/lastChecked", 0);
        for (const std::string& key : m_settings.stringListValue("Library/favorites", {}))
            m_favorites.insert(key);
        // What Android had last time: a frozen Android cannot be asked.
        for (const std::string& entry : m_settings.stringListValue("Android/packages", {})) {
            const size_t colon = entry.rfind(':');
            if (colon != std::string::npos)
                m_androidPackages[entry.substr(0, colon)] = std::atoll(entry.c_str() + colon + 1);
        }
        m_poll.onTimeout = [this] { refreshLibrary(); };
        m_setup.onChanged = [this] { changed(); };
        m_setup.onFinished = [this](const std::string& error) {
            std::optional<App> then = std::move(m_afterSetup);
            m_afterSetup.reset();
            if (error.empty()) {
                // Android runs: no need to wait for the next refresh.
                m_android = lapis::State::Running;
                if (then)
                    install(*then);
            }
            refreshLibrary();
        };
    }

    ~RealBackend() override
    {
        for (auto& job : m_queue)
            job->cancel = true;
        if (m_worker.joinable())
            m_worker.join();
    }

    void start() override
    {
        m_flathubFile = cache::directory() + "/flathub.jsonl";
        m_fdroidFile = cache::directory() + "/fdroid.jsonl";
        m_igosFile = cache::directory() + "/igos.jsonl";
        m_loading = !exists(m_flathubFile) && !exists(m_fdroidFile);
        // The catalog on disk first: the store opens at once, offline too.
        async(
            [this] {
                auto apps = std::make_shared<std::pair<std::vector<App>, std::vector<App>>>();
                apps->first = cache::load(m_flathubFile);
                // IGOS Nusantara's Flatpak apps sit with Flathub's.
                for (App& app : cache::load(m_igosFile))
                    apps->first.push_back(std::move(app));
                apps->second = cache::load(m_fdroidFile);
                return apps;
            },
            [this](std::shared_ptr<std::pair<std::vector<App>, std::vector<App>>> apps) {
                m_catalog.setApps(Source::Flathub, std::move(apps->first));
                m_catalog.setApps(Source::FDroid, std::move(apps->second));
                m_loading = m_catalog.apps().empty();
                changed();
                if (m_loading || fileAge(m_flathubFile) > kSyncAge || fileAge(m_fdroidFile) > kSyncAge)
                    sync();
            });
        // The updates the panel and Settings told about, at once: with a
        // fresh catalogue no sync comes to check them (Check found them).
        checkUpdates(false);
        m_poll.start(std::chrono::seconds(60));
    }
    bool isLoading() const override { return m_loading; }
    std::string syncError() const override { return m_syncError; }

    AndroidState androidState() const override
    {
        return m_android == lapis::State::Missing ? AndroidState::Missing
               : m_android == lapis::State::Stopped ? AndroidState::Off
                                                    : AndroidState::Running;
    }

    void enableAndroid(std::optional<App> then) override
    {
        if (then)
            m_afterSetup = then;
        if (!m_setup.isRunning())
            m_setup.start();
        changed();
    }

    AndroidSetup androidSetup() const override
    {
        AndroidSetup setup = m_setup.state();
        if (m_afterSetup)
            setup.pendingApp = m_afterSetup->name.empty() ? m_afterSetup->id : m_afterSetup->name;
        return setup;
    }

    void disableAndroid() override
    {
        async([] { return lapis::stopSession(); },
              [this](std::string error) {
                  if (!error.empty())
                      m_error = error;
                  refreshLibrary();
              });
    }

    int count(Source source) const override { return m_catalog.count(source); }
    std::vector<Category> categories() const override { return m_catalog.categories(); }

    std::vector<App> featured() const override
    {
        std::vector<App> out;
        for (const std::string& id : m_featured) {
            if (const App* app = m_catalog.find(id, Source::Flathub))
                out.push_back(withLocal(*app));
        }
        if (out.empty()) {
            Query query;
            query.sources = {Source::Flathub};
            query.limit = 3;
            out = query_(query).apps;
        }
        return out;
    }

    Page query(const Query& query) const override { return query_(query); }

    std::optional<App> app(const std::string& id, Source source) const override
    {
        const App* app = m_catalog.find(id, source);
        if (!app)
            return std::nullopt;
        App local = withLocal(*app);
        for (Screenshot& shot : local.screenshots) {
            const std::string path = cache::fileFor(shot.url, "screenshots");
            if (exists(path)) {
                shot.url = path;
            } else {
                fetchMedia(shot.url, path);
                shot.url.clear();
            }
        }
        return local;
    }

    void details(const App& app, std::function<void(const App&)> done) override
    {
        const auto key = std::make_pair(app.id, int(app.source));
        if (app.source != Source::Flathub || app.remote == igos::kRemote || m_detailed.count(key))
            return;
        m_detailed.insert(key);
        App copy = app;
        async(
            [copy]() mutable {
                flathub::fetchDetails(copy);
                return copy;
            },
            [this, done](App detailed) {
                if (App* app = m_catalog.find(detailed.id, Source::Flathub)) {
                    detailed.iconPath.clear();
                    *app = detailed;
                }
                done(detailed);
                changed();
            });
    }

    std::vector<std::string> relatedSearches(const std::string& text) const override
    {
        return m_catalog.relatedSearches(text);
    }
    std::vector<std::pair<std::string, int>> topDevelopers(const Query& query) const override
    {
        return m_catalog.topDevelopers(query);
    }

    std::vector<InstalledApp> installed() const override
    {
        std::vector<InstalledApp> out;
        for (const auto& app : m_flatpaks)
            out.push_back(withUpdate(app));
        for (const auto& [package, code] : m_androidPackages) {
            const App* app = m_catalog.find(package, Source::FDroid);
            // Only apps the store knows: not Android's own or Play's.
            if (!app)
                continue;
            InstalledApp installed;
            installed.id = package;
            installed.source = Source::FDroid;
            installed.version = code == app->versionCode ? app->version : std::to_string(code);
            installed.size = app->downloadSize;
            installed.installed = m_settings.intValue("Installed/" + package, 0);
            if (code < app->versionCode) {
                installed.newVersion = app->version;
                installed.updateSize = app->downloadSize;
            }
            out.push_back(installed);
        }
        return out;
    }

    std::optional<InstalledApp> installedApp(const std::string& id, Source source) const override
    {
        if (source == Source::Flathub) {
            for (const auto& app : m_flatpaks) {
                if (app.id == id)
                    return withUpdate(app);
            }
            return std::nullopt;
        }
        for (const InstalledApp& app : installed()) {
            if (app.id == id && app.source == source)
                return app;
        }
        return std::nullopt;
    }

    std::vector<InstalledApp> updates() const override
    {
        std::vector<InstalledApp> out;
        for (const InstalledApp& app : installed()) {
            if (!app.newVersion.empty())
                out.push_back(app);
        }
        return out;
    }

    std::vector<InstalledApp> recent() const override
    {
        std::vector<InstalledApp> out = installed();
        std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.installed > b.installed; });
        return out;
    }

    // The Check button: it only looks for updates (issue #154); installing
    // them by itself is for the check after each catalogue sync.
    void checkUpdates() override { checkUpdates(false); }
    void checkUpdates(bool install)
    {
        async(
            [] {
                std::string error;
                auto ids = flatpak::updates(&error);
                std::set<std::string> out;
                for (const auto& update : ids)
                    out.insert(update.id);
                return out;
            },
            [this, install](std::set<std::string> ids) {
                m_flatpakUpdates = std::move(ids);
                m_lastChecked = now();
                m_settings.setValue("Updates/lastChecked", m_lastChecked);
                m_settings.sync();
                changed();
                if (install && m_automatic)
                    updateAll();
            });
        refreshLibrary();
    }
    int64_t lastChecked() const override { return m_lastChecked; }

    StorageUsage storage() const override
    {
        StorageUsage usage = m_usage;
        struct statvfs fs{};
        if (::statvfs("/var/lib/flatpak", &fs) == 0 || ::statvfs("/", &fs) == 0) {
            usage.free = uint64_t(fs.f_bavail) * fs.f_frsize;
            usage.total = uint64_t(fs.f_blocks) * fs.f_frsize;
        }
        usage.cache = m_cacheSize;
        return usage;
    }

    std::vector<Job> jobs() const override
    {
        std::vector<Job> out;
        for (const auto& job : m_queue)
            out.push_back(job->job);
        return out;
    }

    std::optional<Job> job(const std::string& id, Source source) const override
    {
        for (const auto& job : m_queue) {
            if (job->job.id == id && job->job.source == source)
                return job->job;
        }
        return std::nullopt;
    }

    void install(const App& app) override { enqueue(app.id, app.source, Job::Kind::Install); }
    void update(const std::string& id, Source source) override { enqueue(id, source, Job::Kind::Update); }
    void updateAll() override
    {
        for (const InstalledApp& app : updates())
            enqueue(app.id, app.source, Job::Kind::Update);
    }
    void remove(const std::string& id, Source source) override { enqueue(id, source, Job::Kind::Remove); }

    void pause(const std::string& id, Source source) override
    {
        if (auto* job = find(id, source)) {
            job->job.paused = true;
            job->cancel = true;
            changed();
        }
    }
    void resume(const std::string& id, Source source) override
    {
        if (auto* job = find(id, source)) {
            job->job.paused = false;
            changed();
            next();
        }
    }
    void cancel(const std::string& id, Source source) override
    {
        if (auto* job = find(id, source)) {
            job->cancel = true;
            job->dropped = true;
            if (!job->running)
                std::erase_if(m_queue, [job](const auto& each) { return each.get() == job; });
            changed();
        }
    }

    void launch(const std::string& id, Source source) override
    {
        if (source == Source::FDroid) {
            // Shows that Android starts, when it does not run.
            bayu::base::Process::startDetached(kAndroidLaunch, {id});
            return;
        }
        async([id] { return flatpak::launch(id); },
              [this](std::string error) {
                  if (!error.empty()) {
                      m_error = error;
                      changed();
                  }
              });
    }

    std::string takeError() override { return std::exchange(m_error, {}); }

    bool sourceEnabled(Source source) const override { return m_enabled[int(source)]; }
    void setSourceEnabled(Source source, bool enabled) override
    {
        m_enabled[int(source)] = enabled;
        m_settings.setValue(source == Source::Flathub ? "Sources/flathub" : "Sources/fdroid", enabled);
        m_settings.sync();
        changed();
    }
    bool automaticUpdates() const override { return m_automatic; }
    void setAutomaticUpdates(bool automatic) override
    {
        m_automatic = automatic;
        m_settings.setValue("Updates/automatic", automatic);
        m_settings.sync();
        changed();
    }
    bool showArmOnly() const override { return m_armOnly; }
    void setShowArmOnly(bool show) override
    {
        m_armOnly = show;
        m_settings.setValue("Android/armOnly", show);
        m_settings.sync();
        changed();
    }
    uint64_t cacheSize() const override { return m_cacheSize; }
    void clearCache() override
    {
        std::error_code error;
        for (const char* folder : {"icons", "screenshots", "apk"})
            std::filesystem::remove_all(cache::directory() + "/" + folder, error);
        m_iconsWanted.clear();
        m_cacheSize = directorySize(cache::directory());
        changed();
    }
    int64_t lastSynced(Source source) const override
    {
        const int64_t age = fileAge(source == Source::Flathub ? m_flathubFile : m_fdroidFile);
        return age == INT64_MAX ? 0 : now() - age;
    }

    void sync() override
    {
        if (m_syncing)
            return;
        m_syncing = true;
        m_syncError.clear();
        const std::string flathubFile = m_flathubFile;
        const std::string fdroidFile = m_fdroidFile;
        const std::string igosFile = m_igosFile;
        const std::string locale = fdroidLocale();
        struct Result
        {
            std::vector<App> flathub;
            std::vector<App> fdroid;
            std::vector<std::string> featured;
            std::string error;
        };
        async(
            [flathubFile, fdroidFile, igosFile, locale] {
                auto result = std::make_shared<Result>();
                std::string error;
                auto featured = std::async(std::launch::async, [] { return flathub::fetchFeatured(); });
                result->flathub = flathub::fetchCatalog(&error);
                if (!error.empty())
                    result->error = error;
                // F-Droid: entry.json names the index and its checksum; the
                // index (gzipped on the way) only when that changed.
                const std::string shaFile = cache::directory() + "/index-v2.sha256";
                std::string known;
                std::getline(std::ifstream(shaFile), known);
                const net::Response entry = net::get(std::string(fdroid::kRepo) + "/entry.json", 60);
                const std::optional<bayu::base::json::Value> entryJson =
                    entry.ok() ? bayu::base::json::Value::parse(entry.body) : std::nullopt;
                const std::string sha = entryJson ? (*entryJson)["index"]["sha256"].toString() : std::string();
                if (sha.empty()) {
                    result->error = "F-Droid: " + (entry.error.empty() ? std::string("no index") : entry.error);
                } else if (sha != known || ::access(fdroidFile.c_str(), F_OK) != 0) {
                    const net::Response index = net::get(std::string(fdroid::kRepo) + "/index-v2.json", 3600);
                    if (!index.ok()) {
                        result->error = "F-Droid: " + index.error;
                    } else if (bayu::base::CryptographicHash::hashHex(index.body,
                                                                      bayu::base::CryptographicHash::Algorithm::Sha256)
                               != sha) {
                        result->error = "F-Droid: the index did not match its checksum.";
                    } else {
                        std::string failure;
                        result->fdroid = fdroid::parseIndex(index.body, locale, &failure);
                        if (!result->fdroid.empty())
                            std::ofstream(shaFile) << sha << "\n";
                        else
                            result->error = "F-Droid: " + failure;
                    }
                }
                if (!result->flathub.empty())
                    cache::save(flathubFile, result->flathub);
                // IGOS Nusantara's apps join Flathub's, each from its cache
                // when its own fetch failed.
                std::string igosError;
                std::vector<App> igosApps = igos::fetchCatalog(&igosError);
                if (!igosApps.empty())
                    cache::save(igosFile, igosApps);
                else if (result->error.empty())
                    result->error = igosError;
                if (!result->flathub.empty() || !igosApps.empty()) {
                    if (result->flathub.empty())
                        result->flathub = cache::load(flathubFile);
                    if (igosApps.empty())
                        igosApps = cache::load(igosFile);
                    for (App& app : igosApps)
                        result->flathub.push_back(std::move(app));
                }
                if (!result->fdroid.empty())
                    cache::save(fdroidFile, result->fdroid);
                result->featured = featured.get();
                return result;
            },
            [this](std::shared_ptr<Result> result) {
                m_syncing = false;
                if (!result->flathub.empty()) {
                    m_catalog.setApps(Source::Flathub, std::move(result->flathub));
                    m_detailed.clear();
                }
                if (!result->fdroid.empty())
                    m_catalog.setApps(Source::FDroid, std::move(result->fdroid));
                if (!result->featured.empty())
                    m_featured = result->featured;
                m_syncError = result->error;
                if (!m_syncError.empty())
                    fprintf(stderr, "bayu-store: %s\n", m_syncError.c_str());
                m_loading = false;
                changed();
                checkUpdates(true);
            });
    }

    bool isFavorite(const std::string& id, Source source) const override
    {
        return m_favorites.count(key(id, source));
    }
    void setFavorite(const std::string& id, Source source, bool favorite) override
    {
        if (favorite)
            m_favorites.insert(key(id, source));
        else
            m_favorites.erase(key(id, source));
        m_settings.setValue("Library/favorites", std::vector<std::string>(m_favorites.begin(), m_favorites.end()));
        m_settings.sync();
        changed();
    }

private:
    struct Work
    {
        Job job;
        std::atomic<bool> cancel = false;
        bool running = false;
        bool dropped = false;
    };

    static std::string key(const std::string& id, Source source)
    {
        return (source == Source::Flathub ? "flathub:" : "fdroid:") + id;
    }

    template<typename WorkFn, typename Done>
    void async(WorkFn work, Done done) const
    {
        m_pool.run(std::move(work), std::move(done)).detach();
    }

    void changed()
    {
        if (onChanged)
            onChanged();
    }

    // The catalog's answer with the icons this computer has, fetching the rest.
    Page query_(const Query& query) const
    {
        Query filtered = query;
        if (filtered.sources.empty()) {
            if (!m_enabled[0])
                filtered.sources = {Source::FDroid};
            else if (!m_enabled[1])
                filtered.sources = {Source::Flathub};
        }
        Page page = m_catalog.query(filtered);
        if (!m_armOnly)
            std::erase_if(page.apps, [](const App& app) { return app.armOnly; });
        for (App& app : page.apps)
            app = withLocal(app);
        return page;
    }

    App withLocal(const App& app) const
    {
        App out = app;
        if (out.iconUrl.empty())
            return out;
        const std::string path = cache::fileFor(out.iconUrl, "icons");
        if (exists(path))
            out.iconPath = path;
        else
            fetchMedia(out.iconUrl, path);
        return out;
    }

    // Icons and screenshots, a few at a time; the page is rebuilt as they come.
    void fetchMedia(const std::string& url, const std::string& path) const
    {
        if (url.empty() || !m_iconsWanted.insert(path).second)
            return;
        m_mediaQueue.push_back({url, path});
        pumpMedia();
    }

    void pumpMedia() const
    {
        while (m_mediaRunning < kIconDownloads && !m_mediaQueue.empty()) {
            const auto [url, path] = m_mediaQueue.front();
            m_mediaQueue.pop_front();
            ++m_mediaRunning;
            auto* self = const_cast<RealBackend*>(this);
            async(
                [url, path] {
                    const std::string part = path + ".part";
                    const std::string error =
                        net::download(url, part, [](uint64_t, uint64_t, uint64_t) { return true; });
                    if (error.empty())
                        std::rename(part.c_str(), path.c_str());
                    else
                        ::unlink(part.c_str());
                    return error.empty();
                },
                [self](bool ok) {
                    --self->m_mediaRunning;
                    if (ok)
                        self->changed();
                    self->pumpMedia();
                });
        }
    }

    InstalledApp withUpdate(const flatpak::Installed& app) const
    {
        InstalledApp out;
        out.id = app.id;
        out.source = Source::Flathub;
        out.version = app.version;
        out.size = app.size;
        out.installed = app.installed;
        if (m_flatpakUpdates.count(app.id)) {
            const App* catalog = m_catalog.find(app.id, Source::Flathub);
            out.newVersion = catalog && !catalog->version.empty() && catalog->version != app.version
                                 ? catalog->version
                                 : std::string("new");
            out.updateSize = catalog ? catalog->downloadSize : 0;
        }
        return out;
    }

    // What is installed, from Flatpak and lapis.
    void refreshLibrary()
    {
        struct Library
        {
            std::vector<flatpak::Installed> flatpaks;
            std::map<std::string, int64_t> packages;
            lapis::State android = lapis::State::Missing;
            flatpak::Usage usage;
            uint64_t cache = 0;
        };
        async(
            [] {
                auto library = std::make_shared<Library>();
                std::string error;
                library->flatpaks = flatpak::installed(&error);
                library->android = lapis::state();
                if (library->android == lapis::State::Running)
                    library->packages = lapis::packages(&error);
                library->usage = flatpak::usage();
                library->cache = directorySize(cache::directory());
                return library;
            },
            [this](std::shared_ptr<Library> library) {
                m_flatpaks = std::move(library->flatpaks);
                // A frozen Android cannot be asked: keep what it had.
                if (library->android == lapis::State::Running || library->android == lapis::State::Missing) {
                    m_androidPackages = std::move(library->packages);
                    std::vector<std::string> entries;
                    for (const auto& [package, code] : m_androidPackages)
                        entries.push_back(package + ":" + std::to_string(code));
                    m_settings.setValue("Android/packages", entries);
                    m_settings.sync();
                }
                m_android = library->android;
                m_usage.apps = library->usage.apps;
                m_usage.runtimes = library->usage.runtimes;
                m_cacheSize = library->cache;
                changed();
            });
    }

    Work* find(const std::string& id, Source source)
    {
        for (auto& job : m_queue) {
            if (job->job.id == id && job->job.source == source)
                return job.get();
        }
        return nullptr;
    }

    void enqueue(const std::string& id, Source source, Job::Kind kind)
    {
        if (Work* existing = find(id, source)) {
            existing->job.paused = false;
            next();
            return;
        }
        auto work = std::make_unique<Work>();
        work->job.id = id;
        work->job.source = source;
        work->job.kind = kind;
        if (const App* app = m_catalog.find(id, source))
            work->job.bytesTotal = app->downloadSize;
        m_queue.push_back(std::move(work));
        changed();
        next();
    }

    // Starts the first job that waits, when none runs.
    void next()
    {
        for (const auto& work : m_queue) {
            if (work->running)
                return;
        }
        if (m_worker.joinable())
            m_worker.join();
        Work* work = nullptr;
        for (const auto& each : m_queue) {
            if (!each->job.paused && !each->dropped) {
                work = each.get();
                break;
            }
        }
        if (!work)
            return;
        work->running = true;
        work->cancel = false;
        const App* found = m_catalog.find(work->job.id, work->job.source);
        const App app = found ? *found : App{.id = work->job.id, .source = work->job.source};
        LoopHandle loop = m_loop;
        m_worker = std::thread([this, work, app, loop] {
            auto progress = [work, loop](uint64_t done, uint64_t total, double fraction, uint64_t speed) {
                loop.post([work, done, total, fraction, speed] {
                    work->job.bytesDone = done;
                    if (total)
                        work->job.bytesTotal = total;
                    work->job.progress = fraction;
                    work->job.bytesPerSecond = speed;
                });
                return !work->cancel;
            };
            const std::string error = perform(*work, app, progress);
            loop.post([this, work, error] { finished(work, error); });
        });
        changed();
        m_progressTimer.onTimeout = [this] { changed(); };
        m_progressTimer.start(std::chrono::milliseconds(400));
    }

    // On the worker thread.
    std::string perform(Work& work, const App& app,
                        const std::function<bool(uint64_t, uint64_t, double, uint64_t)>& progress)
    {
        if (app.source == Source::Flathub) {
            const flatpak::Operation operation = work.job.kind == Job::Kind::Install ? flatpak::Operation::Install
                                                 : work.job.kind == Job::Kind::Update ? flatpak::Operation::Update
                                                                                       : flatpak::Operation::Remove;
            auto started = std::chrono::steady_clock::now();
            return flatpak::run(
                operation, app.id, app.remote,
                [&](uint64_t done, uint64_t, int percent) {
                    const double seconds =
                        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
                    progress(done, 0, percent / 100.0, seconds > 0 ? uint64_t(double(done) / seconds) : 0);
                },
                work.cancel);
        }
        // Android apps.
        if (lapis::state() == lapis::State::Frozen)
            lapis::unfreeze();
        if (lapis::state() == lapis::State::Stopped) {
            Work* waiting = &work;
            m_loop.post([this, waiting] {
                waiting->job.startingAndroid = true;
                changed();
            });
            const std::string error = lapis::startSession(std::chrono::minutes(3));
            m_loop.post([this, waiting] {
                waiting->job.startingAndroid = false;
                changed();
            });
            if (!error.empty())
                return error;
        }
        if (work.job.kind == Job::Kind::Remove)
            return lapis::uninstall(app.id);
        if (app.apkUrl.empty())
            return "This app has no download.";
        // Kept in the user's cache: the control service has a private /tmp.
        const std::string apk = cache::directory() + "/apk";
        ::mkdir(apk.c_str(), 0700);
        const std::string path = apk + "/" + app.id + "_" + std::to_string(app.versionCode) + ".apk";
        if (!exists(path) || sha256File(path) != app.apkSha256) {
            const std::string part = path + ".part";
            const std::string error = net::download(app.apkUrl, part, [&](uint64_t done, uint64_t total, uint64_t speed) {
                return progress(done, total, total ? 0.9 * double(done) / double(total) : -1, speed);
            });
            if (!error.empty())
                return error;
            std::rename(part.c_str(), path.c_str());
            if (sha256File(path) != app.apkSha256) {
                ::unlink(path.c_str());
                return "The download is damaged (its checksum does not match F-Droid's). Try again.";
            }
        }
        progress(app.downloadSize, app.downloadSize, 0.95, 0);
        const std::string error = lapis::install(path);
        if (error.empty())
            ::unlink(path.c_str());
        return error;
    }

    void finished(Work* work, const std::string& error)
    {
        if (m_worker.joinable())
            m_worker.join();
        m_progressTimer.stop();
        work->running = false;
        const bool stopped = error == "stopped";
        if (!error.empty() && !stopped)
            m_error = error;
        if (error.empty() && work->job.kind != Job::Kind::Remove && work->job.source == Source::FDroid) {
            m_settings.setValue("Installed/" + work->job.id, now());
            m_settings.sync();
        }
        if (error.empty() && work->job.source == Source::Flathub)
            m_flatpakUpdates.erase(work->job.id);
        // Paused jobs wait in the list; the rest leave it.
        if (!stopped || work->dropped || !work->job.paused) {
            if (stopped && work->dropped && work->job.source == Source::FDroid) {
                std::error_code ignored;
                for (const auto& entry : std::filesystem::directory_iterator(cache::directory() + "/apk", ignored)) {
                    if (entry.path().filename().string().starts_with(work->job.id + "_"))
                        std::filesystem::remove(entry.path(), ignored);
                }
            }
            std::erase_if(m_queue, [work](const auto& each) { return each.get() == work; });
        }
        refreshLibrary();
        changed();
        next();
    }

    bayu::base::Settings m_settings;
    mutable ThreadPool m_pool;
    LoopHandle m_loop;
    Catalog m_catalog;
    std::string m_flathubFile;
    std::string m_fdroidFile;
    std::string m_igosFile;
    bool m_loading = true;
    bool m_syncing = false;
    std::string m_syncError;
    std::vector<std::string> m_featured;
    mutable std::set<std::pair<std::string, int>> m_detailed;

    mutable std::set<std::string> m_iconsWanted;
    mutable std::deque<std::pair<std::string, std::string>> m_mediaQueue;
    mutable int m_mediaRunning = 0;

    std::vector<flatpak::Installed> m_flatpaks;
    std::set<std::string> m_flatpakUpdates;
    std::map<std::string, int64_t> m_androidPackages;
    lapis::State m_android = lapis::State::Missing;
    AndroidSetupRun m_setup;
    std::optional<App> m_afterSetup;
    StorageUsage m_usage;
    uint64_t m_cacheSize = 0;
    int64_t m_lastChecked = 0;

    std::deque<std::unique_ptr<Work>> m_queue;
    std::thread m_worker;
    bayu::base::Timer m_progressTimer;
    bayu::base::Timer m_poll;

    bool m_enabled[2] = {true, true};
    bool m_automatic = true;
    bool m_armOnly = false;
    std::set<std::string> m_favorites;
    std::string m_error;
};

} // namespace

std::unique_ptr<Backend> createBackend()
{
    return std::make_unique<RealBackend>();
}

} // namespace store
