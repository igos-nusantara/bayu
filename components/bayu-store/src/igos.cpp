// SPDX-License-Identifier: GPL-3.0-or-later

#include "igos.h"

#include <flatpak.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "catalog.h"
#include "flatpak_client.h"

namespace store::igos {

namespace {

std::string collapsed(const std::string& text)
{
    std::string out;
    bool space = false;
    for (char c : text) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            space = !out.empty();
            continue;
        }
        if (space)
            out += ' ';
        space = false;
        out += c;
    }
    return out;
}

const char* attribute(const gchar** names, const gchar** values, const char* name)
{
    for (int i = 0; names[i]; ++i) {
        if (std::strcmp(names[i], name) == 0)
            return values[i];
    }
    return nullptr;
}

struct Parser
{
    std::string iconDir;
    std::vector<App> apps;
    std::vector<std::string> stack;
    std::string text;
    // Inside an element in another language, or a component that is no app.
    int translated = 0;
    bool app = false;
    App current;
    std::vector<std::string> categories;
    std::string icon;
    int iconHeight = 0;
    bool released = false;
    std::string imageType;
    std::string caption;

    std::string parent() const { return stack.size() >= 2 ? stack[stack.size() - 2] : std::string(); }
};

void startElement(GMarkupParseContext*, const gchar* name, const gchar** names, const gchar** values, gpointer data,
                  GError**)
{
    auto& p = *static_cast<Parser*>(data);
    p.stack.push_back(name);
    p.text.clear();
    if (p.translated || attribute(names, values, "xml:lang")) {
        ++p.translated;
        return;
    }
    const std::string element = name;
    if (element == "component") {
        const char* type = attribute(names, values, "type");
        p.app = type && (std::strcmp(type, "desktop-application") == 0 || std::strcmp(type, "desktop") == 0);
        p.current = App();
        p.current.source = Source::Flathub;
        p.current.remote = kRemote;
        p.current.glyph = "package";
        p.categories.clear();
        p.icon.clear();
        p.iconHeight = 0;
        p.released = false;
    } else if (element == "icon") {
        const char* type = attribute(names, values, "type");
        const char* height = attribute(names, values, "height");
        const int size = height ? std::atoi(height) : 0;
        // The 128 pixel one at scale 1, else the largest up to it.
        if (type && std::strcmp(type, "cached") == 0 && !attribute(names, values, "scale") && size <= 128
            && size > p.iconHeight)
            p.iconHeight = size;
        else
            p.stack.back() = "icon-other";
    } else if (element == "release" && !p.released) {
        p.released = true;
        if (const char* version = attribute(names, values, "version"))
            p.current.version = version;
        if (const char* timestamp = attribute(names, values, "timestamp"))
            p.current.updated = std::atoll(timestamp);
    } else if (element == "url") {
        const char* type = attribute(names, values, "type");
        if (!type || std::strcmp(type, "homepage") != 0)
            p.stack.back() = "url-other";
    } else if (element == "image") {
        const char* type = attribute(names, values, "type");
        p.imageType = type ? type : "source";
    } else if (element == "screenshot") {
        p.caption.clear();
    }
}

