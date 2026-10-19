#!/bin/sh
# Install the GC5024 webcam for boot:
#   - gc5024 sensor driver as a DKMS module
#   - patched ipu-bridge / intel_skl_int3472_tps68470 as a DKMS module
#   - modprobe and modules-load configuration (1080p, exclusive_caps=0)
#   - gc5024-webcam.service, the system service running the userspace ISP
#
# Run as your normal user; sudo is used for the privileged steps.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
KVER=$(uname -r)
GC5024_VER=1.0.0
BRIDGE_VER=1.0.0
SERVICE_USER=${SERVICE_USER:-${SUDO_USER:-$(id -un)}}

say() { printf '%s\n' "$*" >&2; }

if [ "$(id -u)" = 0 ]; then
	say "run this as your normal user (sudo is used internally)"
	exit 1
fi

say "building userspace ISP"
make -C "$ROOT" camera

say "installing gc5024 DKMS package"
sudo rm -rf "/usr/src/gc5024-$GC5024_VER"
sudo install -d "/usr/src/gc5024-$GC5024_VER/src"
sudo install -m644 "$ROOT/src/gc5024.c" "$ROOT/src/gc5024_regs.h" \
	"$ROOT/src/Makefile" "/usr/src/gc5024-$GC5024_VER/src/"
sudo install -m644 "$ROOT/dkms/gc5024/dkms.conf" \
	"/usr/src/gc5024-$GC5024_VER/dkms.conf"
sudo dkms remove "gc5024/$GC5024_VER" --all 2>/dev/null || true
sudo dkms add "/usr/src/gc5024-$GC5024_VER"
sudo dkms build "gc5024/$GC5024_VER"
sudo dkms install "gc5024/$GC5024_VER"

say "installing patched IPU3 bridge DKMS package"
sudo rm -rf "/usr/src/ipu-bridge-gc5024-$BRIDGE_VER"
sudo install -d "/usr/src/ipu-bridge-gc5024-$BRIDGE_VER"
sudo cp -a "$ROOT/dkms/ipu-bridge-gc5024/." \
	"/usr/src/ipu-bridge-gc5024-$BRIDGE_VER/"
sudo dkms remove "ipu-bridge-gc5024/$BRIDGE_VER" --all 2>/dev/null || true
# Older manual installs lived directly in updates/; DKMS installs to
# updates/dkms and refuses to overwrite them.
for m in ipu-bridge intel_skl_int3472_tps68470; do
	sudo rm -f "/lib/modules/$KVER/updates/$m.ko" \
		"/lib/modules/$KVER/updates/$m.ko.zst"
done
sudo dkms add "/usr/src/ipu-bridge-gc5024-$BRIDGE_VER"
sudo dkms build "ipu-bridge-gc5024/$BRIDGE_VER"
sudo dkms install "ipu-bridge-gc5024/$BRIDGE_VER"

say "installing module configuration"
sudo install -m644 "$ROOT/dkms/modprobe-gc5024.conf" \
	/etc/modprobe.d/gc5024.conf
sudo install -m644 "$ROOT/dkms/modprobe-v4l2loopback.conf" \
	/etc/modprobe.d/v4l2loopback.conf
sudo install -m644 "$ROOT/dkms/modules-load-gc5024.conf" \
	/etc/modules-load.d/gc5024.conf
sudo install -m644 "$ROOT/dkms/modules-load-v4l2loopback.conf" \
	/etc/modules-load.d/v4l2loopback.conf

say "installing gc5024-webcam.service"
tmp=$(mktemp)
sed -e "s|@USER@|$SERVICE_USER|g" -e "s|@ROOT@|$ROOT|g" \
	"$ROOT/dkms/gc5024-webcam.service.in" > "$tmp"
sudo install -m644 "$tmp" /etc/systemd/system/gc5024-webcam.service
rm -f "$tmp"

say "reloading modules"
sudo systemctl stop gc5024-webcam.service 2>/dev/null || true
pkill -x gc5024-camera 2>/dev/null || true

if lsmod | grep -q '^gc5024 '; then
	echo i2c-GCTI5024:00 |
		sudo tee /sys/bus/i2c/drivers/gc5024/unbind >/dev/null 2>&1 || true
	if ! sudo modprobe -r gc5024; then
		say "warning: could not unload gc5024; reboot to use the DKMS build"
	fi
fi
if lsmod | grep -q '^v4l2loopback '; then
	sudo modprobe -r v4l2loopback 2>/dev/null || true
fi

sudo depmod -A
sudo modprobe gc5024
sudo modprobe v4l2loopback

say "enabling service"
sudo systemctl daemon-reload
sudo systemctl enable --now gc5024-webcam.service

sleep 2
sudo systemctl --no-pager --full status gc5024-webcam.service || true

say ""
say "done.  The webcam is /dev/video20 (\"GC5024 Camera\")."
say "Check:  systemctl status gc5024-webcam"
say "Logs:   journalctl -u gc5024-webcam -f"
