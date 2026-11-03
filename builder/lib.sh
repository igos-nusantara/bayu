# Shared by build-image.sh and build-iso.sh; sourced, not run.

# Outside the container: pick the profile and re-run the calling script with
# --inside in a privileged AlmaLinux container (loop devices and chroots).
#   PROFILE=igos|almalinux   profile under builder/profiles/ (default igos)
#   CONFIG=file REPOS=dir    override the profile's profile.conf / repos.d
# A rootfs/ directory next to profile.conf is copied over the system.
run_in_builder() {
  local here root profile config repos overlay podman
  here=$(realpath "$(dirname "$0")")
  root=$(realpath "$here/..")
  profile=$here/profiles/${PROFILE:-igos}
  config=$(realpath "${CONFIG:-$profile/profile.conf}")
  repos=$(realpath "${REPOS:-$profile/repos.d}")
  overlay=$(dirname "$config")/rootfs
  if [[ -d $overlay ]]; then overlay=$(realpath "$overlay"); else overlay=$(mktemp -d); fi
  mkdir -p "$root/out/repo"
  podman=podman; [[ $EUID == 0 ]] || podman="sudo podman"
  $podman image exists bayu-builder ||
    $podman build -t bayu-builder -f "$root/packaging/Containerfile" "$root/packaging"
  exec $podman run --rm --privileged --network host -v /dev:/dev \
    -v "$here":/builder:ro -v "$root/packaging":/packaging:ro -v "$config":/profile.conf:ro \
    -v "$repos":/repos.d:ro -v "$overlay":/overlay:ro -v "$root/out/repo":/repo:ro -v "$root/out":/out \
    bayu-builder bash "/builder/$(basename "$0")" --inside
}

