#!/usr/bin/env bash
# Makes the target disk of an installer test scenario (tests/installer/e2e.py).
# Windows is a stand-in made here, not an installation: its partitions, file
# systems, boot files and folders, which is what the installer, os-prober and
# GRUB look at. Needs root (sudo), qemu-nbd, sgdisk, mkntfs, mkfs.exfat.
#   mkdisk.sh SCENARIO OUT.qcow2 [SIZE]
# Scenarios:
#   blank        an empty disk (SIZE, default 40G)
#   win-full     Windows (GPT, UEFI) filling 80 GiB: "install alongside" shrinks it
#   win-free     Windows 37 GiB, 20 GiB free, exFAT data on the rest (issue #26)
#   win-esp200   as win-free with a 200 MiB EFI system partition
#   win-esp-full as win-free with its EFI system partition all but full, as
#                vendors leave it: the boot loader needs a new one
#   win-mbr      Windows on an MBR disk (BIOS): System Reserved, Windows, free space
set -Eeuo pipefail
scenario=$1 out=$2 size=${3:-}
GiB=$((1024 * 1024 * 1024))

case $scenario in
blank) qemu-img create -q -f qcow2 "$out" "${size:-40G}"; exit 0 ;;
win-*) qemu-img create -q -f qcow2 "$out" "${size:-80G}" ;;
triple-mid) qemu-img create -q -f qcow2 "$out" "${size:-120G}" ;;
arch-*) qemu-img create -q -f qcow2 "$out" "${size:-60G}" ;;
*) echo "unknown scenario $scenario" >&2; exit 1 ;;
esac