void endElement(GMarkupParseContext*, const gchar*, gpointer data, GError**)
{
    auto& p = *static_cast<Parser*>(data);
    const std::string element = p.stack.back();
    const std::string parent = p.parent();
    const std::string text = collapsed(p.text);
    p.text.clear();
    if (p.translated) {
        --p.translated;
        p.stack.pop_back();
        return;
    }
    App& app = p.current;
    if (parent == "component") {
        if (element == "id")
            app.id = text.ends_with(".desktop") ? text.substr(0, text.size() - 8) : text;
        else if (element == "name")
            app.name = text;
        else if (element == "summary")
            app.summary = text;
        else if (element == "developer_name" && app.developer.empty())
            app.developer = text;
        else if (element == "project_license")
            app.license = text;
        else if (element == "icon")
            p.icon = text;
        else if (element == "url")
            app.homepage = text;
        else if (element == "bundle")
            app.ref = text;
    } else if (parent == "developer" && element == "name") {
        app.developer = text;
    } else if (parent == "description" && element == "p" && !text.empty()) {
        app.description += (app.description.empty() ? "" : "\n\n") + text;
    } else if (element == "li" && !text.empty()) {
        app.description += (app.description.empty() ? "• " : "\n\n• ") + text;
    } else if (element == "category") {
        p.categories.push_back(text);
    } else if (element == "keyword") {
        app.keywords.push_back(text);
    } else if (element == "caption" && parent == "screenshot") {
        p.caption = text;
    } else if (element == "image" && parent == "screenshot" && p.imageType == "source") {
        app.screenshots.push_back({text, p.caption});
    } else if (element == "component") {
        if (p.app && !app.id.empty()) {
            if (app.name.empty())
                app.name = app.id;
            app.categories = categoriesForFreedesktop(p.categories);
            if (app.ref.empty())
                app.ref = "app/" + app.id + "/x86_64/stable";
            if (!p.icon.empty()) {
                const std::string path = p.iconDir + "/icons/" + std::to_string(p.iconHeight) + "x"
                        + std::to_string(p.iconHeight) + "/" + p.icon;
                if (std::filesystem::exists(path))
                    app.iconPath = path;
            }
            p.apps.push_back(std::move(app));
        }
        p.app = false;
    }
    p.stack.pop_back();
}

void textElement(GMarkupParseContext*, const gchar* text, gsize length, gpointer data, GError**)
{
    static_cast<Parser*>(data)->text.append(text, length);
}

} // namespace

std::vector<App> parseAppstream(const std::string& xml, const std::string& iconDir)
{
    Parser parser;
    parser.iconDir = iconDir;
    const GMarkupParser callbacks{startElement, endElement, textElement, nullptr, nullptr};
    GMarkupParseContext* context = g_markup_parse_context_new(&callbacks, GMarkupParseFlags(0), &parser, nullptr);
    GError* error = nullptr;
    if (!g_markup_parse_context_parse(context, xml.data(), gssize(xml.size()), &error)
        || !g_markup_parse_context_end_parse(context, &error))
        g_clear_error(&error);
    g_markup_parse_context_free(context);
    return parser.apps;
}

std::vector<App> fetchCatalog(std::string* error)
{
    std::string failure = flatpak::ensureRemote(kRemote, kRepoFile);
    if (!failure.empty()) {
        if (error)
            *error = "IGOS Nusantara: " + failure;
        return {};
    }
    GError* gerror = nullptr;
    FlatpakInstallation* installation = flatpak_installation_new_system(nullptr, &gerror);
    if (!installation) {
        if (error)
            *error = std::string("IGOS Nusantara: ") + (gerror ? gerror->message : "no Flatpak installation");
        g_clear_error(&gerror);
        return {};
    }
    // Offline, or not allowed: the AppStream from last time still lists them.
    flatpak_installation_update_appstream_full_sync(installation, kRemote, nullptr, nullptr, nullptr, nullptr,
                                                    nullptr, &gerror);
    g_clear_error(&gerror);
    std::string directory;
    if (FlatpakRemote* remote = flatpak_installation_get_remote_by_name(installation, kRemote, nullptr, &gerror)) {
        if (GFile* file = flatpak_remote_get_appstream_dir(remote, nullptr)) {
            if (char* path = g_file_get_path(file)) {
                directory = path;
                g_free(path);
            }
            g_object_unref(file);
        }
        g_object_unref(remote);
    }
    g_clear_error(&gerror);
    g_object_unref(installation);

    std::stringstream xml;
    xml << std::ifstream(directory + "/appstream.xml", std::ios::binary).rdbuf();
    std::vector<App> apps = directory.empty() ? std::vector<App>() : parseAppstream(xml.str(), directory);
    if (apps.empty() && error)
        *error = "IGOS Nusantara: its app list is not there yet.";
    return apps;
}

} // namespace store::igos
