#pragma once

#include <string>
#include <vector>

namespace dm {

struct Config {
    // Separate VTs keep a session's end off the screen; VT1 has the boot splash.
    int greeterVt = 1;
    int sessionVt = 2;
    std::string greeterUser = "bayu-greeter";
    std::vector<std::string> greeterCommand = {"/usr/bin/bayu-compositor", "--login"};
    std::string greeterService = "bayu-dm-greeter";
    std::string sessionService = "bayu-dm";
    // Wayland's first: an X11 session of the same id is hidden behind it.
    std::vector<std::string> sessionDirectories = {"/usr/local/share/wayland-sessions", "/usr/share/wayland-sessions",
                                                   "/usr/local/share/xsessions", "/usr/share/xsessions"};
    std::string defaultSession = "bayu-wayland";
    // Logged in without a password once per boot, to `autologinSession` or the
    // default; nobody when empty.
    std::string autologinUser;
    std::string autologinSession;
    std::string statePath = "/var/lib/bayu-dm/state.conf";
    std::string backgroundDirectory = "/var/lib/bayu-dm/backgrounds";
    std::string runtimeDirectory = "/run/bayu-dm";

    // Defaults, overridden by whatever `path` sets.
    static Config load(const std::string &path = "/etc/bayu-dm.conf");
};

// Splits a command line at white space; double quotes group, and three in a
// row stand for a quote character.
std::vector<std::string> splitCommand(const std::string &command);

} // namespace dm
