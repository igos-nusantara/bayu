# SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
# SPDX-License-Identifier: BSD-3-Clause

cmake_minimum_required(VERSION 3.16)

project(filemanager LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(GNUInstallDirs)
include(CTest)

find_package(BayuToolkit REQUIRED)
find_package(BayuFiles REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(ICU REQUIRED IMPORTED_TARGET icu-uc icu-i18n)

# ---------------------------------------------------------------------------
# bayu-mountd: phones and servers mounted with FUSE, so their files are
# local paths to everything else.
# ---------------------------------------------------------------------------
pkg_check_modules(Fuse3 REQUIRED IMPORTED_TARGET fuse3)
pkg_check_modules(LibSsh REQUIRED IMPORTED_TARGET libssh)
pkg_check_modules(SmbClient REQUIRED IMPORTED_TARGET smbclient)
pkg_check_modules(LibMtp REQUIRED IMPORTED_TARGET libmtp)
pkg_check_modules(LibUdev REQUIRED IMPORTED_TARGET libudev)

add_library(mountd-core STATIC
    mountd/fusemount.cpp
    mountd/keyring.cpp
    mountd/localbackend.cpp
    mountd/mountservice.cpp
    mountd/mtpbackend.cpp
    mountd/mtpmonitor.cpp
    mountd/remoteurl.cpp
    mountd/sftpbackend.cpp
    mountd/smbbackend.cpp
)
target_include_directories(mountd-core PUBLIC mountd)
target_link_libraries(mountd-core PUBLIC
    Bayu::ToolkitBase
    PkgConfig::Fuse3
    PkgConfig::LibSsh
    PkgConfig::SmbClient
    PkgConfig::LibMtp
    PkgConfig::LibUdev
    PkgConfig::ICU
)

add_executable(bayu-mountd mountd/main.cpp)
target_link_libraries(bayu-mountd PRIVATE mountd-core)

# bayu-admin-mount: the file system as root, for the user who asked through
# polkit, so the file manager can open folders as an administrator.
add_executable(bayu-admin-mount mountd/adminmain.cpp)
target_link_libraries(bayu-admin-mount PRIVATE mountd-core)
install(TARGETS bayu-admin-mount RUNTIME DESTINATION ${CMAKE_INSTALL_LIBEXECDIR})
install(FILES data/org.bayu.filemanager.admin.policy DESTINATION ${CMAKE_INSTALL_DATADIR}/polkit-1/actions)

configure_file(mountd/org.bayu.Mounts1.service.in org.bayu.Mounts1.service @ONLY)

install(TARGETS bayu-mountd RUNTIME DESTINATION ${CMAKE_INSTALL_LIBEXECDIR})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/org.bayu.Mounts1.service
        DESTINATION ${CMAKE_INSTALL_DATADIR}/dbus-1/services)

# ---------------------------------------------------------------------------
# bayu-filemanager-daemon: file operations that outlive the windows,
# org.bayu.FileOperations.
# ---------------------------------------------------------------------------
find_path(LIBZIP_INCLUDE_DIR zip.h)
find_library(LIBZIP_LIBRARY NAMES zip)
if(NOT LIBZIP_INCLUDE_DIR OR NOT LIBZIP_LIBRARY)
    message(FATAL_ERROR "libzip development files are required to build filemanager")
endif()
pkg_check_modules(LibArchive REQUIRED IMPORTED_TARGET libarchive)

add_library(fileoperations-core STATIC
    daemon/archivejob.cpp
    daemon/daemontask.cpp
    daemon/fileoperationsservice.cpp
)
target_include_directories(fileoperations-core PUBLIC ${PROJECT_SOURCE_DIR} daemon ${LIBZIP_INCLUDE_DIR})
target_link_libraries(fileoperations-core PUBLIC Bayu::ToolkitBase Bayu::Files ${LIBZIP_LIBRARY}
                      PkgConfig::LibArchive)

add_executable(bayu-filemanager-daemon daemon/main.cpp)
target_link_libraries(bayu-filemanager-daemon PRIVATE fileoperations-core)

configure_file(daemon/org.bayu.FileOperations.service.in org.bayu.FileOperations.service @ONLY)
install(TARGETS bayu-filemanager-daemon RUNTIME DESTINATION ${CMAKE_INSTALL_LIBEXECDIR})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/org.bayu.FileOperations.service
        DESTINATION ${CMAKE_INSTALL_DATADIR}/dbus-1/services)

