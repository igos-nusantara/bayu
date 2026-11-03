#!/usr/bin/env bash
# Builds eplq-cups, Epson's driver for its LQ-2190 and other LQ dot-matrix
# printers (issue #165), from Epson's download into out/repo. A build is kept
# in EPLQ_CACHE and reused: releases only rebuild it when the spec changes.
#   EPLQ_CACHE=~/.cache/bayu-eplq-cups
set -Eeuo pipefail
ROOT=$(realpath "$(dirname "$0")/../..")
SPEC=$ROOT/packaging/eplq-cups/eplq-cups.spec
URL=https://download.epson-europe.com/pub/download/3273/epson327368eu.zip
SHA256=d815a2c0037c2a5beac9f21dd8a669e50afaee69a90caea240616f44369d6b5c
CACHE=${EPLQ_CACHE:-$HOME/.cache/bayu-eplq-cups}/$(sha256sum "$SPEC" | cut -c1-12)
mkdir -p "$CACHE" "$ROOT/out/repo"

if ! compgen -G "$CACHE/eplq-cups-*.rpm" >/dev/null; then
  work=$(mktemp -d) && trap 'rm -rf --one-file-system "$work"' EXIT
  curl -fL -o "$work/epson327368eu.zip" "$URL"
  echo "$SHA256  $work/epson327368eu.zip" | sha256sum -c -
  cp "$SPEC" "$work/"
  podman run --rm -v "$work":/work:Z bayu-builder bash -euc '
    dnf -y -q install unzip cpio >/dev/null
    rpmbuild -bb /work/eplq-cups.spec --define "_topdir /tmp/top" --define "_sourcedir /work" --define "dist .el10.bayu"
    cp /tmp/top/RPMS/*/*.rpm /work/'
  cp "$work"/eplq-cups-*.x86_64.rpm "$CACHE/"
fi
cp "$CACHE"/*.rpm "$ROOT/out/repo/"
podman run --rm -v "$ROOT/out/repo":/repo:Z bayu-builder createrepo_c -q --update /repo
ls -l "$CACHE"
