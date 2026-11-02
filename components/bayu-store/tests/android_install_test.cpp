// SPDX-License-Identifier: GPL-3.0-or-later
// android-install's decisions: when an APK goes through a copy lapis can
// read, and which package an install added or updated.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unistd.h>

#include "android_install.h"

namespace {

int failures = 0;

void expect(bool ok, const char* what)
{
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

} // namespace

int main()
{
    using store::install::changedPackage;
    using store::install::readableByLapis;

    // A file of the user's, outside /tmp, is read where it is.
    const char* home = std::getenv("HOME");
    const std::string mine = std::string(home ? home : ".") + "/.android-install-test-" + std::to_string(getpid()) + ".apk";
    std::ofstream(mine) << "apk";
    expect(readableByLapis(mine), "a file of the user's is read where it is");
    // lapis' service has a private /tmp.
    const std::string temporary = "/tmp/android-install-test-" + std::to_string(getpid()) + ".apk";
    std::ofstream(temporary) << "apk";
    expect(!readableByLapis(temporary), "a file in /tmp goes through a copy");
    expect(!readableByLapis(mine + ".missing"), "a missing file is not readable");
    expect(store::install::stagingPath("/tmp/x/F-Droid.apk").ends_with("/bayu-store/apk/F-Droid.apk"),
           "the copy keeps the file name");
    std::remove(mine.c_str());
    std::remove(temporary.c_str());

    // New, then updated, then nothing.
    expect(changedPackage({{"a", 1}}, {{"a", 1}, {"org.fdroid.fdroid", 1023050}}) == "org.fdroid.fdroid",
           "a new package");
    expect(changedPackage({{"a", 1}, {"b", 2}}, {{"a", 1}, {"b", 3}}) == "b", "an updated package");
    expect(changedPackage({{"a", 1}, {"b", 2}}, {{"b", 3}, {"c", 1}}) == "c", "a new one before an update");
    expect(changedPackage({{"a", 1}}, {{"a", 1}}).empty(), "nothing changed");

    if (failures == 0)
        printf("ok\n");
    return failures ? 1 : 0;
}
