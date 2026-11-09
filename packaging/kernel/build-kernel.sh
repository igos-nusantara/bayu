#!/usr/bin/env bash
# Rebuilds Fedora's kernel (its source RPM from Koji: the kernel-ark spec
# AlmaLinux's kernel comes from too) for EL10, with its Fedora config,
# packaging/kernel/kernel-local applied (binder for Android containers, and
# what AlmaLinux's 6.12 build had that Fedora's leaves out, issue #133) and
# cs8409-macbookpro.patch (MacBook Pro sound), and adds it to the out/repo
# dnf repository. Its resulting config is written to
# packaging/kernel/config-x86_64 and its driver database to
# packaging/kernel/drivers.db, to commit with the change.
# The release gets .BUILD.bayu.el10_2, above Bayu's last build of it. A build
# is kept in KERNEL_CACHE and reused: releases only rebuild it when KERNEL or
# BUILD changes.
#   KERNEL=7.3.1-200.fc44    Fedora kernel version-release to rebuild
#   BUILD=3                  raised with every change to kernel-local or the patch
#   SRPM_URL=...             where to fetch its source RPM
#   HEADERS=7.3.0-65.fc46    Fedora kernel-headers version-release to rebuild
#   HEADERS_URL=...          where to fetch its source RPM
#   KERNEL_CACHE=~/.cache/bayu-kernel
set -Eeuo pipefail
ROOT=$(realpath "$(dirname "$0")/../..")
KERNEL=${KERNEL:-7.3.3-200.fc44}
BUILD=${BUILD:-3}
SRPM_URL=${SRPM_URL:-https://kojipkgs.fedoraproject.org/packages/kernel/${KERNEL%-*}/${KERNEL##*-}/src/kernel-$KERNEL.src.rpm}
CACHE=${KERNEL_CACHE:-$HOME/.cache/bayu-kernel}/$KERNEL.$BUILD
mkdir -p "$CACHE" "$ROOT/out/repo"

if ! compgen -G "$CACHE/kernel-core-*.rpm" >/dev/null; then
  work=$(mktemp -d) && trap 'rm -rf "$work"' EXIT
  curl -fL -o "$work/kernel.src.rpm" "$SRPM_URL"
  cp "$ROOT/packaging/kernel/kernel-local" "$ROOT/packaging/kernel/cs8409-macbookpro.patch" "$work/"
  podman run --rm -v "$work":/work:Z -e BUILD="$BUILD" bayu-builder bash -euc '
    # The Fedora config: on EL10 the spec would pick the RHEL one, without
    # drivers the 6.12 build had (issue #133).
    printf "%%fedora 44\n%%rhel %%{nil}\n" > ~/.rpmmacros
    rpm -i --define "_topdir /tmp/top" /work/kernel.src.rpm
    cp /work/kernel-local /tmp/top/SOURCES/kernel-local
    cp /work/cs8409-macbookpro.patch /tmp/top/SOURCES/linux-kernel-test.patch
    dnf -y -q builddep --define "_with_baseonly 1" /tmp/top/SPECS/kernel.spec
    rpmbuild -bb /tmp/top/SPECS/kernel.spec --define "_topdir /tmp/top" --define "buildid .$BUILD.bayu" --define "dist .el10_2" \
      --with baseonly --without debug --without debuginfo --without doc --without perf \
      --without libperf --without tools --without selftests --without kabichk \
      --without kernel_abi_stablelists --without zfcpdump --without efiuki --without realtime
    cp /tmp/top/RPMS/*/*.rpm /work/'
  cp "$work"/kernel*.x86_64.rpm "$CACHE/"
fi
cp "$CACHE"/kernel*.x86_64.rpm "$ROOT/out/repo/"

# Its userspace headers (/usr/include/linux), from Fedora's kernel-headers
# source RPM: Fedora's kernel spec leaves them out. EL10's 6.12 ones keep
# working, the kernel API only grows; these let programs use what 7.x adds.
# Fedora names them after the 7.3 series, not its point releases.
HEADERS=${HEADERS:-7.3.0-65.fc46}
HEADERS_URL=${HEADERS_URL:-https://kojipkgs.fedoraproject.org/packages/kernel-headers/${HEADERS%-*}/${HEADERS##*-}/src/kernel-headers-$HEADERS.src.rpm}
HEADERS_CACHE=${KERNEL_CACHE:-$HOME/.cache/bayu-kernel}/headers-$HEADERS.$BUILD
mkdir -p "$HEADERS_CACHE"
if ! compgen -G "$HEADERS_CACHE/kernel-headers-*.rpm" >/dev/null; then
  hwork=$(mktemp -d)
  curl -fL -o "$hwork/kernel-headers.src.rpm" "$HEADERS_URL"
  podman run --rm -v "$hwork":/work:Z -e BUILD="$BUILD" bayu-builder bash -euc '
    rpm -i --define "_topdir /tmp/top" /work/kernel-headers.src.rpm
    dnf -y -q builddep /tmp/top/SPECS/kernel-headers.spec
    rpmbuild -bb /tmp/top/SPECS/kernel-headers.spec --define "_topdir /tmp/top" --define "buildid .$BUILD.bayu" --define "dist .el10_2"
    cp /tmp/top/RPMS/*/*.rpm /work/'
  cp "$hwork"/kernel-*headers-*.rpm "$HEADERS_CACHE/"
  rm -rf "$hwork"
fi
cp "$HEADERS_CACHE"/kernel-*headers-*.rpm "$ROOT/out/repo/"
# Its whole config, kept in the repository next to kernel-local: what changes
# from one kernel to the next shows in git.
podman run --rm -v "$CACHE":/kernel:ro,Z bayu-builder bash -c \
  "rpm2archive - < \$(ls /kernel/kernel-core-*.rpm | head -1) | tar xzO --wildcards './lib/modules/*/config'" \
  > "$ROOT/packaging/kernel/config-x86_64"
# And what its devices need from the repositories, for Settings > Drivers &
# Hardware (driver-db.sh), kept beside it too; bayu-desktop installs it.
"$ROOT/packaging/kernel/driver-db.sh" "$CACHE" > "$ROOT/packaging/kernel/drivers.db"
podman run --rm -v "$ROOT/out/repo":/repo:Z bayu-builder createrepo_c -q --update /repo
ls -l "$CACHE" "$HEADERS_CACHE"
