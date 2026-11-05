#!/usr/bin/env bash
# Writes the driver database Settings > Drivers & Hardware reads
# (/usr/share/bayu/drivers.db, packaging/kernel/drivers.db): from a kernel
# build, what each module's devices need that a package of the repositories
# brings, as Ubuntu's driver detection finds it from its packages' modaliases.
# AlmaLinux's firmware packages name no modalias, so the kernel tells:
#   firmware <module> <file> <package>       a firmware file a module loads
#   module <module> <package>                a module outside kernel-modules
#   alias <modalias> <module>                the devices of such a module
#   proprietary <modalias> <name> <package>...   drivers not in the kernel,
#                                            from proprietary-drivers
# Fields are tab-separated. Runs in the builder (dnf and the repositories).
#   driver-db.sh KERNEL_RPMS_DIR > drivers.db
set -Eeuo pipefail
ROOT=$(realpath "$(dirname "$0")/../..")
KERNEL_DIR=$(realpath "$1")

podman run --rm -v "$KERNEL_DIR":/kernel:ro,Z -v "$ROOT/packaging/kernel":/src:ro,Z bayu-builder bash -euc '
  cd /tmp
  for rpm in /kernel/kernel-modules*.rpm; do
    # Not internal: the tests of the kernel.
    case $rpm in *-devel-*|*-debug-*|*-matched-*|*-internal-*) continue ;; esac
    name=$(rpm -qp --qf "%{NAME}" "$rpm")
    mkdir -p "m/$name" && (cd "m/$name" && rpm2archive - < "$rpm" | tar xz --wildcards "*.ko*" 2>/dev/null || true)
  done

  # The firmware files of the repositories and their packages.
  for p in $(dnf -q repoquery --qf "%{name}\n" "*firmware*" 2>/dev/null | grep . | sort -u); do
    dnf -q repoquery -l "$p" 2>/dev/null | sed -n "s|^/usr/lib/firmware/||p" |
      sed -E "s/\.(xz|zst)$//" | sed "s|$|\t$p|"
  done | sort -u > firmware-files

  echo "# Bayu driver database, from $(basename "$(ls /kernel/kernel-core-*.rpm | head -1)" .rpm)"
  echo "# Written by packaging/kernel/driver-db.sh; see there."
  for package in m/*; do
    name=${package#m/}
    find "$package" -name "*.ko*" | sort | while read -r ko; do
      module=$(basename "$ko"); module=${module%%.ko*}; module=${module//-/_}
      modinfo -F firmware "$ko" 2>/dev/null | while read -r file; do
        owner=$(awk -F "\t" -v f="$file" "\$1 == f {print \$2; exit}" firmware-files)
        if [ -n "$owner" ]; then printf "firmware\t%s\t%s\t%s\n" "$module" "$file" "$owner"; fi
      done
      case $name in kernel-modules|kernel-modules-core) continue ;; esac
      printf "module\t%s\t%s\n" "$module" "$name"
      modinfo -F alias "$ko" 2>/dev/null | grep -v "^of:\|^acpi\*" | while read -r alias; do
        printf "alias\t%s\t%s\n" "$alias" "$module"
      done
    done
  done | sort -u
  grep -v "^#" /src/proprietary-drivers | grep . | sed "s/^/proprietary\t/"
'
