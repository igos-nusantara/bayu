# Building Bayu

Everything Bayu ships is built from this repository with `make`. Each target
runs one of the scripts in `build.sh`, `packaging/` and `builder/`; the
release workflow (`.github/workflows/release.yml`) runs the same scripts in
the same order, so a local build is what a release builds.

```shell
make            # lists the targets
make all        # every package, then the live ISO
```

## Requirements

- A Linux host with **Podman**. The packages and images are built inside an
  AlmaLinux 10 container (`packaging/Containerfile`, image `bayu-builder`),
  so the host distribution does not matter.
- **sudo** for the ISO and disk image (loop devices, a privileged container).
- **KVM** and QEMU to boot them (`make vm`, `make iso-vm`, `make e2e`,
  `make kernel-test`).
- Plenty of disk space: the kernel build is the largest.

The first build downloads and compiles Skia (for `bayu-toolkit`) and takes a
while; later builds reuse it.

## Outputs

| Path | What |
|---|---|
| `out/rpms/` | `bayu-desktop`, `nabau` .rpm and .src.rpm |
| `out/repo/` | a dnf repository of everything the system installs from this build: the RPMs above, the kernel, `wl`, `usb_modeswitch`, lapis |
| `out/igos-bayu-live.iso` | the live ISO (`PROFILE=almalinux`: `out/alma-bayu-live.iso`) |
| `out/igos-bayu.qcow2` | the disk image |
| `build/`, `out/stage/` | the development build of `make desktop` |
| `~/.cache/bayu-kernel/`, `~/.cache/bayu-wl/`, ... | kernel, wl and usb_modeswitch builds, reused until their version changes |

## Targets

### Packages

| Target | Runs | Does |
|---|---|---|
| `make rpm` | `packaging/build-rpm.sh` | Builds the `bayu-builder` container and the `bayu-desktop` and `nabau` RPMs into `out/rpms` and `out/repo`. A `vX.Y.Z` tag on HEAD gives version X.Y.Z-1; otherwise a dated snapshot of the last tag. |
| `make kernel` | `packaging/kernel/build-kernel.sh`, `packaging/wl/build-wl.sh`, `packaging/usb-modeswitch/build-usb-modeswitch.sh` | AlmaLinux's kernel rebuilt with `packaging/kernel/kernel-local` and `cs8409-macbookpro.patch`; RPM Fusion's `wl` for it; Fedora's `usb_modeswitch`. |
| `make kernel-test` | `packaging/kernel/test-kernel.sh` | Checks the kernel build before anything uses it: its config (binder, PSI), every driver of `wireless-modules.txt`, MacBook Pro keyboard and sound, that `wl` builds, and that it boots in QEMU and loads them. A pass is remembered next to the build. |
| `make lapis` | `packaging/fetch-lapis.sh` | Adds the latest lapis release (Android apps), checked against its `SHA256SUMS`. `packaging/fetch-lapis.sh 0.7.0` takes a given version. |
| `make packages` | | `rpm`, `kernel`, `kernel-test` and `lapis`. |
| `make sign` | `builder/sign-rpms.sh` | Signs `out/rpms` and `out/repo` with the IGOS repository key, from `BAYU_REPO_GPG_KEY`. Only releases need it. |

### System

| Target | Runs | Does |
|---|---|---|
| `make iso` | `builder/build-iso.sh` | The live ISO of `PROFILE` (`igos` by default, or `almalinux`), from `out/repo` and the profile's repositories. It boots on BIOS and UEFI, from a DVD or a USB stick written with `dd`, and carries its own checksum for the installer's media check. |
| `make image` | `builder/build-image.sh` | A bootable disk image of `PROFILE`. |
| `make all` | | `packages`, then `iso`. |

### Running and testing