# install_root DIR: installs $PACKAGES (plus any arguments) into DIR from
# /repos.d, with /dev, /proc and /sys mounted for package scriptlets.
install_root() {
  local dir=$1; shift
  mkdir -p "$dir"/{dev,proc,sys,etc/kernel,etc/sysconfig}
  mount --bind /dev "$dir/dev" && mount -t proc proc "$dir/proc" && mount -t sysfs sys "$dir/sys"
  # Kernel installs and updates become the boot default.
  printf 'UPDATEDEFAULT=yes\nDEFAULTKERNEL=kernel-core\n' > "$dir/etc/sysconfig/kernel"
  # The container image keeps only C.utf8 translations (%_install_langs);
  # the system gets them all.
  rm -f /etc/rpm/macros.image-language-conf
  # out/repo, built here, is trusted unsigned; the installed system checks
  # Bayu's published packages against the IGOS key. With their documentation:
  # without it `man` has nothing to show (issue #87).
  # The published Bayu repository too, signed: the installed system updates
  # from it, with every earlier release, and the image has to get what it
  # would pick, or a package Bayu stopped building comes back as an update.
  local published=()
  if [[ -n ${BAYU_REPO_URL:-} ]]; then
    published=(--repofrompath="bayu-published,$BAYU_REPO_URL" --setopt=bayu-published.gpgcheck=1
               --setopt=bayu-published.gpgkey="$(sed -n 's/^gpgkey=//p' /repos.d/bayu.repo)")
  fi
  # shellcheck disable=SC2086
  dnf -y --installroot="$dir" --releasever="$RELEASEVER" --setopt=reposdir=/repos.d "${published[@]}" \
    --setopt=install_weak_deps=False --setopt=bayu.gpgcheck=0 install $PACKAGES "$@"
  # The system updates from the repositories it was built from. A file:///repo
  # source only exists at build time: it points at BAYU_REPO_URL when one is
  # set, and is left disabled otherwise.
  rm -f "$dir"/etc/yum.repos.d/*.repo && cp /repos.d/*.repo "$dir/etc/yum.repos.d/"
  if [[ -n ${BAYU_REPO_URL:-} ]]; then
    grep -l 'file:///repo' "$dir"/etc/yum.repos.d/*.repo | xargs -r sed -i "s|^baseurl=file:///repo.*|baseurl=$BAYU_REPO_URL|"
  else
    grep -l 'file:///repo' "$dir"/etc/yum.repos.d/*.repo | xargs -r sed -i 's/^enabled=1/enabled=0/'
  fi
}

# configure_root DIR: locale, users, login and services from the profile.
configure_root() {
  local dir=$1
  chr() { chroot "$dir" "$@"; }
  # wl, the Broadcom Wi-Fi driver of older MacBooks, built for the image's
  # kernels now: akmods would build it at the first boot only, too late for
  # the live session and for that boot's Wi-Fi.
  if chr rpm -q akmod-wl >/dev/null 2>&1; then
    local kernel
    for kernel in $(chr rpm -q --qf '%{VERSION}-%{RELEASE}.%{ARCH}\n' kernel-core); do
      chr akmods --force --kernels "$kernel" >/dev/null 2>&1 &&
        ls "$dir"/lib/modules/"$kernel"/extra/wl/wl.ko* >/dev/null 2>&1 ||
        echo "WARNING: wl did not build for $kernel; MacBook Wi-Fi waits for akmods at boot." >&2
    done
  fi
  echo "$HOSTNAME" > "$dir/etc/hostname"
  echo "LANG=$LOCALE" > "$dir/etc/locale.conf"
  echo "KEYMAP=$KEYMAP" > "$dir/etc/vconsole.conf"
  ln -sf "../usr/share/zoneinfo/$TIMEZONE" "$dir/etc/localtime"
  sed -i "s/^SELINUX=.*/SELINUX=$SELINUX/" "$dir/etc/selinux/config"

  chr useradd -m -G wheel "$USER_NAME"
  echo "$USER_NAME:$USER_PASSWORD" | chr chpasswd
  if [[ -n $ROOT_PASSWORD ]]; then echo "root:$ROOT_PASSWORD" | chr chpasswd; else chr passwd -l root; fi
  if [[ ${SUDO_NOPASSWD:-no} == yes ]]; then
    echo "$USER_NAME ALL=(ALL) NOPASSWD: ALL" > "$dir/etc/sudoers.d/90-$USER_NAME"
    chmod 440 "$dir/etc/sudoers.d/90-$USER_NAME"
  fi
  if [[ ${AUTOLOGIN:-no} == yes ]]; then
    printf '[Autologin]\nUser=%s\n' "$USER_NAME" > "$dir/etc/bayu-dm.conf"
  fi
  chr systemctl enable bayu-dm.service NetworkManager.service
  chr systemctl set-default graphical.target

  # lapis enables its control socket on install; keep it off unless asked.
  # The autostart entry only starts Android once the socket is enabled.
  if chr rpm -q lapis >/dev/null 2>&1; then
    install -Dm644 /packaging/lapis/lapis-session.desktop "$dir/etc/xdg/autostart/lapis-session.desktop"
    if [[ ${LAPIS_ENABLED:-no} == yes ]]; then
      chr systemctl enable lapis-control.socket
    else
      chr systemctl disable lapis-control.socket
    fi
    # dnf installs the newest kernel; once the distribution ships one newer
    # than Bayu's, lapis has no binder until packaging/kernel is rebuilt.
    local config
    for config in "$dir"/lib/modules/*/config; do
      grep -q '^CONFIG_ANDROID_BINDER_IPC=y' "$config" ||
        echo "WARNING: kernel $(basename "$(dirname "$config")") has no binder; lapis will not run." \
             "Rebuild it with packaging/kernel/build-kernel.sh." >&2
    done
  fi

  # Owned by root, without group or other write, and without touching the
  # directories the system already has: cp -a gave / and /etc the owner and
  # mode of the overlay checkout, which a later user with that UID would own.
  tar -C /overlay -cf - --owner=0 --group=0 --numeric-owner --mode=go-w . |
    tar -C "$dir" -xf - --no-overwrite-dir --same-owner
  # A profile's own icons, such as its logo, join the hicolor cache.
  [[ -x $dir/usr/bin/gtk-update-icon-cache ]] &&
    chroot "$dir" gtk-update-icon-cache -qf /usr/share/icons/hicolor

  # Anaconda goes on when rsync fails with any code but 11, and stops later at
  # systemd-machine-id-setup without saying why (rhbz#1963778): installs say
  # what stopped the copy (a full disk, a USB drive that cannot be read).
  local site
  for site in "$dir"/usr/lib64/python3*/site-packages; do
    [[ -d $site/pyanaconda ]] || continue
    patch -d "$site" -p1 --forward --quiet < /builder/patches/anaconda-rsync-failures.patch || {
      echo "ERROR: builder/patches/anaconda-rsync-failures.patch does not apply to this anaconda." >&2
      return 1
    }
  done

  # Each boot of a copy of this system gets its own machine-id.
  : > "$dir/etc/machine-id"
}

# release_root DIR: stops what package scriptlets left running in DIR (such
# as gpg-agent) and unmounts what install_root mounted.
release_root() {
  local pid m root
  root=$(realpath "$1")
  for pid in /proc/[0-9]*; do
    if [[ $(readlink "$pid/root" 2>/dev/null) == "$root" ]]; then kill -9 "${pid#/proc/}" 2>/dev/null || true; fi
  done
  for m in sys proc dev; do
    if mountpoint -q "$root/$m"; then umount -R "$root/$m"; fi
  done
}
