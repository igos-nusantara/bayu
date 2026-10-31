#!/usr/bin/env bash
# Tests a kernel build before it is published: what Bayu needs of it.
#   1. its config: binder and binderfs built in, PSI on by default (lapis);
#   2. every wireless driver of wireless-modules.txt is there (Realtek's
#      rtw88/rtw89/rtl8xxxu, brcmfmac, iwlwifi, ath, mt76...);
#   3. wl, the Broadcom driver of older MacBooks, builds for it from akmod-wl,
#      and NVIDIA's from RPM Fusion's akmod (Settings offers it);
#   4. it boots in QEMU, where binderfs mounts, /proc/pressure is there, the
#      Wi-Fi stack makes radios (mac80211_hwsim), and wl and the other
#      drivers load.
# Any failure ends it with a non-zero status, so nothing is published.
#   test-kernel.sh KERNEL_RPMS_DIR WL_RPMS_DIR
set -Eeuo pipefail
ROOT=$(realpath "$(dirname "$0")/../..")
KERNEL_DIR=$(realpath "$1")
WL_DIR=$(realpath "$2")
work=$(mktemp -d) && trap 'rm -rf "$work"' EXIT
cp "$ROOT/packaging/kernel/wireless-modules.txt" "$work/"

# 1-3, and the initramfs of 4, in the builder.
podman run --rm --network host -v "$KERNEL_DIR":/kernel:ro,Z -v "$WL_DIR":/wl:ro,Z -v "$work":/work:Z bayu-builder bash -euc '
  fail() { echo "FAIL: $*"; exit 1; }
  cd /kernel
  core=$(ls kernel-core-*.rpm | head -1)
  kver=$(rpm -qp --qf "%{VERSION}-%{RELEASE}.%{ARCH}" "$core")
  echo "kernel $kver"
  echo "$kver" > /work/kver
  # modules-internal holds mac80211_hwsim; a build without it skips that check.
  rpm -i --nodeps --noscripts --excludedocs kernel-core-*.rpm kernel-modules-core-*.rpm kernel-modules-[0-9]*.rpm \
    kernel-modules-extra-[0-9]*.rpm kernel-devel-[0-9]*.rpm $(ls kernel-modules-internal-[0-9]*.rpm 2>/dev/null)
  mods=/lib/modules/$kver
  cp "$mods/vmlinuz" /work/vmlinuz

  config=$mods/config
  for option in CONFIG_ANDROID_BINDER_IPC=y CONFIG_ANDROID_BINDERFS=y; do
    grep -qx "$option" "$config" || fail "config: $option"
  done
  grep -qx "CONFIG_PSI=y" "$config" || fail "config: CONFIG_PSI=y"
  grep -qx "CONFIG_PSI_DEFAULT_DISABLED=y" "$config" && fail "config: PSI is off by default"
  echo "ok: config (binder, binderfs, PSI)"

  depmod "$kver"
  missing=0
  while read -r module; do
    [ -z "$module" ] || [ "${module#\#}" != "$module" ] && continue
    modinfo -k "$kver" -F filename "$module" >/dev/null 2>&1 || { echo "missing module: $module"; missing=1; }
  done < /work/wireless-modules.txt
  [ $missing = 0 ] || fail "wireless drivers missing"
  echo "ok: $(grep -cv "^#" /work/wireless-modules.txt) wireless drivers"

  # MacBook Pro 2016-2017 (issue #104): applespi, and the CS8409 with the Apple code.
  modinfo -k "$kver" -F filename applespi >/dev/null 2>&1 || fail "applespi missing"
  cs8409=$(modinfo -k "$kver" -F filename snd-hda-codec-cs8409 2>/dev/null) || fail "snd-hda-codec-cs8409 missing"
  xzcat -f "$cs8409" | grep -aq "NOT FOUND trying APPLE" || fail "snd-hda-codec-cs8409 without the MacBook Pro code"
  echo "ok: MacBook Pro keyboard, touchpad and sound"

  dnf -y -q install kmodtool akmods elfutils-libelf-devel >/tmp/dnf.log 2>&1 || { tail /tmp/dnf.log; fail "akmods"; }
  rpm -i --nodeps /wl/akmod-wl-*.rpm
  srpm=$(ls /usr/src/akmods/wl-kmod-*.src.rpm | head -1)
  # As akmods builds it on a machine: akmodsbuild refuses root.
  useradd -m builder
  mkdir -p /tmp/wl && chown builder /tmp/wl
  su builder -c "/usr/sbin/akmodsbuild --kernels $kver --outputdir /tmp/wl $srpm" >/tmp/wl.log 2>&1 ||
    { tail -30 /tmp/wl.log; fail "wl does not build"; }
  rpm2cpio /tmp/wl/kmod-wl-*.rpm | (cd / && cpio -idm --quiet)
  wl=$(ls $mods/extra/wl/wl.ko* | head -1)
  modinfo -F vermagic "$wl" | grep -q "^$kver " || fail "wl vermagic: $(modinfo -F vermagic "$wl")"
  echo "ok: wl builds ($(basename "$wl"))"

  # NVIDIA, which Settings > Drivers & Hardware offers: RPM Fusion akmod
  # from the IGN mirror builds for it too.
  printf "[igos-rpmfusion]\nname=rpmfusion\nbaseurl=https://r2.igos-nusantara.or.id/26/rpmfusion/\ngpgcheck=0\n" \
    > /etc/yum.repos.d/igos-rpmfusion.repo
  dnf -y -q install pciutils >/dev/null 2>&1 || fail "pciutils"
  dnf -y -q download --destdir /tmp/nvidia akmod-nvidia-580xx xorg-x11-drv-nvidia-580xx-kmodsrc >/tmp/dnf.log 2>&1 ||
    { tail /tmp/dnf.log; fail "NVIDIA akmod download"; }
  rpm -i --nodeps /tmp/nvidia/*.rpm
  mkdir -p /tmp/nvidia-out && chown builder /tmp/nvidia-out
  srpm=$(ls /usr/src/akmods/nvidia-*-kmod-*.src.rpm | head -1)
  su builder -c "/usr/sbin/akmodsbuild --kernels $kver --outputdir /tmp/nvidia-out $srpm" >/tmp/nvidia.log 2>&1 ||
    { tail -30 /tmp/nvidia.log; fail "NVIDIA does not build"; }
  echo "ok: NVIDIA builds ($(basename "$srpm" .src.rpm))"
  depmod "$kver"

  # The boot check: run by dracut before udev, its verdict on the console.
  cat > /tmp/bayu-check.sh <<"CHECK"
#!/bin/sh
result() { echo "BAYU-CHECK $*" > /dev/console; }
result "kernel $(uname -r)"
mkdir -p /dev/binderfs
if mount -t binder binder /dev/binderfs && [ -e /dev/binderfs/binder-control ]; then result "ok binderfs"; else result "FAIL binderfs"; fi
if [ -r /proc/pressure/memory ]; then result "ok psi"; else result "FAIL psi"; fi
if modprobe mac80211_hwsim radios=2 && [ -e /sys/class/ieee80211/phy0 ]; then result "ok wifi-stack"; else result "FAIL wifi-stack"; fi
for module in wl rtw89_8852be rtw89_8852ae rtw88_8822ce rtw88_8821ce rtl8xxxu brcmfmac iwlwifi ath9k ath10k_pci ath11k_pci mt7921e \
    rtw88_8821cu rtw88_8822cu rtw88_8812au rtw89_8852bu mt7921u mt7925u rt2800usb ath9k_htc; do
  if modprobe "$module" 2>/dev/null; then result "ok load $module"; else result "FAIL load $module"; fi
done
result "done"
echo o > /proc/sysrq-trigger
CHECK
  chmod +x /tmp/bayu-check.sh
  # What the build has: a missing one fails its load at the boot instead of
  # stopping dracut.
  drivers=""
  for module in cfg80211 mac80211 mac80211_hwsim wl rtw89_8852be rtw89_8852ae rtw88_8822ce rtw88_8821ce rtl8xxxu \
                brcmfmac iwlwifi ath9k ath10k_pci ath11k_pci mt7921e rtw88_8821cu rtw88_8822cu rtw88_8812au \
                rtw89_8852bu mt7921u mt7925u rt2800usb ath9k_htc; do
    modinfo -k "$kver" "$module" >/dev/null 2>&1 && drivers="$drivers $module"
  done
  dracut -q --no-hostonly --no-hostonly-cmdline --kver "$kver" --force \
    --add-drivers "$drivers" \
    --install "mount mkdir modprobe" \
    --include /tmp/bayu-check.sh /usr/lib/dracut/hooks/pre-udev/00-bayu-check.sh \
    /work/initramfs.img 2>&1 | grep -v -i "warning\|cannot find" || true
  [ -s /work/initramfs.img ] || fail "initramfs"
  echo "ok: initramfs"
'

# 4: the boot.
kver=$(cat "$work/kver")
accel=tcg
[ -w /dev/kvm ] && accel=kvm
timeout 300 qemu-system-x86_64 -machine q35,accel=$accel -cpu max -m 2G -smp 2 -nographic -no-reboot \
  -kernel "$work/vmlinuz" -initrd "$work/initramfs.img" \
  -append "console=ttyS0 panic=-1 sysrq_always_enabled=1 rd.shell=0 quiet" > "$work/serial.log" 2>&1 || true
grep "BAYU-CHECK" "$work/serial.log" | sed 's/^.*BAYU-CHECK /boot: /'
grep -q "BAYU-CHECK done" "$work/serial.log" || { echo "FAIL: the boot did not finish"; tail -20 "$work/serial.log"; exit 1; }
grep -q "BAYU-CHECK kernel $kver" "$work/serial.log" || { echo "FAIL: booted another kernel"; exit 1; }
if grep -q "BAYU-CHECK FAIL" "$work/serial.log"; then
  echo "FAIL: $kver"
  exit 1
fi
echo "PASS: $kver"
