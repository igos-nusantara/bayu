// SPDX-License-Identifier: GPL-3.0-or-later

#include "cache.h"

#include <sys/stat.h>

#include <cstdio>
#include <fstream>

#include "base/hash.h"
#include "base/json.h"
#include "base/paths.h"

using bayu::base::json::Value;

namespace store::cache {

namespace {

Value list(const std::vector<std::string>& values)
{
    Value::Array array;
    for (const std::string& value : values)
        array.emplace_back(value);
    return Value(std::move(array));
}

std::vector<std::string> strings(const Value& value)
{
    std::vector<std::string> out;
    for (size_t i = 0; i < value.size(); ++i)
        out.push_back(value[i].toString());
    return out;
}

Value toJson(const App& app)
{
    Value v = Value::object();
    v.insert("id", app.id);
    v.insert("s", int(app.source));
    v.insert("name", app.name);
    v.insert("summary", app.summary);
    v.insert("description", app.description);
    v.insert("developer", app.developer);
    v.insert("verified", app.verified);
    v.insert("categories", list(app.categories));
    v.insert("filters", list(app.filters));
    v.insert("keywords", list(app.keywords));
    v.insert("icon", app.iconUrl);
    v.insert("glyph", app.glyph);
    v.insert("license", app.license);
    v.insert("free", app.freeLicense);
    v.insert("version", app.version);
    v.insert("updated", app.updated);
    v.insert("added", app.added);
    v.insert("download", int64_t(app.downloadSize));
    v.insert("installed", int64_t(app.installedSize));
    v.insert("installs", app.installs);
    v.insert("month", app.installsLastMonth);
    v.insert("trending", app.trending);
    Value::Array shots;
    for (const Screenshot& shot : app.screenshots) {
        Value s = Value::object();
        s.insert("url", shot.url);
        s.insert("caption", shot.caption);
        s.insert("portrait", shot.portrait);
        shots.push_back(std::move(s));
    }
    v.insert("shots", Value(std::move(shots)));
    v.insert("notes", app.releaseNotes);
    Value::Array permissions;
    for (const Permission& permission : app.permissions) {
        Value p = Value::object();
        p.insert("glyph", permission.glyph);
        p.insert("name", permission.name);
        p.insert("detail", permission.detail);
        permissions.push_back(std::move(p));
    }
    v.insert("permissions", Value(std::move(permissions)));
    v.insert("anti", list(app.antiFeatures));
    v.insert("homepage", app.homepage);
    v.insert("ref", app.ref);
    v.insert("remote", app.remote);
    v.insert("iconPath", app.iconPath);
    v.insert("apk", app.apkUrl);
    v.insert("sha256", app.apkSha256);
    v.insert("code", app.versionCode);
    v.insert("arm", app.armOnly);
    return v;
}

App fromJson(const Value& v)
{
    App app;
    app.id = v["id"].toString();
    app.source = Source(v["s"].toInt());
    app.name = v["name"].toString();
    app.summary = v["summary"].toString();
    app.description = v["description"].toString();
    app.developer = v["developer"].toString();
    app.verified = v["verified"].toBool();
    app.categories = strings(v["categories"]);
    app.filters = strings(v["filters"]);
    app.keywords = strings(v["keywords"]);
    app.iconUrl = v["icon"].toString();
    app.glyph = v["glyph"].toString();
    app.license = v["license"].toString();
    app.freeLicense = v["free"].toBool(true);
    app.version = v["version"].toString();
    app.updated = v["updated"].toInt();
    app.added = v["added"].toInt();
    app.downloadSize = uint64_t(v["download"].toInt());
    app.installedSize = uint64_t(v["installed"].toInt());
    app.installs = v["installs"].toInt();
    app.installsLastMonth = v["month"].toInt();
    app.trending = v["trending"].toDouble();
    const Value& shots = v["shots"];
    for (size_t i = 0; i < shots.size(); ++i)
        app.screenshots.push_back({shots[i]["url"].toString(), shots[i]["caption"].toString(),
                                   shots[i]["portrait"].toBool()});
    app.releaseNotes = v["notes"].toString();
    const Value& permissions = v["permissions"];
    for (size_t i = 0; i < permissions.size(); ++i)
        app.permissions.push_back({permissions[i]["glyph"].toString(), permissions[i]["name"].toString(),
                                   permissions[i]["detail"].toString()});
    app.antiFeatures = strings(v["anti"]);
    app.homepage = v["homepage"].toString();
    app.ref = v["ref"].toString();
    app.remote = v["remote"].toString();
    app.iconPath = v["iconPath"].toString();
    app.apkUrl = v["apk"].toString();
    app.apkSha256 = v["sha256"].toString();
    app.versionCode = v["code"].toInt();
    app.armOnly = v["arm"].toBool();
    return app;
}

} // namespace

std::string directory()
{
    const std::string dir = bayu::base::paths::cacheHome() + "/bayu-store";
    ::mkdir(dir.c_str(), 0700);
    return dir;
}

bool save(const std::string& path, const std::vector<App>& apps)
{
    const std::string temporary = path + ".new";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out)
            return false;
        for (const App& app : apps)
            out << toJson(app).serialize() << '\n';
        if (!out)
            return false;
    }
    return std::rename(temporary.c_str(), path.c_str()) == 0;
}

std::vector<App> load(const std::string& path)
{
    std::vector<App> apps;
    std::ifstream in(path, std::ios::binary);
    for (std::string line; std::getline(in, line);) {
        if (const std::optional<Value> value = Value::parse(line); value && value->isObject())
            apps.push_back(fromJson(*value));
    }
    return apps;
}

std::string fileFor(const std::string& url, const std::string& folder)
{
    const std::string dir = directory() + "/" + folder;
    ::mkdir(dir.c_str(), 0700);
    std::string extension;
    const size_t dot = url.rfind('.');
    if (dot != std::string::npos && url.size() - dot <= 5 && url.find('/', dot) == std::string::npos)
        extension = url.substr(dot);
    return dir + "/"
           + bayu::base::CryptographicHash::hashHex(url, bayu::base::CryptographicHash::Algorithm::Sha256).substr(0, 32)
           + extension;
}

} // namespace store::cache
