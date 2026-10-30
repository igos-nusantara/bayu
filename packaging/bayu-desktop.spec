# Builds the whole Bayu monorepo. Sources come from packaging/rpmbuild.sh:
# Source0 is `git archive` of the repository, Source1 the pinned Skia checkout
# that bayu-toolkit builds (components/bayu-toolkit/scripts/fetch-skia.sh),
# Source2 the crates bayu-computer-use builds with (cargo vendor).
%global debug_package %{nil}
%global __brp_mangle_shebangs %{nil}

Name:           bayu-desktop
Version:        %{?bayu_version}%{!?bayu_version:0.1.0}
Release:        %{?bayu_release}%{!?bayu_release:1}%{?dist}
Summary:        Bayu desktop environment
License:        GPL-3.0-or-later AND BSD-3-Clause
URL:            https://github.com/igos-nusantara/bayu
Source0:        bayu-%{version}.tar.gz
Source1:        skia.tar.gz
Source2:        computer-use-vendor.tar.gz

BuildRequires:  git cmake ninja-build pkgconf clang gcc-c++ python3 gettext
BuildRequires:  gtk-update-icon-cache libxcrypt-devel libzip-devel pam-devel
BuildRequires:  pkgconfig(wayland-client) pkgconfig(wayland-server) pkgconfig(wayland-egl)
BuildRequires:  pkgconfig(wayland-scanner) pkgconfig(wayland-protocols) pkgconfig(egl) pkgconfig(glesv2)
BuildRequires:  pkgconfig(xkbcommon) pkgconfig(libsystemd) pkgconfig(fontconfig) pkgconfig(freetype2)
BuildRequires:  pkgconfig(harfbuzz) pkgconfig(icu-uc) pkgconfig(icu-i18n) pkgconfig(expat)
BuildRequires:  pkgconfig(libpng) pkgconfig(libjpeg) pkgconfig(zlib) pkgconfig(libwebp)
BuildRequires:  pkgconfig(libtiff-4) pkgconfig(libheif) pkgconfig(libjxl) pkgconfig(libjxl_threads)
BuildRequires:  pkgconfig(libpipewire-0.3) pkgconfig(libspa-0.2) pkgconfig(gbm) pkgconfig(libdrm)
BuildRequires:  pkgconfig(libavcodec) pkgconfig(libavformat) pkgconfig(libavutil) pkgconfig(libswscale)
BuildRequires:  pkgconfig(libva) pkgconfig(libinput) pkgconfig(libseat) pkgconfig(libudev)
BuildRequires:  pkgconfig(xcb) pkgconfig(xcb-composite) pkgconfig(xcb-xfixes) pkgconfig(xcb-shape)
BuildRequires:  pkgconfig(xcb-sync) pkgconfig(xcursor) pkgconfig(libpcre2-16) pkgconfig(libpcre2-8)
BuildRequires:  pkgconfig(libarchive) pkgconfig(fuse3) pkgconfig(libmtp) pkgconfig(smbclient)
BuildRequires:  pkgconfig(libssh) pkgconfig(libcurl) pkgconfig(libical) pkgconfig(librsvg-2.0)
BuildRequires:  flatpak-devel cups-devel
BuildRequires:  cargo rust
BuildRequires:  qt6-qtbase-devel qt6-qtbase-private-devel qt6-qtdeclarative-devel

# Runtime services, from upstream's getting-started step 4.
Requires:       accountsservice polkit gsettings-desktop-schemas xdg-user-dirs
Requires:       flatpak flatpak-libs redhat-display-fonts redhat-text-fonts
# Settings > System Update and the bar's updates: the system's updates and
# the offline update that installs them at a restart.
Requires:       PackageKit
Requires:       NetworkManager NetworkManager-wifi bluez upower (power-profiles-daemon or tuned-ppd) udisks2 fuse3
Requires:       pipewire pipewire-pulseaudio wireplumber gnome-keyring gnome-keyring-pam
# xdg-desktop-portal offers OpenURI, how Flatpak apps open links and show a
# file in its folder, only with an AppChooser backend: the GTK portal's.
Requires:       xdg-desktop-portal xdg-desktop-portal-gtk xorg-x11-server-Xwayland adwaita-icon-theme
Requires:       mesa-dri-drivers mesa-libEGL mesa-libgbm glibc-langpack-en
# dbus-update-activation-environment: the session hands its environment
# (WAYLAND_DISPLAY, XDG_CURRENT_DESKTOP) to D-Bus and systemd services.
Requires:       dbus-tools
# X11 sessions at the login screen (issue #102): window managers and desktops
# that only have an xsessions entry, such as IceWM, run on Xorg, which
# bayu-dm-xsession starts with xinit and an xauth cookie.
Requires:       xorg-x11-server-Xorg xorg-x11-drv-libinput xorg-x11-xinit xorg-x11-xauth
# xdg-open opens links, from the terminal's Ctrl+click and from command-line
# programs; wl-copy is how those programs copy to the clipboard (Claude Code's
# own selection does). Without them both fail silently.
Requires:       xdg-utils wl-clipboard
# Hardware (issue #73): USB Wi-Fi adapters that first show up as a CD with
# their driver (usb_modeswitch, from packaging/usb-modeswitch), printers found
# on USB and on the network and set up with their driver (Settings > Printers),
# scanners, and the tools that show what hardware is there (lsusb, lspci...).
Requires:       usb_modeswitch usb_modeswitch-data
Requires:       cups cups-filters cups-browsed ipp-usb system-config-printer-udev avahi nss-mdns
Requires:       hplip gutenprint-cups splix
Requires:       sane-backends sane-airscan
Requires:       usbutils pciutils lshw dmidecode hwinfo lm_sensors smartmontools nvme-cli iw ethtool hdparm
# Default UI and fixed-width fonts.
Requires:       google-noto-sans-vf-fonts google-noto-sans-mono-vf-fonts
Requires(pre):  shadow-utils

