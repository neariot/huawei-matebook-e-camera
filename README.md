# Huawei MateBook E Camera (GalaxyCore GC5024 on Intel IPU3)

Linux kernel driver and userspace integration for the front-facing camera of the **HUAWEI MateBook E** (Kaby Lake, Intel IPU3 ImgU / TPS68470 PMIC).

[![Linux](https://img.shields.io/badge/Linux-Mainline-blue.svg)](https://kernel.org)
[![ISP](https://img.shields.io/badge/ISP-Intel%20IPU3%20Hardware%20ISP-brightgreen.svg)](#intel-ipu3-hardware-isp-stack-recommended)
[![libcamera](https://img.shields.io/badge/libcamera-Supported-success.svg)](https://libcamera.org)
[![PipeWire](https://img.shields.io/badge/PipeWire-Camera%20Ready-red.svg)](https://pipewire.org)

---

## Status: Fully Working

- **Hardware ISP Acceleration**: Powered by Intel IPU3 ImgU via `libcamera` and `PipeWire`.
- **Zero Heavy CPU Usage**: Near 0% CPU consumption during streaming (unlike software ISP / CPU debayer).
- **Auto Exposure (AE) & Auto White Balance (AWB)**: Real-time hardware tuning handled by IPU3 IPA algorithms with display Gamma 2.2 and tuned BLC.
- **Resolution**:
  - **1080p (`1920x1080`) @ ~30 fps** (Default Viewfinder / Webcam mode).
  - **720p (`1280x720`) @ ~45 fps**.
  - **Full 5MP (`2592x1944`)** still raw capture support.
- **Desktop Application Compatibility**: Works out-of-the-box in GNOME Camera (Snapshot), Chromium / Chrome, Firefox, OBS Studio, Cheese, and WebRTC video conferencing.

---

## Hardware Specifications

| Item | Value | Source |
| :--- | :--- | :--- |
| **Model** | Huawei MateBook E (BL-W09 / BL-W19 / Kaby Lake) | DMI `HUAWEI` / `HUAWEI MateBook E` |
| **Sensor** | GalaxyCore GC5024 (Chip ID `0x5024`) | ACPI `GCTI5024`, hardware verified |
| **Location** | Built-in Front Camera (定焦 / Fixed Focus) | ACPI SSDB + hardware |
| **I2C Bus** | `0x37` on `I2C2` (`\_SB.PCI0.I2C2`) | ACPI `_CRS` |
| **Data Lanes** | 2 MIPI CSI-2 lanes @ 420 MHz link frequency | ACPI `SSDB` |
| **MCLK** | 19.2 MHz | ACPI `SSDB` + `tps68470` PLL |
| **Native Resolution** | 2592 x 1944 (5 MP, 10-bit Bayer BGGR, 1.4µm pixel) | Vendor spec / Hardware registers |
| **PMIC** | TI TPS68470 (`INT3472`) at `I2C2:0x4c` | ACPI |
| **Sensor Reset** | `tps68470-gpio` line 9 (`s_resetn`, active low) | Hardware verified |
| **PMIC Clock** | `PLLCTL = 0xD0`, then PLL enable bit | `clk-tps68470.c` |

---

## What Mainline Was Missing

On stock kernels, the Intel IPU3 capture nodes exist but fail with `VIDIOC_STREAMON returned -1 (Link has been severed)` because:
1. **No `gc5024` sensor driver** in mainline Linux (`drivers/media/i2c/`).
2. **Missing TPS68470 board data** for Huawei MateBook E, so `int3472-tps68470` bails out with `No board-data found for this model`, failing to power the sensor rails or clock.
3. **Missing `GCTI5024` entry in `ipu-bridge`**, preventing the ACPI camera graph from connecting to the CIO2 receiver.
4. **Missing GC5024 definitions in `libcamera`**, so IPU3 hardware ISP would reject the camera without sensor properties and tuning YAML.

---

## Architecture

This repository provides two ways to use the camera:

### 1. Modern Hardware ISP Stack (Recommended)
```text
GC5024 Sensor -> CIO2 (MIPI CSI-2) -> IPU3 ImgU (Hardware ISP)
                                              |
                                       libcamera (IPA AE/AWB)
                                              |
                                    PipeWire / WirePlumber
                                              |
                        GNOME Camera / Browsers / Video Apps
```
* **CPU Usage**: ~0%
* **Quality**: Hardware demosaicing, AE/AWB hardware stats, Gamma 2.2 correction.

### 2. Standalone Software ISP Stack (Fallback / Testing)
```text
GC5024 Sensor -> CIO2 -> /dev/video0 -> gc5024-camera (userspace) -> v4l2loopback (/dev/video20)
```
* Useful for headless environments or manual raw capture testing.

---

## Installation & Setup

### Prerequisites

For Arch Linux / Manjaro:
```sh
sudo pacman -S --needed dkms linux-headers base-devel libcamera pipewire-libcamera gst-plugin-libcamera
```

For Ubuntu / Debian / Fedora:
Ensure kernel headers, DKMS, and `libcamera` with PipeWire plugins are installed.

---

### Step 1: Install Kernel Drivers via DKMS

Run the install script as your normal user (uses `sudo` internally):
```sh
./tools/install.sh
```

This will automatically:
1. Install `gc5024` sensor driver as a DKMS module (`/extra/gc5024.ko`).
2. Install patched `ipu-bridge` and `intel_skl_int3472_tps68470` modules as DKMS.
3. Configure module loading and options in `/etc/modprobe.d/` and `/etc/modules-load.d/`.

Verify that the sensor is detected:
```sh
dmesg | grep -E "gc5024|ipu-bridge"
# Output should show:
# ipu-bridge: Found supported sensor GCTI5024:00
# gc5024 2-0037: Identified GC5024 sensor (chip id 0x5024)
```

---

### Step 2: Build & Install libcamera with IPU3 GC5024 Support

Run the helper script:
```sh
./tools/setup_libcamera.sh
```

This will:
- Clone libcamera.
- Apply [`patches/0003-libcamera-add-GalaxyCore-GC5024-support.patch`](patches/0003-libcamera-add-GalaxyCore-GC5024-support.patch).
- Build and install `libcamera` with IPU3 pipeline and signed IPA module.
- Install the tuning configuration to `/usr/share/libcamera/ipa/ipu3/gc5024.yaml`.

---

### Step 3: Test the Camera

Verify with `cam`:
```sh
cam -c 1 -I
```

You should see:
```text
Using camera \_SB_.PCI0.I2C2.CAM1 as cam0
0: 1920x1080-NV12
```

Launch **GNOME Camera (Snapshot)**, **Firefox**, or **Chrome** — the camera will be recognized as **"Built-in Front Camera"** streaming at 1080p 30fps.

---

## Repository Structure

```text
├── src/
│   ├── gc5024.c                 # Linux V4L2 subdev sensor driver
│   ├── gc5024_regs.h            # GC5024 register map definitions
│   └── Makefile                 # Kernel kbuild file
├── patches/
│   ├── 0001-media-ipu-bridge-add-GalaxyCore-GC5024-sensor.patch
│   ├── 0002-platform-x86-int3472-add-Huawei-MateBook-E-board-data.patch
│   └── 0003-libcamera-add-GalaxyCore-GC5024-support.patch
├── tuning/
│   └── ipu3/gc5024.yaml         # libcamera IPU3 IPA tuning profile
├── dkms/                        # DKMS packaging configs
├── tools/
│   ├── install.sh               # DKMS kernel driver installer
│   ├── setup_libcamera.sh       # Automated libcamera IPU3 builder
│   ├── probe_gpios.sh           # Hardware diagnostic script for TPS68470
│   ├── test_capture.sh          # Raw frame capture test
│   └── raw2png.py               # Software Bayer to PNG converter
├── apps/
│   └── gc5024-camera.c          # Standalone fallback userspace ISP
└── docs/                        # Hardware documentation and register tables
```

---

## Technical Details

### Power Sequencing & Timing
1. **`PLLCTL = 0xD0`**: Configures external capacitor `5 << 4` and crystal oscillator `1 << 7`, followed by PLL enable bit. Without this, no 19.2 MHz MCLK is produced.
2. **`s_resetn` (GPIO 9)**: The sensor is held in active-low reset until SGPO bit 2 is driven high.
3. **Power Rails**:
   - `dvdd` (CORE) = 1.2 V
   - `avdd` (ANA) = 2.8152 V
   - `dovdd` (VIO / VSIO) = 1.8 V
4. The rails and MCLK must stabilize before `reset` is deasserted, otherwise the sensor NAKs on the I2C bus.

### Sensor Windowing Modes
The GC5024 supports native windowing without scaling:
- **1080p Mode**: `1952x1096` window cropped to `1920x1080` @ 31.5 fps.
- **720p Mode**: `1312x736` window cropped to `1280x720` @ 45.9 fps.
- **Vertical Blanking**: `P0:0x07/0x08` controls VB (not VTS). Correct configuration prevents frame rate halving.

---

## License

- Kernel modules and patches: **GPL-2.0**
- libcamera tuning files and userspace scripts: **MIT / CC0-1.0**