# ---------------------------------------------------------------------------
# The file manager's models: listing, sorting, selection, file operations
# through the daemon. No UI beyond the clipboard.
# ---------------------------------------------------------------------------
add_library(fm-core STATIC
    src/core/config.cpp
    src/core/dates.cpp
    src/core/fileitem.cpp
    src/core/launcher.cpp
    src/core/lister.cpp
    src/core/notifications.cpp
    src/core/permissions.cpp
    src/core/preferences.cpp
    src/core/previewkeys.cpp
    src/core/standardfolders.cpp
    src/core/mounts.cpp
    src/core/shortcutconvention.cpp
    src/core/thumbnails.cpp
    src/model/foldercontents.cpp
    src/model/foldermodel.cpp
    src/model/foldersizecache.cpp
    src/model/pathbarmodel.cpp
    src/model/pathhistory.cpp
    src/model/places.cpp
    src/model/positioner.cpp
    src/ops/operations.cpp
)
target_include_directories(fm-core PUBLIC src)
target_link_libraries(fm-core PUBLIC Bayu::Toolkit Bayu::Files Bayu::FilesThumbnails)

# The browser window, its views and dialogs.
add_library(fm-ui STATIC
    src/views/colors.cpp
    src/views/completionpopup.cpp
    src/views/foldermenu.cpp
    src/views/folderpage.cpp
    src/views/folderview.cpp
    src/views/folderwindow.cpp
    src/views/gridview.cpp
    src/views/icongridview.cpp
    src/views/listview.cpp
    src/views/pathbar.cpp
    src/views/previewclient.cpp
    src/views/renameeditor.cpp
    src/views/sidebar.cpp
    src/views/style.cpp
    src/dialogs/archivepassworddialog.cpp
    src/dialogs/archiveprogressdialog.cpp
    src/dialogs/framelessdialog.cpp
    src/dialogs/iotaskdialog.cpp
    src/dialogs/openwithdialog.cpp
    src/dialogs/propertiesdialog.cpp
    src/dialogs/serverdialogs.cpp
    src/dialogs/unlockdialog.cpp
    src/chooser/chooserwindow.cpp
    src/desktop/desktopmenuexporter.cpp
    src/desktop/desktopview.cpp
    src/desktop/desktopwindow.cpp
    src/chooser/filechooser.cpp
    src/preview/imageinfo.cpp
    src/preview/imagepreview.cpp
    src/preview/previewcontroller.cpp
    src/preview/previewwindow.cpp
)
target_link_libraries(fm-ui PUBLIC fm-core PkgConfig::ICU)

add_executable(bayu-filemanager
    src/app/application.cpp
    src/app/chooserservice.cpp
    src/app/dialogs.cpp
    src/app/main.cpp
    src/app/previewservice.cpp
)
target_link_libraries(bayu-filemanager PRIVATE fm-ui)
install(TARGETS bayu-filemanager RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
configure_file(data/org.bayu.FileManager1.service.in org.bayu.FileManager1.service @ONLY)
configure_file(data/org.bayu.FilePreview.service.in org.bayu.FilePreview.service @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/org.bayu.FileManager1.service
              ${CMAKE_CURRENT_BINARY_DIR}/org.bayu.FilePreview.service
        DESTINATION ${CMAKE_INSTALL_DATADIR}/dbus-1/services)
install(FILES data/60-bayu-filemanager.rules DESTINATION ${CMAKE_INSTALL_DATADIR}/polkit-1/rules.d)

bayu_add_translations(bayu-filemanager SOURCES src daemon mountd
                          DESKTOP_FILES data/bayu-filemanager.desktop)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/bayu-filemanager.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
