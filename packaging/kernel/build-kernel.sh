#!/usr/bin/env bash
# Rebuilds the AlmaLinux kernel with packaging/kernel/kernel-local applied
# (binder for Android containers) and cs8409-macbookpro.patch (MacBook Pro
# sound), and adds it to the out/repo dnf repository. Its resulting config is
# written to packaging/kernel/config-x86_64 and its driver database to
# packaging/kernel/drivers.db, to commit with the change.
# The release gets .BUILD.bayu, so it sorts above the AlmaLinux build it came
# from and above Bayu's last build of it. A build is kept in KERNEL_CACHE and
# reused: releases only rebuild it when KERNEL or BUILD changes.
#   KERNEL=6.12.0-211.68.1.el10_2    AlmaLinux kernel version-release to rebuild
#   BUILD=3                          raised with every change to kernel-local or the patch
#   SRPM_URL=...                     where to fetch its source RPM
#   KERNEL_CACHE=~/.cache/bayu-kernel
set -Eeuo pipefail
ROOT=$(realpath "$(dirname "$0")/../..")
KERNEL=${KERNEL:-6.12.0-211.69.1.el10_2}
BUILD=${BUILD:-3}
# The AlmaLinux minor release the kernel is of: el10_2 is 10.2.
MINOR=$(sed -n 's/.*\.el10_\([0-9]*\).*/10.\1/p' <<<"$KERNEL")
SRPM_URL=${SRPM_URL:-https://vault.almalinux.org/${MINOR:-10.2}/BaseOS/Source/Packages/kernel-$KERNEL.src.rpm}
CACHE=${KERNEL_CACHE:-$HOME/.cache/bayu-kernel}/$KERNEL.$BUILD
mkdir -p "$CACHE" "$ROOT/out/repo"

if ! compgen -G "$CACHE/kernel-core-*.rpm" >/dev/null; then
  work=$(mktemp -d) && trap 'rm -rf "$work"' EXIT
  curl -fL -o "$work/kernel.src.rpm" "$SRPM_URL"
  cp "$ROOT/packaging/kernel/kernel-local" "$ROOT/packaging/kernel/cs8409-macbookpro.patch" "$work/"
  podman run --rm -v "$work":/work:Z -e BUILD="$BUILD" bayu-builder bash -euc '
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
# Its whole config, kept in the repository next to kernel-local: what changes
# from one kernel to the next shows in git.
podman run --rm -v "$CACHE":/kernel:ro,Z bayu-builder bash -c \
  "rpm2archive - < \$(ls /kernel/kernel-core-*.rpm | head -1) | tar xzO --wildcards './lib/modules/*/config'" \
  > "$ROOT/packaging/kernel/config-x86_64"
# And what its devices need from the repositories, for Settings > Drivers &
# Hardware (driver-db.sh), kept beside it too; bayu-desktop installs it.
"$ROOT/packaging/kernel/driver-db.sh" "$CACHE" > "$ROOT/packaging/kernel/drivers.db"
podman run --rm -v "$ROOT/out/repo":/repo:Z bayu-builder createrepo_c -q --update /repo
ls -l "$CACHE"
