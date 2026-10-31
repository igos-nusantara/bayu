#pragma once

#include <optional>
#include <string>
#include <vector>

namespace dm {

// One entry of wayland-sessions or xsessions, as sddm and GDM offer it.
struct SessionEntry {
    // The file name without ".desktop", e.g. "bayu-wayland".
    std::string id;
    std::string name;
    std::vector<std::string> exec;
    std::vector<std::string> desktopNames;
    // From xsessions: started on an X server (bayu-dm-xsession).
    bool x11 = false;
};

// Hidden, NoDisplay, a missing TryExec or no Exec: not a session.
std::optional<SessionEntry> readSessionFile(const std::string &path);

// An id in an earlier directory hides the same id in later ones. Entries of an
// xsessions directory are X11 sessions, named "... (X11)", and offered only
// with Xorg and xinit installed.
bool canRunXSessions();
std::vector<SessionEntry> findSessions(const std::vector<std::string> &directories,
                                       bool canRunX = canRunXSessions());

std::optional<SessionEntry> findSession(const std::vector<std::string> &directories, const std::string &id);

// Bayu's own session, whose compositor waits for its seat to be active, so
// that the login screen can stay until it draws.
bool isBayuSession(const SessionEntry &session);

// For pam_systemd, set before pam_open_session (sddm's Display::startAuth).
std::vector<std::string> sessionEnvironment(const SessionEntry &session, int vt);
std::vector<std::string> greeterEnvironment(int vt, const std::string &socket);

} // namespace dm
