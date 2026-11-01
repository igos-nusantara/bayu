#!/usr/bin/env bash
# Rebuilds RPM Fusion's wl-kmod (akmod-wl: wl, the Broadcom Wi-Fi driver of
# older MacBooks) with packaging/wl's patches and adds it to out/repo: RPM
# Fusion's akmod-wl does not build for Bayu's kernel. The release gets
# .WL_BUILD.bayu, above RPM Fusion's and above Bayu's last build of it. A build
# is kept in WL_CACHE and reused: releases only rebuild it when WL or WL_BUILD
# changes.
#   WL=6.30.223.271-63.el10      RPM Fusion's version-release to rebuild
#   WL_BUILD=2                   raised with every change to the patches
#   WL_CACHE=~/.cache/bayu-wl
set -Eeuo pipefail
ROOT=$(realpath "$(dirname "$0")/../..")
WL=${WL:-6.30.223.271-63.el10}
WL_BUILD=${WL_BUILD:-2}
SRPM_URL=${SRPM_URL:-https://download1.rpmfusion.org/nonfree/el/updates/10/SRPMS/w/wl-kmod-$WL.src.rpm}
CACHE=${WL_CACHE:-$HOME/.cache/bayu-wl}/$WL.$WL_BUILD
mkdir -p "$CACHE" "$ROOT/out/repo"

if ! compgen -G "$CACHE/akmod-wl-*.rpm" >/dev/null; then
  work=$(mktemp -d) && trap 'rm -rf "$work"' EXIT
  curl -fL -o "$work/wl-kmod.src.rpm" "$SRPM_URL"
  cp "$ROOT"/packaging/wl/*.patch "$work/"
  podman run --rm -v "$work":/work:Z -e WL_BUILD="$WL_BUILD" bayu-builder bash -euc '
    dnf -y -q install kmodtool elfutils-libelf-devel >/dev/null
    rpm -i --define "_topdir /tmp/top" /work/wl-kmod.src.rpm
    spec=/tmp/top/SPECS/wl-kmod.spec
    # The patches here after those of RPM Fusion, numbered from 100.
    n=100
    for patch in /work/*.patch; do
      cp "$patch" /tmp/top/SOURCES/
      sed -i "/^Patch37:/a Patch$n: $(basename "$patch")" "$spec"
      sed -i "/^%patch -P 37 /a %patch -P $n -p1" "$spec"
      n=$((n + 1))
    done
    # --nodeps: buildsys-build-rpmfusion-kerneldevpkgs-akmod only gathers
    # the build root of RPM Fusion; an akmod needs kmodtool, installed above.
    rpmbuild -bb "$spec" --nodeps --define "_topdir /tmp/top" --define "dist .el10.$WL_BUILD.bayu"
    cp /tmp/top/RPMS/*/*.rpm /work/'
  cp "$work"/*.x86_64.rpm "$CACHE/"
fi
cp "$CACHE"/*.rpm "$ROOT/out/repo/"
podman run --rm -v "$ROOT/out/repo":/repo:Z bayu-builder createrepo_c -q --update /repo
ls -l "$CACHE"
