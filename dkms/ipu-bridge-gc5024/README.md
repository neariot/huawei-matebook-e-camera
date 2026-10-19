Kernel modules for the GC5024 camera, built with DKMS.

`ipu-bridge.c` and `tps68470_board_data.c` are taken from Linux v7.2.11
(the kernel this project was developed against) with the two patches in
`patches/` applied:

- `0001-media-ipu-bridge-add-GalaxyCore-GC5024-sensor.patch`
- `0002-platform-x86-int3472-add-Huawei-MateBook-E-board-data.patch`

`tps68470.c`, `tps68470_board_data.c` and `tps68470.h` build the
`intel_skl_int3472_tps68470` module; it links against the in-tree
`intel_skl_int3472_common` module.

Both modules are installed into `/lib/modules/$(uname -r)/updates/`, so
they override the in-tree copies.

When a future kernel changes these files, refresh them from the matching
source tree and reapply the patches, then rerun `tools/install.sh`.
