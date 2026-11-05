// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BAYU_AUTH_LOGIN_SESSIONS_H
#define BAYU_AUTH_LOGIN_SESSIONS_H

#include <string>
#include <string_view>
#include <vector>

namespace bayu::auth {

// A session the login screen offers: a wayland-sessions entry, or an xsessions
// one, named "... (X11)", read as bayu-dm reads it (bayu-dm/daemon/sessions.cpp),
// so both agree on what exists.
struct LoginSession {
    std::string id;
    std::string name;
    std::vector<std::string> command;
    // What pam_systemd and the session need for a daemon that, like greetd,
    // sets none of it; bayu-dm sets its own from the id.
    std::vector<std::string> environment;
};

// Whether bayu-dm can start an X server: Xorg and xinit are installed.
bool canRunXSessions();
std::vector<LoginSession> findLoginSessions(const std::vector<std::string> &directories,
                                            bool canRunX = canRunXSessions());

// wayland-sessions, then xsessions, of /usr/local/share and /usr/share, as
// bayu-dm looks.
std::vector<std::string> loginSessionDirectories();
// Replaces those directories; empty restores them.
void setLoginSessionDirectoriesForTesting(std::vector<std::string> directories);

// The accounts a person logs in with, in UID order: UIDs within login.defs'
// UID_MIN..UID_MAX that have a login shell, as sddm's user list takes them.
std::vector<std::string> findLoginUsers();

// bayu-dm's record of the last login, from the file it keeps for the login
// screen to read. Empty under any other daemon.
std::string lastLoginUser();
std::string lastLoginSession(const std::string &user);

// The copy of the user's [Appearance] settings and picture that bayu-dm
// keeps for the login screen; a user without one shows the default.
std::string loginBackgroundFile(const std::string &user);

// Splits a command line on white space, with double quotes around an
// argument holding spaces and three in a row standing for one.
std::vector<std::string> splitCommand(std::string_view command);

} // namespace bayu::auth

#endif // BAYU_AUTH_LOGIN_SESSIONS_H
