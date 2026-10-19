#!/bin/sh
# Configure the IPU3 pipeline for the GC5024 and capture raw Bayer frames.
set -eu

MEDIA=/dev/media0
VIDEO=${VIDEO:-/dev/video0}
COUNT=${COUNT:-3}
OUT=${OUT:-/tmp/gc5024.raw}
SENSOR='gc5024 2-0037'
CSI2='ipu3-csi2 0'
CIO2='ipu3-cio2 0'

W=2592
H=1944

echo "== media topology =="
media-ctl -d "$MEDIA" -p 2>/dev/null | grep -E "entity|-> " || {
	echo "no media device at $MEDIA"; exit 1;
}

echo
echo "== configure pipeline =="
media-ctl -d "$MEDIA" --set-v4l2 "\"$SENSOR\":0[fmt:SBGGR10_1X10/${W}x${H}]"
media-ctl -d "$MEDIA" --set-v4l2 "\"$CSI2\":0[fmt:SBGGR10_1X10/${W}x${H}]"
media-ctl -d "$MEDIA" --set-v4l2 "\"$CSI2\":1[fmt:SBGGR10_1X10/${W}x${H}]"
media-ctl -d "$MEDIA" -l "\"$SENSOR\":0 -> \"$CSI2\":0[1]"
media-ctl -d "$MEDIA" -l "\"$CSI2\":1 -> \"$CIO2\":0[1]"
# 'ip3b' is the IPU3 packed BGGR format matching the sensor's bayer order.
v4l2-ctl -d "$VIDEO" --set-fmt-video=width=$W,height=$H,pixelformat=ip3b

echo
echo "== capture $COUNT frames =="
v4l2-ctl -d "$VIDEO" --stream-mmap --stream-count="$COUNT" --stream-to="$OUT"
ls -l "$OUT"
echo
echo "done - raw 10-bit BGGR, ${W}x${H}, IPU3 packed"
