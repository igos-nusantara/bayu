cmake_minimum_required(VERSION 3.22)

project(bayu-dm
    VERSION 1.0
    DESCRIPTION "Bayu display manager"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(GNUInstallDirs)
option(BUILD_TESTING "Build the test suite" OFF)
include(CTest)

find_package(BayuToolkit REQUIRED)
find_library(PAM_LIBRARY pam)
if(NOT PAM_LIBRARY)
    message(FATAL_ERROR "bayu-dm needs libpam")
endif()

set(DM_HELPER_PATH ${CMAKE_INSTALL_FULL_LIBEXECDIR}/bayu-dm-helper)
set(DM_SESSION_WRAPPER ${CMAKE_INSTALL_FULL_LIBEXECDIR}/bayu-dm-session)
set(DM_XSESSION_WRAPPER ${CMAKE_INSTALL_FULL_LIBEXECDIR}/bayu-dm-xsession)
configure_file(data/bayu-dm-xsession.in bayu-dm-xsession @ONLY)

add_library(bayu-dm-common STATIC common/wire.cpp)
target_link_libraries(bayu-dm-common PUBLIC Bayu::ToolkitBase)

add_executable(bayu-dm
    daemon/backgrounds.cpp
    daemon/config.cpp
    daemon/daemon.cpp
    daemon/greeterserver.cpp
    daemon/helperprocess.cpp
    daemon/main.cpp
    daemon/sessions.cpp
    daemon/state.cpp
    daemon/terminal.cpp
)
target_compile_definitions(bayu-dm PRIVATE
    DM_HELPER_PATH="${DM_HELPER_PATH}"
    DM_SESSION_WRAPPER="${DM_SESSION_WRAPPER}"
    DM_XSESSION_WRAPPER="${DM_XSESSION_WRAPPER}"
)
target_link_libraries(bayu-dm PRIVATE bayu-dm-common)

# No event loop and no threads: PAM modules and the session's fork run here.
add_executable(bayu-dm-helper helper/main.cpp)
target_link_libraries(bayu-dm-helper PRIVATE bayu-dm-common ${PAM_LIBRARY})

install(TARGETS bayu-dm DESTINATION ${CMAKE_INSTALL_BINDIR})
install(TARGETS bayu-dm-helper DESTINATION ${CMAKE_INSTALL_LIBEXECDIR})
install(PROGRAMS data/bayu-dm-session ${CMAKE_CURRENT_BINARY_DIR}/bayu-dm-xsession
        DESTINATION ${CMAKE_INSTALL_LIBEXECDIR})
install(FILES data/bayu-dm.service DESTINATION /usr/lib/systemd/system)
install(FILES data/bayu-dm.pam DESTINATION ${CMAKE_INSTALL_FULL_SYSCONFDIR}/pam.d
        RENAME bayu-dm)
install(FILES data/bayu-dm-greeter.pam DESTINATION ${CMAKE_INSTALL_FULL_SYSCONFDIR}/pam.d
        RENAME bayu-dm-greeter)
install(FILES data/org.bayu.DisplayManager.conf
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/dbus-1/system.d)
foreach(unit pipewire.socket pipewire.service pipewire-pulse.socket pipewire-pulse.service
        wireplumber.service filter-chain.service)
    install(FILES data/no-greeter-audio.conf
            DESTINATION /usr/lib/systemd/user/${unit}.d
            RENAME bayu-dm.conf)
endforeach()

bayu_add_translations(bayu-dm SOURCES daemon helper)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
