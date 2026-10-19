#!/bin/sh
# Build and install libcamera with IPU3 + GC5024 support
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD_DIR="${TMPDIR:-/tmp}/libcamera-build"

say() { printf '%s\n' "$*" >&2; }

if [ "$(id -u)" = 0 ]; then
	say "run this as your normal user (sudo is used internally)"
	exit 1
fi

say "== Installing dependencies =="
if command -v pacman >/dev/null 2>&1; then
	sudo pacman -S --needed --noconfirm git meson ninja pkgconf gcc \
		python-jinja python-yaml python-ply \
		openssl gnutls libyaml libevent pipewire-media-session || true
elif command -v apt-get >/dev/null 2>&1; then
	sudo apt-get update && sudo apt-get install -y \
		git meson ninja-build pkg-config build-essential \
		python3-jinja2 python3-yaml python3-ply \
		libssl-dev libgnutls28-dev libyaml-dev libevent-dev || true
fi

say "== Cloning libcamera =="
rm -rf "$BUILD_DIR"
git clone --depth 1 https://github.com/libcamera-org/libcamera.git "$BUILD_DIR"

say "== Applying GC5024 + IPU3 tuning patches =="
cd "$BUILD_DIR"
git apply "$ROOT/patches/0003-libcamera-add-GalaxyCore-GC5024-support.patch"

say "== Configuring and building libcamera =="
meson setup build \
	--prefix=/usr \
	--libdir=lib \
	--sysconfdir=/etc \
	-Dpipelines=ipu3,simple \
	-Dipas=ipu3,simple \
	-Dtracing=disabled \
	-Ddocumentation=disabled \
	-Dtest=false \
	-Dpycamera=disabled

ninja -C build

say "== Installing libcamera & IPA tuning files =="
sudo ninja -C build install

if [ -f "$ROOT/tuning/ipu3/gc5024.yaml" ]; then
	sudo install -d /usr/share/libcamera/ipa/ipu3
	sudo install -m644 "$ROOT/tuning/ipu3/gc5024.yaml" /usr/share/libcamera/ipa/ipu3/gc5024.yaml
fi

# Refresh shared library cache
sudo ldconfig

say ""
say "Done! libcamera with Intel IPU3 hardware ISP is installed."
say "You can test the camera with: cam -c 1 -I"
