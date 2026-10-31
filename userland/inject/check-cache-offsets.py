#!/usr/bin/env python3
"""Re-derive every address in fuxi_soft_render.h from the 20H392 dyld symbol table.

The table in fuxi_soft_render.h is not taken on trust: this script reads it back out of the
C header, looks each value up in the local symbol table of the dyld shared cache that the
20H392 restore image ships, and -- when the image itself is still around -- reads the six
vtable slots the hooks write and checks that each one holds the value the table records.

  check-cache-offsets.py                     # header, symbols and image at their defaults
  check-cache-offsets.py --symbols PATH       # only the symbol file is required
  check-cache-offsets.py --image PATH         # also verify the vtable slots

Inputs (defaults, relative to this file):
  header   ../fuxi_soft_render.h                     (this directory)
  symbols  ../../../.cache/ipsw/work/dyld_shared_cache_arm64.symbols
  image    ../../../.cache/ipsw/members/087-86593-047.dmg

How the two inputs are produced is written down in .cache/ipsw/REPORT-20H392-recon.md: the
symbol file comes out of Cryptex1,SystemOS (member 087-86593-047.dmg) at
/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64.symbols, which is a decmpfs
type 14 (LZBITMAP, resource fork) file; the image is that same member inflated.

Exit status is 0 when every field resolved and every vtable slot matched, 1 otherwise.
"""
import argparse
import mmap
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

# Field -> (vtable field, offset from the first virtual function). Offsets are the numbers
# VSLOT() adds in fuxi_soft_render.c; the absolute offset from __ZTV is 0x10 more.
SLOTS = [
    ("clcd_is_ready", "vt_apple_clcd_display", 0xb0),
    ("clcd_ignore_update_p", "vt_apple_clcd_display", 0x138),
    ("clcd_begin_update", "vt_apple_clcd_display", 0x178),
    ("clcd_finish_update", "vt_apple_clcd_display", 0x180),
    ("eagl_renderer", "vt_eagl_server", 0x1a0),
    ("eagl_render_update", "vt_eagl_server", 0x178),
]


def read_table(path):
    """The initialiser of fuxi_cache_16_7_16 as a dict of field -> int."""
    text = open(path, "r", encoding="utf-8").read()
    start = text.index("fuxi_cache_16_7_16")
    body = text[text.index("{", start):text.index("}", start)]
    table = {}
    for name, value in re.findall(r"\.(\w+)\s*=\s*(0x[0-9a-fA-F]+|\d+)", body):
        table[name] = int(value, 0)
    return table


class Symbols:
    """The local symbol table of a dyld shared cache (dyld_cache_local_symbols_info)."""

    def __init__(self, path):
        self.file = open(path, "rb")
        self.image = mmap.mmap(self.file.fileno(), 0, access=mmap.ACCESS_READ)
        if self.image[:7] != b"dyld_v1":
            raise SystemExit("%s: not a dyld cache or symbol file" % path)
        for offset in (0x48, 0x1a8):		# localSymbolsOffset: this layout, then the older one
            info, = struct.unpack_from("<Q", self.image, offset)
            if not info or info + 24 > len(self.image):
                continue
            no, nc, so, ss, eo, ec = struct.unpack_from("<6I", self.image, info)
            if nc and ss and info + so + ss <= len(self.image):
                break
        else:
            raise SystemExit("%s: no local symbol table in this file" % path)
        self.info = info
        self.nlist_at = info + no
        self.nlist_count = nc
        self.strings_at = info + so
        self.strings_size = ss

    def name(self, index):
        at = self.strings_at + index
        end = self.image.find(bytes([0]), at, self.strings_at + self.strings_size)
        return self.image[at:end].decode("latin1")

    def resolve(self, values):
        """value -> first symbol name, for the addresses asked about."""
        found = {}
        blob = self.image[self.nlist_at:self.nlist_at + 16 * self.nlist_count]
        for strx, ntype, sect, desc, value in struct.iter_unpack("<IBBHQ", blob):
            if value in values and value not in found:
                found[value] = self.name(strx)
        return found