%package -n nabau
Summary:        Installs the system from Bayu live media
Requires:       %{name}%{?_isa} = %{version}-%{release}
Requires:       anaconda-live anaconda-core polkit redhat-mono-fonts rsync kbd chrony
# checkisomd5: the medium is checked before a disk is changed.
Requires:       isomd5sum
Requires:       libblockdev-lvm libblockdev-dm libblockdev-mpath libblockdev-mdraid lvm2
Requires:       dosfstools e2fsprogs xfsprogs efibootmgr cryptsetup
Recommends:     ntfsprogs

%description -n nabau
Nabau, the installer of Bayu live media: a native frontend for Anaconda's
installation modules. It copies the live system to a disk, beside Windows
or alone, and removes itself from the installed system.

%description
Bayu is a Wayland desktop environment forked from Cutefish
(https://github.com/cutefishos): compositor, shell, session, display
manager, file manager, settings, terminal and the apps around them.

%prep
%autosetup -n bayu-%{version}
mkdir -p components/bayu-toolkit/third_party
tar -xzf %{SOURCE1} -C components/bayu-toolkit/third_party
# The vendored crates instead of Debian's, offline.
tar -xzf %{SOURCE2} -C components/bayu-computer-use
cat > components/bayu-computer-use/.cargo/config.toml <<'EOF'
[source.crates-io]
replace-with = "vendored"

[source.vendored]
directory = "vendor"

[net]
offline = true
EOF

%build
BUILD=$PWD/_build STAGE=$PWD/_stage ./build.sh
(cd components/bayu-computer-use && cargo build --release --locked)

%install
cp -a _stage/. %{buildroot}/
# The only path the compositor lets drive the desktop (computerControlClients).
install -Dm 755 components/bayu-computer-use/target/release/bayu-computer-use \
  %{buildroot}%{_bindir}/bayu-computer-use
install -m 644 packaging/pam/bayu-dm packaging/pam/bayu-dm-greeter packaging/pam/bayu-auth \
  %{buildroot}%{_sysconfdir}/pam.d/
install -Dm 644 packaging/fontconfig/60-bayu-noto-mono.conf \
  %{buildroot}%{_datadir}/fontconfig/conf.avail/60-bayu-noto-mono.conf
mkdir -p %{buildroot}%{_sysconfdir}/fonts/conf.d
ln -s ../../..%{_datadir}/fontconfig/conf.avail/60-bayu-noto-mono.conf \
  %{buildroot}%{_sysconfdir}/fonts/conf.d/60-bayu-noto-mono.conf
cd %{buildroot} && find . \( -type f -o -type l \) | sed -e 's#^\.##' \
  -e 's#^/etc/#%%config(noreplace) /etc/#' > %{_builddir}/all-files.list
# The installer is its own package, on live media only.
grep -E 'nabau' %{_builddir}/all-files.list > %{_builddir}/installer-files.list
grep -vE 'nabau' %{_builddir}/all-files.list > %{_builddir}/bayu-files.list

%pre
getent passwd bayu-greeter >/dev/null || \
  useradd --system --user-group --home-dir /var/lib/bayu-greeter --create-home bayu-greeter
:

%post
# Images up to 0.3.2 copied their profile overlay with its owner: / and /etc,
# and the overlay's files, belonged to the UID that built the image, which a
# later user could get. Back to root; then the boot-only directories
# systemd-tmpfiles skipped over the unsafe / (/tmp/.X11-unix for Xwayland).
if [ "$(stat -c %%u /)" != 0 ]; then
    chown root:root / && chmod go-w /
fi
find /etc /usr/share/icons/hicolor -xdev -uid +999 -exec chown root:root {} + -exec chmod go-w {} + 2>/dev/null || :
systemd-tmpfiles --create --boot /usr/lib/tmpfiles.d/x11.conf >/dev/null 2>&1 || :
# Printers on the network (issue #73), once, so that a later change by the
# administrator stays: cups-browsed makes the printers Avahi finds queues that
# every application lists, and Avahi needs mDNS through the firewall.
if [ ! -e /var/lib/bayu-desktop/printing ]; then
    systemctl enable cups.service cups-browsed.service avahi-daemon.service >/dev/null 2>&1 || :
    if firewall-cmd --state >/dev/null 2>&1; then
        firewall-cmd --permanent --add-service=mdns >/dev/null 2>&1 && firewall-cmd --reload >/dev/null 2>&1 || :
    else
        firewall-offline-cmd --add-service=mdns >/dev/null 2>&1 || :
    fi
    mkdir -p /var/lib/bayu-desktop && touch /var/lib/bayu-desktop/printing
fi
# The store installs Flathub apps system-wide.
flatpak remote-add --system --if-not-exists flathub %{_datadir}/bayu-store/flathub.flatpakrepo >/dev/null 2>&1 || :
# The Bayu GTK themes, which bayu-services copies to ~/.themes, for every Flatpak application.
flatpak override --system --filesystem=~/.themes:ro >/dev/null 2>&1 || :
gtk-update-icon-cache -q -f /usr/share/icons/* 2>/dev/null || :
glib-compile-schemas /usr/share/glib-2.0/schemas 2>/dev/null || :

%files -n nabau -f %{_builddir}/installer-files.list
%dir %{_libexecdir}/nabau
%dir %{_datadir}/cockpit
%dir %{_datadir}/cockpit/anaconda-webui

%files -f %{_builddir}/bayu-files.list
%license LICENSE LICENSE.cutefish
%doc README.md FORK.md