sudo modprobe nbd max_part=16
# A free device: one left by a killed run may hang.
for dev in /dev/nbd{15..0}; do [[ -e /sys/block/${dev#/dev/}/pid ]] || break; done
sudo qemu-nbd -c "$dev" "$out"
mnt=$(mktemp -d)
trap 'sudo umount -q "$mnt" 2>/dev/null || true; rmdir "$mnt"; sync; sudo qemu-nbd -d "$dev" >/dev/null' EXIT
until [[ $(sudo blockdev --getsize64 "$dev") != 0 ]]; do sleep 0.2; done

part() { sudo partprobe "$dev"; sudo udevadm settle; echo "${dev}p$1"; }
put() { # put DEVICE FILE...: empty files (and their folders) on a file system
  local d=$1; shift
  sudo mount "$d" "$mnt"
  for f in "$@"; do sudo mkdir -p "$mnt/$(dirname "$f")"; sudo touch "$mnt/$f"; done
  sudo umount "$mnt"
}
windows() { # windows NTFS-PARTITION: Windows' folders, and some data to keep
  sudo mkntfs -Q -q -L Windows "$1" 2>/dev/null
  sudo mount "$1" "$mnt"
  sudo mkdir -p "$mnt/Windows/System32" "$mnt/Users/Public" "$mnt/Program Files"
  sudo dd if=/dev/urandom of="$mnt/Users/Public/data.bin" bs=1M count=64 status=none
  sudo umount "$mnt"
}

if [[ $scenario == arch-* ]]; then
  # Arch Linux before the free space, a stand-in as Windows is: its partitions,
  # boot files and the files os-prober reads. arch-grub boots GRUB with the
  # kernel in the root's /boot; arch-sdboot is archinstall's default,
  # systemd-boot with the EFI partition mounted at /boot and the kernel on it.
  sudo sgdisk -n 1:2048:+1G -t 1:EF00 -c "1:EFI system partition" \
    -n 2:0:+25G -t 2:8304 -c 2:root "$dev" >/dev/null
  sudo mkfs.vfat -F 32 "$(part 1)" >/dev/null
  sudo mkfs.ext4 -q -L arch "$(part 2)"
  root=$(sudo blkid -s UUID -o value "$(part 2)")
  esp=$(sudo blkid -s UUID -o value "$(part 1)")
  sudo mount "$(part 2)" "$mnt"
  sudo mkdir -p "$mnt"/{etc,usr/lib,boot,home/user}
  printf 'NAME="Arch Linux"\nPRETTY_NAME="Arch Linux"\nID=arch\nBUILD_ID=rolling\n' |
    sudo tee "$mnt/usr/lib/os-release" "$mnt/etc/os-release" >/dev/null
  sudo dd if=/dev/urandom of="$mnt/home/user/data.bin" bs=1M count=32 status=none
  if [[ $scenario == arch-grub ]]; then
    printf 'UUID=%s / ext4 rw,relatime 0 1\nUUID=%s /boot/efi vfat rw 0 2\n' "$root" "$esp" | sudo tee "$mnt/etc/fstab" >/dev/null
    sudo mkdir -p "$mnt/boot/grub" "$mnt/boot/efi"
    sudo touch "$mnt/boot/vmlinuz-linux" "$mnt/boot/initramfs-linux.img" "$mnt/boot/initramfs-linux-fallback.img"
    printf 'menuentry "Arch Linux" {\n  linux /boot/vmlinuz-linux root=UUID=%s rw\n  initrd /boot/initramfs-linux.img\n}\n' "$root" |
      sudo tee "$mnt/boot/grub/grub.cfg" >/dev/null
    sudo umount "$mnt"
    put "$(part 1)" EFI/GRUB/grubx64.efi EFI/BOOT/BOOTX64.EFI
  else
    printf 'UUID=%s / ext4 rw,relatime 0 1\nUUID=%s /boot vfat rw 0 2\n' "$root" "$esp" | sudo tee "$mnt/etc/fstab" >/dev/null
    sudo umount "$mnt"
    put "$(part 1)" EFI/systemd/systemd-bootx64.efi EFI/BOOT/BOOTX64.EFI vmlinuz-linux initramfs-linux.img \
      initramfs-linux-fallback.img loader/loader.conf
    sudo mount "$(part 1)" "$mnt"
    sudo mkdir -p "$mnt/loader/entries"
    printf 'title Arch Linux\nlinux /vmlinuz-linux\ninitrd /initramfs-linux.img\noptions root=UUID=%s rw\n' "$root" |
      sudo tee "$mnt/loader/entries/arch.conf" >/dev/null
    sudo umount "$mnt"
  fi
  sudo sgdisk -p "$dev" | tail -n +10
  exit 0
fi

if [[ $scenario == triple-mid ]]; then
  # As the disk of issue #65: Windows, another Linux after it, 27 GiB free in
  # the middle, then two exFAT data partitions.
  sudo sgdisk -n 1:2048:+100M -t 1:EF00 -c "1:EFI system partition" \
    -n 2:0:+16M -t 2:0C01 -c "2:Microsoft reserved partition" \
    -n 3:0:+37G -t 3:0700 -c "3:Basic data partition" \
    -n 4:0:+10G -t 4:8300 "$dev" >/dev/null
  end=$(sudo sgdisk -i 4 "$dev" | awk '/Last sector/ {print $3}')
  sudo sgdisk -n "5:$((end + 1 + 27 * GiB / 512)):+20G" -t 5:0700 "$dev" >/dev/null
  # After the data partition, not in the free space before it, where the first
  # free sector would put it.
  end=$(sudo sgdisk -i 5 "$dev" | awk '/Last sector/ {print $3}')
  sudo sgdisk -n "6:$((end + 1)):$(sudo sgdisk -p "$dev" | awk '/last usable sector/ {print $NF}')" -t 6:0700 -c 6:ntfs "$dev" >/dev/null
  sudo mkfs.vfat -F 32 -n SYSTEM "$(part 1)" >/dev/null
  put "$(part 1)" EFI/Microsoft/Boot/bootmgfw.efi EFI/Microsoft/Boot/BCD EFI/Boot/bootx64.efi
  windows "$(part 3)"
  sudo mkfs.ext4 -q -L IGN26-mate "$(part 4)"
  sudo mkfs.exfat -q -L DATA2 "$(part 5)"
  sudo mkfs.exfat -q -L DATA1 "$(part 6)"
  sudo mount "$(part 6)" "$mnt"
  sudo dd if=/dev/urandom of="$mnt/photos.bin" bs=1M count=64 status=none
  sudo umount "$mnt"
  sudo sgdisk -p "$dev" | tail -n +10
  exit 0
fi

if [[ $scenario == win-mbr ]]; then
  # As Windows sets up a BIOS machine: a 500 MiB System Reserved partition
  # with its boot manager, then Windows; the rest of the disk free.
  sudo sfdisk -q "$dev" <<EOF
label: dos
size=500MiB, type=7, bootable
size=37GiB, type=7
EOF
  sudo mkntfs -Q -q -L "System Reserved" "$(part 1)" 2>/dev/null
  put "$(part 1)" bootmgr Boot/BCD
  windows "$(part 2)"
  sudo sfdisk -l "$dev" | tail -n +8
  exit 0
fi

esp=100M
[[ $scenario == win-esp200 ]] && esp=200M
ntfs=+37G
[[ $scenario == win-full ]] && ntfs=0  # to the end
sudo sgdisk -n "1:2048:+$esp" -t 1:EF00 -c "1:EFI system partition" \
  -n 2:0:+16M -t 2:0C01 -c "2:Microsoft reserved partition" \
  -n "3:0:$ntfs" -t 3:0700 -c "3:Basic data partition" "$dev" >/dev/null
if [[ $scenario != win-full ]]; then
  # 20 GiB left free after Windows, then exFAT data on the rest.
  end=$(sudo sgdisk -i 3 "$dev" | awk '/Last sector/ {print $3}')
  sudo sgdisk -n "4:$((end + 1 + 20 * GiB / 512)):0" -t 4:0700 -c 4:Data "$dev" >/dev/null
  sudo mkfs.exfat -q -L Data "$(part 4)"
  sudo mount "$(part 4)" "$mnt"
  sudo dd if=/dev/urandom of="$mnt/photos.bin" bs=1M count=64 status=none
  sudo umount "$mnt"
fi
sudo mkfs.vfat -F 32 -n SYSTEM "$(part 1)" >/dev/null
put "$(part 1)" EFI/Microsoft/Boot/bootmgfw.efi EFI/Microsoft/Boot/BCD EFI/Boot/bootx64.efi
if [[ $scenario == win-esp-full ]]; then
  # 2 MiB left: the copy of /boot/efi failed at 3% (rsync exit 10).
  sudo mount "$(part 1)" "$mnt"
  free=$(df -B1M --output=avail "$mnt" | tail -1)
  sudo mkdir -p "$mnt/EFI/HP"
  sudo dd if=/dev/urandom of="$mnt/EFI/HP/recovery.bin" bs=1M count=$((free - 2)) status=none
  sudo umount "$mnt"
fi
windows "$(part 3)"
sudo sgdisk -p "$dev" | tail -n +10