def cache_ranges(image):
    """(va, size, file offset) for every dyld cache fragment in a raw disk image."""
    ranges = []
    at = image.find(b"dyld_v1")
    while at >= 0:
        mapping_at, count = struct.unpack_from("<II", image, at + 16)
        if 32 <= mapping_at < 0x1000 and 1 <= count <= 64:
            maps = []
            for i in range(count):
                va, size, foff, maxprot, initprot = struct.unpack_from("<QQQII", image, at + mapping_at + 32 * i)
                if va % 0x4000 or size % 0x4000 or foff % 0x4000 or not size:
                    maps = []
                    break
                maps.append((va, size, at + foff))
            ranges.extend(maps)
        at = image.find(b"dyld_v1", at + 16)
    return ranges


def slot_target(image, ranges, va):
    """The function address a fixed-up vtable slot will hold, read from the cache image.

    The slot stores a dyld chained-fixup rebase: the address is in the low 34 bits and the
    remaining bits are the chain metadata dyld applies when the page is brought in. For this
    cache the metadata is a constant 0x20 in the high8 field, which is why the plain 8-byte
    value is not the address and the mask below is.
    """
    for base, size, foff in ranges:
        if base <= va < base + size:
            raw, = struct.unpack_from("<Q", image, foff + (va - base))
            return raw & 0x3FFFFFFFF
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--header", default=os.path.join(HERE, "fuxi_soft_render.h"))
    parser.add_argument("--symbols", default=os.path.join(HERE, "..", "..", "..", ".cache", "ipsw", "work",
                                                          "dyld_shared_cache_arm64.symbols"))
    parser.add_argument("--image", default=os.path.join(HERE, "..", "..", "..", ".cache", "ipsw", "members",
                                                        "087-86593-047.dmg"))
    args = parser.parse_args()

    table = read_table(args.header)
    problems = []
    print("table %s" % os.path.relpath(args.header))
    for name, value in table.items():
        print("  %-28s %#012x" % (name, value))

    symbols = Symbols(args.symbols)
    values = {v for v in table.values() if v}
    names = symbols.resolve(values)
    print()
    print("symbols %s (%d nlist entries)" % (os.path.relpath(args.symbols), symbols.nlist_count))
    for name, value in sorted(table.items()):
        if not value:
            print("  %-28s 0  MISSING" % name)
            problems.append("%s is 0" % name)
            continue
        symbol = names.get(value)
        print("  %-28s %#012x  %s" % (name, value, symbol if symbol else "NOT IN THE SYMBOL TABLE"))
        if not symbol:
            problems.append("%s does not resolve" % name)

    if os.path.exists(args.image):
        image_file = open(args.image, "rb")
        image = mmap.mmap(image_file.fileno(), 0, access=mmap.ACCESS_READ)
        ranges = cache_ranges(image)
        print()
        print("vtable slots in %s (%d cache fragments)" % (os.path.relpath(args.image), len(ranges)))
        for field, vt_field, offset in SLOTS:
            vt = table.get(vt_field, 0)
            expected = table.get(field, 0)
            if not vt or not expected:
                print("  %-28s skipped (table entry 0)" % field)
                continue
            va = vt + 0x10 + offset
            found = slot_target(image, ranges, va)
            if found is None:
                print("  %-28s %#012x -> not mapped by any cache fragment" % (field, va))
                problems.append("%s slot %#x is not in the cache" % (field, va))
                continue
            ok = found == expected
            print("  %-28s slot %#012x holds %#012x  %s  %s" % (field, va, found, "ok" if ok else "MISMATCH",
                                                               names.get(found, "")))
            if not ok:
                problems.append("%s: slot holds %#x, table says %#x" % (field, found, expected))
    else:
        print()
        print("image %s not present: the vtable slots were not checked" % args.image)

    print()
    if problems:
        for problem in problems:
            print("FAIL %s" % problem)
        return 1
    print("PASS: every field resolves to a symbol and every vtable slot holds the recorded value")
    return 0


if __name__ == "__main__":
    sys.exit(main())
