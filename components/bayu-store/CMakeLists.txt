cmake_minimum_required(VERSION 3.16)

project(bayu-store
    VERSION 0.1
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(GNUInstallDirs)

find_package(BayuToolkit REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl)
pkg_check_modules(FLATPAK REQUIRED IMPORTED_TARGET flatpak)

add_library(store-ui OBJECT
    src/android_install.cpp
    src/android_setup.cpp
    src/cache.cpp
    src/catalog.cpp
    src/fdroid.cpp
    src/flathub.cpp
    src/flatpak_client.cpp
    src/lapis.cpp
    src/net.cpp
    src/mock_backend.cpp
    src/real_backend.cpp
    src/ui/pages.cpp
    src/ui/store_widgets.cpp
    src/ui/store_window.cpp
    src/ui/style.cpp
    src/ui/widgets.cpp
)
target_include_directories(store-ui PUBLIC src)
target_link_libraries(store-ui PUBLIC Bayu::Toolkit PkgConfig::CURL PkgConfig::FLATPAK)

add_executable(${PROJECT_NAME} src/main.cpp)
target_link_libraries(${PROJECT_NAME} PRIVATE store-ui)

# The tour after the first login, in the store's look and catalog.
set(WELCOME_DATA_DIR ${CMAKE_INSTALL_FULL_DATADIR}/bayu-store/welcome CACHE PATH "The tour's installed screenshots")
find_package(BayuClients REQUIRED COMPONENTS appearance)
add_executable(bayu-welcome src/welcome/main.cpp)
# The layout pictures of Settings > Desktop, for the tour's layout slide.
set(LAYOUT_PICTURES_DIR ${CMAKE_INSTALL_FULL_DATADIR}/bayu-settings/images CACHE PATH "Settings' layout pictures")
target_compile_definitions(bayu-welcome PRIVATE WELCOME_DATA_DIR="${WELCOME_DATA_DIR}"
    LAYOUT_PICTURES_DIR="${LAYOUT_PICTURES_DIR}")
target_link_libraries(bayu-welcome PRIVATE store-ui Bayu::AppearanceClient)

# Opens an Android app, with a dialog while Android starts again.
add_executable(android-launch src/launch/main.cpp)
target_link_libraries(android-launch PRIVATE store-ui)

# Installs an APK into Android, as opening one in the file manager does.
add_executable(android-install src/install/main.cpp)
target_link_libraries(android-install PRIVATE store-ui)

add_executable(store-fdroid-test tests/fdroid_test.cpp)
target_link_libraries(store-fdroid-test PRIVATE store-ui)

add_executable(store-android-setup-test tests/android_setup_test.cpp)
target_link_libraries(store-android-setup-test PRIVATE store-ui)
enable_testing()
add_test(NAME store-android-setup-test COMMAND store-android-setup-test)
add_executable(store-android-install-test tests/android_install_test.cpp)
target_link_libraries(store-android-install-test PRIVATE store-ui)
add_test(NAME store-android-install-test COMMAND store-android-install-test)

install(TARGETS ${PROJECT_NAME} bayu-welcome RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(PROGRAMS data/android-setup DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}/bayu-store)
install(TARGETS android-launch android-install RUNTIME DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}/bayu-store)
install(FILES data/lapis-session-off.conf DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/systemd/user/lapis-session.service.d
    RENAME bayu-store.conf)
install(FILES data/flathub.flatpakrepo DESTINATION ${CMAKE_INSTALL_DATADIR}/bayu-store)
install(FILES data/org.bayu.store.policy DESTINATION ${CMAKE_INSTALL_DATADIR}/polkit-1/actions)
bayu_add_translations(bayu-store SOURCES src DESKTOP_FILES data/bayu-store.desktop data/bayu-welcome.desktop
    data/bayu-android-install.desktop)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/bayu-store.desktop ${CMAKE_CURRENT_BINARY_DIR}/bayu-welcome.desktop
    ${CMAKE_CURRENT_BINARY_DIR}/bayu-android-install.desktop
    DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES data/bayu-welcome-autostart.desktop DESTINATION ${CMAKE_INSTALL_FULL_SYSCONFDIR}/xdg/autostart
    RENAME bayu-welcome.desktop)
install(DIRECTORY data/welcome/ DESTINATION ${WELCOME_DATA_DIR})
