#!/usr/bin/env python3
"""Regenerate src/gc5024_regs.h from the Rockchip BSP reference driver.

The reference driver lives in docs/gc5024_rockchip_reference.c.  The table in
that file is built from the IMAGE_* macros; this script expands the
IMAGE_NORMAL variant (no mirroring) into a v4l2-cci compatible sequence.
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
REFERENCE = os.path.join(ROOT, "docs", "gc5024_rockchip_reference.c")
OUTPUT = os.path.join(ROOT, "src", "gc5024_regs.h")

# IMAGE_NORMAL
MACROS = {
    "MIRROR": "0xd4",
    "PH_SWITCH": "0x1b",
    "STARTX": "0x0d",
    "STARTY": "0x03",
}

HEADER = """\
// SPDX-License-Identifier: GPL-2.0
/*
 * GC5024 register initialisation table.
 *
 * Derived from the Rockchip BSP driver (drivers/media/i2c/gc5024.c),
 * configured for IMAGE_NORMAL (no mirror/flip).
 * Do not edit by hand - see tools/gen_regs.py.
 */
#ifndef __GC5024_REGS_H__
#define __GC5024_REGS_H__

"""


def main():
    with open(REFERENCE, encoding="utf-8") as fh:
        src = fh.read()

    start = src.index("static const struct regval gc5024_global_regs[]")
    block = src[start:]
    block = block[: block.index("};")]

    pair = re.compile(r"\{0x([0-9a-fA-F]{2}),\s*(0x[0-9a-fA-F]{2}|[A-Z_]+)\}")
    pairs = pair.findall(block)

    out = [HEADER]
    out.append("static const struct cci_reg_sequence gc5024_global_regs[] = {\n")
    for addr, val in pairs:
        out.append("\t{ CCI_REG8(0x%s), %s },\n" % (addr, MACROS.get(val, val)))
    out.append("};\n")
    out.append("/* %d registers */\n\n" % len(pairs))
    out.append("#endif /* __GC5024_REGS_H__ */\n")

    with open(OUTPUT, "w", encoding="utf-8") as fh:
        fh.write("".join(out))

    print("wrote %s (%d registers)" % (OUTPUT, len(pairs)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