| Target | Runs | Does |
|---|---|---|
| `make vm` | `builder/run-vm.sh` | Boots the disk image, screen over VNC on the Tailscale address (`VNC_ADDR`, `VNC_DISPLAY`). |
| `make iso-vm` | `UEFI=yes builder/run-vm.sh` | Boots the live ISO with UEFI firmware, as a Mac would. |
| `make serve` | `packaging/serve-repo.sh` | Serves `out/repo` over HTTP (port 8090), so running systems take new builds with `sudo dnf upgrade --refresh` instead of a new image. |
| `make e2e` | `tests/installer/e2e.py` | Installs the ISO in VMs through the installer's window and checks what it installed; `SCENARIOS="blank luks"` picks scenarios (`tests/installer/e2e.py --list`). See `tests/installer/README.md`. |

### Development

| Target | Runs | Does |
|---|---|---|
| `make desktop` | `build.sh` | Builds the components on this host into `STAGE` (`out/stage`), each finding the ones before it there; `COMPONENTS="terminal settings"` builds only those. Needs their build dependencies on the host: on AlmaLinux 10, `sudo dnf builddep packaging/bayu-desktop.spec`. |
| `make clean` | | Removes `build/` and `out/`; the caches in `~/.cache` stay. |

## Variables

| Variable | Default | For |
|---|---|---|
| `PROFILE` | `igos` | `iso`, `image`, `vm`, `iso-vm`, `e2e`: the profile in `builder/profiles/` |
| `ISO`, `IMAGE` | from the profile's `IMAGE_NAME` | the file `vm`, `iso-vm` and `e2e` use |
| `STAGE`, `COMPONENTS` | `out/stage`, all | `desktop` |
| `SCENARIOS` | all | `e2e` |
| `CONFIG`, `REPOS` | the profile's | another `profile.conf` or `repos.d` for `iso` and `image` |
| `KERNEL`, `BUILD` | in `build-kernel.sh` | `kernel`: another AlmaLinux kernel, or Bayu's build number of it |
| `ALMA_MIRROR`, `EPEL_MIRROR` | | `rpm`: mirrors for the build container |
| `BAYU_REPO_GPG_KEY` | | `sign` |

Variables go on the command line (`make iso PROFILE=almalinux`) or in the
environment; the scripts read the same names.

## Changing the kernel

- A config option: add it to `packaging/kernel/kernel-local`, which the
  kernel spec merges over AlmaLinux's config.
- A source change: `packaging/kernel/cs8409-macbookpro.patch` is applied by
  the kernel spec as `linux-kernel-test.patch`; changes go into it, as one
  patch with `a/` and `b/` paths from the kernel tree.
- Either way, raise `BUILD` in `build-kernel.sh`: the build is cached by
  kernel version and `BUILD`, and the new release must sort above the last.
- Add a check for it to `packaging/kernel/test-kernel.sh`, then
  `make kernel kernel-test`.
- `make kernel` writes the build's whole config to
  `packaging/kernel/config-x86_64`, and its driver database to
  `packaging/kernel/drivers.db` (`driver-db.sh`: the firmware each module
  loads and its package, the modules outside `kernel-modules`, and the
  proprietary drivers of `proprietary-drivers`, as NVIDIA's), which Settings >
  Drivers & Hardware reads; commit both with the change, so the difference to
  the last kernel shows in the diff.

A workflow opens a pull request when AlmaLinux publishes a newer kernel,
after building and testing it.

## Releases

A `vX.Y.Z` tag starts the release workflow on the self-hosted runner. It runs
`make packages`, `make sign` and `make iso` in effect, then:

- publishes the RPMs as a GitHub release;
- uploads the ISO to R2 as `iso/nightly/ign26-bayu-harian-<timestamp>.iso`
  and `ign26-bayu-harian-latest.iso` (`builder/publish-nightly.sh`);
- adds the RPMs to the Bayu dnf repository, `repo/26/bayu` on R2, which the
  IGOS mirror host then pulls (`builder/publish-repo.sh`).

Publishing needs the R2 credentials and the signing key, which only the
workflow has.

IGOS Nusantara's own packages (`ign-release`, `ign-logos`, `ign-config`...)
are built and published from
[igos-nusantara/ign-packages](https://github.com/igos-nusantara/ign-packages)
with its `build.sh` and `publish.sh`.
