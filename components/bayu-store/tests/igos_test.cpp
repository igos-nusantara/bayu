// SPDX-License-Identifier: GPL-3.0-or-later
// IGOS Nusantara's remote: what the store makes of its AppStream.
//   store-igos-test <directory with appstream.xml and icons/>

#include <cstdio>
#include <fstream>
#include <sstream>

#include "igos.h"

static int failures = 0;

#define CHECK(condition)                                                                                               \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);                                            \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

int main(int argc, char** argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s directory\n", argv[0]);
        return 2;
    }
    const std::string directory = argv[1];
    std::stringstream xml;
    xml << std::ifstream(directory + "/appstream.xml").rdbuf();
    const std::vector<store::App> apps = store::igos::parseAppstream(xml.str(), directory);

    // The runtime is left out.
    CHECK(apps.size() == 2);
    if (apps.size() != 2)
        return 1;
    const store::App& photo = apps[0];
    CHECK(photo.id == "ai.storyteller.photocraft");
    CHECK(photo.source == store::Source::Flathub);
    CHECK(photo.remote == "igos");
    CHECK(photo.name == "PhotoCraft");
    CHECK(photo.summary == "Edit photos and layered PSD documents");
    CHECK(photo.description == "The open-source image editor.\n\n• Layers\n\n• PSD files");
    CHECK(photo.developer == "Learning Machines LLC");
    CHECK(photo.license == "MIT OR Apache-2.0");
    CHECK(photo.homepage == "https://getartcraft.com/apps");
    CHECK(photo.keywords == std::vector<std::string>{"photo"});
    CHECK(photo.version == "0.20.1");
    CHECK(photo.updated == 1793664000);
    CHECK(photo.ref == "app/ai.storyteller.photocraft/x86_64/stable");
    CHECK(photo.iconPath == directory + "/icons/128x128/ai.storyteller.photocraft.png");
    CHECK(photo.screenshots.size() == 1 && photo.screenshots[0].url == "https://example.org/shot.png"
          && photo.screenshots[0].caption == "Curves");
    CHECK(!photo.categories.empty());

    const store::App& word = apps[1];
    CHECK(word.id == "ai.storyteller.wordcraft");
    CHECK(word.name == "ai.storyteller.wordcraft");
    CHECK(word.ref == "app/ai.storyteller.wordcraft/x86_64/stable");
    CHECK(word.iconPath.empty());

    printf("%s\n", failures ? "FAILED" : "OK");
    return failures ? 1 : 0;
}
