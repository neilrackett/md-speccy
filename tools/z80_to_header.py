#!/usr/bin/env python3
"""Convert a .z80 Spectrum snapshot into a C header for the firmware.

The snapshot lands in flash as a `static const uint8_t` array; the
emulator seeds it into the SD app folder on boot when it isn't there
already (see populate_games_list() in rp/src/zxemu.c).

Usage:
  z80_to_header.py GAME.z80 --out rp/src/zx/builtin_game.h --name zx_builtin_game
"""
import argparse
import os


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("z80")
    ap.add_argument("--out", required=True)
    ap.add_argument("--name", required=True, help="C symbol prefix")
    args = ap.parse_args()

    with open(args.z80, "rb") as f:
        data = f.read()

    lines = []
    for i in range(0, len(data), 16):
        row = "".join("0x%02x," % b for b in data[i:i + 16])
        lines.append("    " + row)

    with open(args.out, "w") as f:
        f.write("/*\n"
                " * Copyright (C) 2026 Neil Rackett\n"
                " * SPDX-License-Identifier: GPL-3.0-or-later\n"
                " *\n"
                " * Auto-generated -- do not edit by hand.\n"
                " * Source: %s\n"
                " */\n"
                "#pragma once\n"
                "#include <stdint.h>\n"
                "\n"
                "static const uint8_t %s[] = {\n"
                % (os.path.basename(args.z80), args.name))
        f.write("\n".join(lines))
        # A macro, not a const variable, so it can size a file-scope table.
        f.write("\n};\n"
                "#define %s_len %du\n" % (args.name, len(data)))

    print("%s: %d bytes -> %s" % (args.z80, len(data), args.out))


if __name__ == "__main__":
    main()
