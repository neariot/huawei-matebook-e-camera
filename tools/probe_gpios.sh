#!/bin/sh
# GC5024 bring-up helper for the Huawei MateBook E.
#
# Powers the camera through the TPS68470 PMIC (I2C2 @ 0x4c), configures MCLK
# and deasserts the sensor reset, then checks for the GC5024 at 0x37.
#
# The order matters and was established on hardware:
#   1. program rail voltages           (selectors come up at 0 = far too low)
#   2. configure MCLK                  (PLLCTL = 0xD0, then PLL enable)
#   3. enable the rails
#   4. deassert s_resetn = SGPO bit 2  (otherwise the sensor never answers)
#
# SAFETY
#   * GPIO1 and GPIO2 are the TPS68470 I2C daisy-chain pins.  Driving them
#     disconnects the PMIC and wedges I2C2 until a cold power cycle.  This
#     script never touches them.
#   * All PMIC registers touched are snapshotted and restored on exit.
#     Because the kernel's tps68470-regulator/clk drivers cache their own
#     state, reload those modules (or reboot) before using the driver after
#     running this script with the modules loaded.
set -u

BUS=2
PMIC=0x4c
SENSOR=0x37
SENSOR_HEX=$(printf '%02x' "$SENSOR")

# registers we modify and must restore
REGS="0x06 0x07 0x08 0x09 0x0a 0x0b 0x0c 0x0d 0x0f 0x10 \
      0x14 0x16 0x18 0x1a 0x1c 0x1e 0x20 0x21 \
      0x22 0x27 0x3c 0x3d 0x3e 0x3f 0x40 0x41 0x42 \
      0x43 0x44 0x45 0x46 0x47 0x48"

w() { i2ctransfer -f -y "$BUS" w2@"$PMIC" "$1" "$2" >/dev/null 2>&1; }
r() { i2ctransfer -f -y "$BUS" w1@"$PMIC" "$1" r1 2>/dev/null; }

sensor_present() {
	i2cdetect -y -r "$BUS" "$SENSOR" "$SENSOR" 2>/dev/null \
		| grep -qw "$SENSOR_HEX"
}

SNAP=$(mktemp)
snapshot() {
	: > "$SNAP"
	for reg in $REGS; do
		val=$(r "$reg")
		[ -n "$val" ] && echo "$reg $val" >> "$SNAP"
	done
}

restore() {
	[ -s "$SNAP" ] || return
	while read -r reg val; do
		w "$reg" "$val"
	done < "$SNAP"
	rm -f "$SNAP"
	echo "PMIC registers restored."
}
trap restore EXIT INT TERM

clock_on() {
	# 19.2 MHz, identical to clk-tps68470.c's clk_freqs[0]
	w 0x06 0x01	# POSTDIV2
	w 0x07 0x03	# BOOSTDIV
	w 0x08 0x02	# BUCKDIV
	w 0x09 0x03	# PLLSWR default
	w 0x0a 0xaa	# XTALDIV = 170
	w 0x0b 0x20	# PLLDIV  = 32
	w 0x0c 0x01	# POSTDIV = 1
	w 0x10 0x05	# CLKCFG2: 2 mA drive
	# PLLCTL = (OSC_EXT_CAP 5 << 4) | (CLK_SRC_XTAL 1 << 7) = 0xD0
	w 0x0d 0xd0
	w 0x0f 0x0a	# CLKCFG1: outputs A and B
	w 0x0d 0xd1	# PLL enable
	sleep 0.02
}

rail_voltages() {
	w 0x42 0x0c	# VDVAL   CORE 1.200 V
	w 0x41 0x6d	# VAVAL   ANA  2.8152 V
	w 0x3f 0x34	# VIOVAL  VIO  1.8006 V
	w 0x40 0x34	# VSIOVAL VSIO 1.8006 V
	w 0x3d 0x34	# AUX1
	w 0x3e 0x34	# AUX2
	w 0x3c 0x34	# VCM
}

rails_on() {
	clock_on
	rail_voltages
	w 0x43 0x03	# VSIO / S_I2C
	w 0x47 0x01	# ANA
	w 0x48 0x05	# CORE
	w 0x45 0x01	# AUX1
	w 0x46 0x01	# AUX2
	w 0x44 0x01	# VCM
	sleep 0.2
	# s_resetn = SGPO bit 2: the sensor stays in reset without this.
	w 0x22 0x04
	sleep 0.2
}

echo "== snapshot PMIC state =="
snapshot

echo "== baseline =="
sensor_present && echo "  0x37 present" || echo "  0x37 absent"

echo "== power up =="
rails_on

if sensor_present; then
	echo "  >>> GC5024 present at 0x37"
	printf "      chip id: "
	hi=$(i2ctransfer -f -y "$BUS" w1@"$SENSOR" 0xf0 r1 2>&1)
	lo=$(i2ctransfer -f -y "$BUS" w1@"$SENSOR" 0xf1 r1 2>&1)
	echo "$hi / $lo"
else
	echo "  0x37 absent - sensor did not come up"
fi
