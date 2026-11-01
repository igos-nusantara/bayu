# Bayu's build entry points: each target runs the script that does the work
# (the same ones, in the same order, as .github/workflows/release.yml).
# docs/building.md explains them. `make` alone lists the targets.

PROFILE    ?= igos
STAGE      ?= out/stage
COMPONENTS ?=
NAME       := $(shell sed -n 's/^IMAGE_NAME=//p' builder/profiles/$(PROFILE)/profile.conf)
ISO        ?= out/$(NAME)-live.iso
IMAGE      ?= out/$(NAME).qcow2
SCENARIOS  ?=

export PROFILE

# What packaging/kernel/build-kernel.sh and packaging/wl/build-wl.sh build,
# for test-kernel.sh to find in their caches.
KERNEL_BUILD = $(shell sed -n 's/^KERNEL=$${KERNEL:-\(.*\)}$$/\1/p' packaging/kernel/build-kernel.sh).$(shell sed -n 's/^BUILD=$${BUILD:-\(.*\)}$$/\1/p' packaging/kernel/build-kernel.sh)
WL_RELEASE   = $(shell sed -n 's/^WL=$${WL:-\(.*\)}$$/\1/p' packaging/wl/build-wl.sh).$(shell sed -n 's/^WL_BUILD=$${WL_BUILD:-\(.*\)}$$/\1/p' packaging/wl/build-wl.sh)

.PHONY: help all desktop rpm kernel kernel-test lapis packages sign iso image vm iso-vm serve e2e clean

help:
	@echo "Bayu build targets (docs/building.md):"
	@echo "  make desktop       components into STAGE=$(STAGE), for development (COMPONENTS=\"...\")"
	@echo "  make rpm           bayu-desktop and nabau RPMs into out/repo"
	@echo "  make kernel        Bayu kernel, wl and usb_modeswitch into out/repo"
	@echo "  make kernel-test   test the kernel build (config, drivers, boot in QEMU)"
	@echo "  make lapis         lapis's latest release into out/repo"
	@echo "  make packages      rpm, kernel, kernel-test and lapis"
	@echo "  make sign          sign out/repo with the IGOS key (BAYU_REPO_GPG_KEY)"
	@echo "  make iso           live ISO of PROFILE=$(PROFILE): $(ISO)"
	@echo "  make image         disk image of PROFILE=$(PROFILE): $(IMAGE)"
	@echo "  make all           packages, then the ISO"
	@echo "  make vm            boot the disk image in QEMU, over VNC"
	@echo "  make iso-vm        boot the ISO in QEMU with UEFI, as a Mac would"
	@echo "  make serve         serve out/repo to running systems"
	@echo "  make e2e           installer end-to-end tests on the ISO (SCENARIOS=\"...\")"
	@echo "  make clean         remove build/ and out/"

all: packages iso

desktop:
	STAGE=$(STAGE) $(if $(COMPONENTS),COMPONENTS="$(COMPONENTS)") ./build.sh

rpm:
	packaging/build-rpm.sh

kernel:
	packaging/kernel/build-kernel.sh
	packaging/wl/build-wl.sh
	packaging/usb-modeswitch/build-usb-modeswitch.sh

# A pass is kept with the build, so a kernel is tested once, as in releases.
kernel-test:
	@stamp=$(HOME)/.cache/bayu-kernel/$(KERNEL_BUILD)/tested-wl-$(WL_RELEASE); \
	if [ -f "$$stamp" ]; then echo "$(KERNEL_BUILD) with wl $(WL_RELEASE) passed before"; \
	else packaging/kernel/test-kernel.sh $(HOME)/.cache/bayu-kernel/$(KERNEL_BUILD) \
	  $(HOME)/.cache/bayu-wl/$(WL_RELEASE) && touch "$$stamp"; fi

lapis:
	packaging/fetch-lapis.sh

packages: rpm kernel kernel-test lapis

sign:
	builder/sign-rpms.sh

iso:
	builder/build-iso.sh

image:
	builder/build-image.sh

vm:
	builder/run-vm.sh $(IMAGE)

iso-vm:
	UEFI=yes builder/run-vm.sh $(ISO)

serve:
	packaging/serve-repo.sh

e2e:
	tests/installer/e2e.py --iso $(ISO) $(SCENARIOS)

# The builder's chroots are mounted inside its container only; stay on this
# file system all the same.
clean:
	rm -rf --one-file-system build out
