#!/bin/sh
# Start the GC5024 as a normal virtual webcam.
#
# Creates a v4l2loopback device (default /dev/video20), loads the sensor
# driver in the requested mode, configures the IPU3 pipeline and runs the
# userspace ISP.
#
#   tools/start-webcam.sh                  # native 1920x1080 @ ~30 fps (default)
#   MODE=720p  tools/start-webcam.sh       # native 1280x720 @ ~45 fps
#   MODE=full  tools/start-webcam.sh       # 5 MP readout, ~17.7 fps
#   tools/start-webcam.sh --width 640 --height 480
#   VIDEO=21 tools/start-webcam.sh
#
# MODE_720P=0 / MODE_1080P=0 are still accepted as aliases.
# Stop with Ctrl-C.  Applications see it as the capture device
# "GC5024 Camera" (PipeWire exposes it automatically).
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BIN="$ROOT/apps/gc5024-camera"
VIDEO=${VIDEO:-20}
NODE="/dev/video$VIDEO"

if [ -n "${MODE:-}" ]; then
	:
elif [ "${MODE_720P:-1}" = 0 ]; then
	MODE=full
elif [ "${MODE_1080P:-1}" = 0 ]; then
	MODE=720p
else
	MODE=1080p
fi

if [ ! -x "$BIN" ]; then
	echo "building userspace ISP..." >&2
	make -C "$ROOT" camera >&2
fi

# PipeWire/WirePlumber only creates a camera source for a loopback device
# when it advertises capture capabilities.  exclusive_caps=1 hides them
# until a producer opens the device, so GNOME Camera never sees it.
caps=$(cat /sys/module/v4l2loopback/parameters/exclusive_caps 2>/dev/null |
	cut -d, -f1)
if ! lsmod | grep -q '^v4l2loopback'; then
	sudo modprobe v4l2loopback exclusive_caps=0 \
		card_label="GC5024 Camera" video_nr="$VIDEO"
elif [ "$caps" != "N" ]; then
	echo "reloading v4l2loopback with exclusive_caps=0" >&2
	if sudo modprobe -r v4l2loopback 2>/dev/null; then
		sudo modprobe v4l2loopback exclusive_caps=0 \
			card_label="GC5024 Camera" video_nr="$VIDEO"
		sleep 1
	else
		echo "warning: v4l2loopback is busy; PipeWire may not list the camera" >&2
	fi
fi

if [ ! -e "$NODE" ]; then
	echo "$NODE did not appear (video_nr=$VIDEO already taken?)" >&2
	exit 1
fi

case "$MODE" in
720p)  want720=Y; want1080=N ;;
1080p) want720=N; want1080=Y ;;
full)  want720=N; want1080=N ;;
*) echo "MODE must be 720p, 1080p or full" >&2; exit 1 ;;
esac

have720=$(cat /sys/module/gc5024/parameters/mode_720p 2>/dev/null || echo none)
have1080=$(cat /sys/module/gc5024/parameters/mode_1080p 2>/dev/null || echo none)

if [ "$have720" != "$want720" ] || [ "$have1080" != "$want1080" ]; then
	echo "loading gc5024 in $MODE mode" >&2
	if [ "$have720" != none ]; then
		echo i2c-GCTI5024:00 | \
			sudo tee /sys/bus/i2c/drivers/gc5024/unbind >/dev/null 2>&1 || true
		sudo rmmod gc5024 2>/dev/null || true
	fi
	sudo modprobe v4l2-cci 2>/dev/null || true
	case "$MODE" in
	720p)  sudo insmod "$ROOT/src/gc5024.ko" mode_720p=1 ;;
	1080p) sudo insmod "$ROOT/src/gc5024.ko" mode_1080p=1 ;;
	full)  sudo insmod "$ROOT/src/gc5024.ko" ;;
	esac
	sleep 1
fi

case "$MODE" in
720p)  set -- --src-width 1280 --src-height 720 --width 1280 --height 720 "$@" ;;
1080p) set -- --src-width 1920 --src-height 1080 --width 1920 --height 1080 "$@" ;;
esac
# Prepended, so an explicit --width/--height still wins.

exec "$BIN" --dst "$NODE" "$@"
