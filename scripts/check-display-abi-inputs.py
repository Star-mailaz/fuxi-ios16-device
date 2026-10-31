#!/usr/bin/env python3
"""Check the CPU-presentation ABI inputs of this board against its pinned kernelcache.

The legacy display archive (scripts/build-native-legacy-display.py) binds
IOMobileGraphicsFamily by exact address and vtable slot. The audit it runs first,
scripts/audit-mobile-graphics-legacy.py, is a pin on the sheng iPadOS 17.7.11
kernelcache: it rejects any candidate whose IOMobileGraphicsFamily __TEXT_EXEC sha256
is not the sheng value, then checks 17.7.11 layout constants (358-slot legacy vtable,
262-slot base pair, 0x18a0-byte instances). This board prelinks iOS 16.7.16 (20H392),
so those pins have to be re-derived from that image instead of edited.

This check reads the pinned kernelcache directly and reports, without claiming any
qualification:

  * whether the image matches the [kernel.prelink] kernelcache_sha256 pin;
  * the IOMobileGraphicsFamily extent and the sha256 a re-pin would record;
  * the class vtable tables in the component's __DATA_CONST and their shapes;
  * the delta against what the archive's builder and audit assume today;
  * the inputs that are still missing before the archive can be wired.

It writes nothing into the build. Run it from the resolved tree, or point it at the
prelink kit of a UniXNU checkout:

  python3 scripts/check-display-abi-inputs.py --kernelcache out/ipsw/20H392/kernelcache.decompressed
"""
import argparse
import hashlib
import os
import struct
import sys
from collections import Counter
from pathlib import Path

FAMILY = 'com.apple.iokit.IOMobileGraphicsFamily'

# Pins of the target image, derived from the 16.7.16 (20H392) kernelcache itself.
# kernelcache_sha256 is the same value as [kernel.prelink] kernelcache_sha256 in
# profiles/os/ios-16.7.16-20H392/profile.toml; the others are what a re-pin of
# scripts/audit-mobile-graphics-legacy.py and of the archive builder needs.
# Addresses are kernelcache addresses: the prelink applies slide -0x1000000 on top.
PIN = {'kernelcache_sha256': '2ec297bac95bcba01046fd0148e97e7e5885c05a6c18827f39f534ba375c1221',
       'family_text_sha256': '03e4c4d80e4f9c78d4ad03366cd3f1d5894d054ae53cdf06b1a43c81a2eab1cd',
       'base_vptr': 0xfffffff006ded3f0, 'service_vptr': 0xfffffff006dedc80,
       'legacy_vptr': 0xfffffff006def8e0, 'legacy_entries': 338,
       'base_entries': 252, 'abstract_slots': [211, 212, 242, 243, 292, 293, 321, 322, 329]}

# What scripts/build-native-legacy-display.py and scripts/audit-mobile-graphics-legacy.py
# assume today: sheng's iPadOS 17.7.11 class ABI. Kept here only to print the delta.
SHENG = {'family_text_sha256': '2e044ee68c1650397e1435234ca6459206262e4c7ed709cb7131c6f664f9bd9b',
         'base_entries': 262, 'legacy_entries': 358, 'instance_bytes': 0x18a0,
         'abstract_slots': [211, 213, 245, 246, 306, 307, 337, 338, 345]}

VTABLE_SCAN_MIN = 200
ABSTRACT_STUB_MIN = 2


def load_prelink(kit):
    """Import native_prelink from kits/XnuPrelink (or its NativePrelink alias)."""
    candidates = [Path(kit)] if kit else [Path(__file__).resolve().parents[1] / name
                                          for name in ('kits/XnuPrelink', 'kits/NativePrelink')]
    for directory in candidates:
        if (directory / 'native_prelink.py').is_file():
            sys.path.insert(0, str(directory))
            import native_prelink
            return native_prelink
    raise SystemExit('native_prelink.py not found; run this from the resolved tree '
                     '("unixnu tree") or pass --prelink-kit <checkout>/kits/XnuPrelink')


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def report_pin(name, actual, expected):
    state = 'PASS' if actual == expected else 'FAIL'
    print(f'  {name}: {actual}  {state} pinned {expected}')
    return actual == expected


def read_at(native_prelink, blob, segments, address, length):
    return blob[native_prelink.backed_offset(segments, address, length, len(blob)):][:length]


def vtables(native_prelink, blob, segments, data_const, text_ranges):
    """Find the class vtable runs: pointers into component or kernel executable text."""
    at = native_prelink.backed_offset(segments, data_const['va'], data_const['size'], len(blob))
    window = blob[at:at + data_const['size']]
    found, run = [], []

    def inside(value):
        return any(va <= value < va + size for va, size in text_ranges)

    for offset in range(0, len(window) - 7, 8):
        value = struct.unpack_from('<Q', window, offset)[0]
        if inside(value):
            run.append((offset, value))
            continue
        if len(run) >= VTABLE_SCAN_MIN:
            found.append(run)
        run = []
    if len(run) >= VTABLE_SCAN_MIN:
        found.append(run)
    result = []
    for run in found:
        va = data_const['va'] + run[0][0]
        boundary = read_at(native_prelink, blob, segments, va + 8 * len(run), 24)
        targets = [value for _, value in run]
        stub = Counter(targets).most_common(1)[0]
        abstract = [index for index, value in enumerate(targets) if value == stub[0]]
        result.append({'va': va, 'entries': len(run), 'targets': targets,
                       'boundary': boundary, 'abstract': abstract if stub[1] > ABSTRACT_STUB_MIN else []})
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--kernelcache', default=os.environ.get('UNIXNU_KERNELCACHE'),
                        help='decompressed kernelcache Mach-O; or UNIXNU_KERNELCACHE')
    parser.add_argument('--prelink-kit', help='directory holding native_prelink.py')
    args = parser.parse_args()
    if not args.kernelcache:
        raise SystemExit('--kernelcache is required (the decompressed IPSW KernelCache: '
                         '"unixnu ipsw fetch KernelCache -D <device>")')
    path = Path(args.kernelcache)
    if not path.is_file():
        raise SystemExit(f'kernelcache not found: {path}')
    native_prelink = load_prelink(args.prelink_kit)
    blob = path.read_bytes()
    print(f'kernelcache: {path}')
    ok = report_pin('sha256', sha256(blob), PIN['kernelcache_sha256'])
    image = native_prelink.macho(blob)
    segments = image['segments']
    info = next(s for s in segments if s['name'] == '__PRELINK_INFO')
    metadata = native_prelink.unserialize_xml(
        read_at(native_prelink, blob, segments, info['va'], info['filesize']))
    entries = {entry['CFBundleIdentifier']: entry
               for entry in metadata['_PrelinkInfoDictionary']}
    if FAMILY not in entries:
        raise SystemExit(f'{FAMILY} is not in this kernelcache')
    component = native_prelink.macho(
        blob, native_prelink.backed_offset(segments, entries[FAMILY]['_PrelinkExecutableLoadAddr'],
                                          32, len(blob)))
    text = next(s for s in component['segments'] if s['name'] == '__TEXT_EXEC')
    data_const = next(s for s in component['segments'] if s['name'] == '__DATA_CONST')
    kernel_text = next(s for s in segments if s['name'] == '__TEXT_EXEC')
    print(f'{FAMILY}: load {entries[FAMILY]["_PrelinkExecutableLoadAddr"]:#x} '
          f'extent {entries[FAMILY]["_PrelinkExecutableSize"]:#x}')
    text_sha = sha256(read_at(native_prelink, blob, segments, text['va'], text['filesize']))
    print(f'  __TEXT_EXEC {text["va"]:#x} size {text["filesize"]:#x}')
    ok &= report_pin('family __TEXT_EXEC sha256', text_sha, PIN['family_text_sha256'])
    print(f'  __DATA_CONST {data_const["va"]:#x} size {data_const["size"]:#x}')

    tables = vtables(native_prelink, blob, segments, data_const,
                     [(text['va'], text['filesize']), (kernel_text['va'], kernel_text['size'])])
    # The component carries the abstract base pair plus one longer concrete subclass,
    # exactly as the sheng image did; length separates them without a symbol table.
    lengths = sorted(table['entries'] for table in tables)
    if len(tables) != 3 or lengths[0] != lengths[1] or lengths[2] <= lengths[1]:
        raise SystemExit('expected the base pair plus one longer subclass vtable, found '
                         + ', '.join(f'{table["va"]:#x}/{table["entries"]}' for table in tables))
    pair = sorted((table for table in tables if table['entries'] == lengths[0]),
                  key=lambda table: table['va'])
    base, service = pair
    legacy = next(table for table in tables if table['entries'] == lengths[2])
    differing = [index for index, (left, right) in enumerate(zip(base['targets'], service['targets']))
                 if left != right]
    print('vtables:')
    for name, table in (('base', base), ('service', service), ('legacy', legacy)):
        print(f'  {name:<8}{table["va"]:#x} {table["entries"]:>3} slots'
              + (f'  abstract leaves {table["abstract"]}' if name == 'legacy' else ''))
    for name, table in (('base', base), ('service', service), ('legacy', legacy)):
        expected = PIN[{'base': 'base_vptr', 'service': 'service_vptr', 'legacy': 'legacy_vptr'}[name]]
        ok &= report_pin(f'{name} vptr', hex(table['va']), hex(expected))
        expected_entries = PIN['base_entries'] if name != 'legacy' else PIN['legacy_entries']
        ok &= report_pin(f'{name} entries', table['entries'], expected_entries)
    ok &= report_pin('base/service differing slots', differing, [0, 1, 7])
    ok &= report_pin('abstract leaves', legacy['abstract'], PIN['abstract_slots'])
    for name, table in (('base', base), ('service', service), ('legacy', legacy)):
        if table['boundary'] != bytes(24):
            ok = False
            print(f'  {name} vtable boundary: {table["boundary"].hex()}  FAIL expected 24 zero bytes')

    print('delta against the sheng iPadOS 17.7.11 pin the archive assumes today:')
    print(f'  family __TEXT_EXEC sha256  {SHENG["family_text_sha256"]} -> {text_sha}')
    print(f'  base/service slots         {SHENG["base_entries"]} -> {base["entries"]}')
    print(f'  legacy slots               {SHENG["legacy_entries"]} -> {legacy["entries"]}')
    print(f'  abstract leaves            {SHENG["abstract_slots"]} -> {legacy["abstract"]}')
    print(f'  instance bytes             {SHENG["instance_bytes"]:#x} (not derivable from the '
          'kernelcache: needs a runtime observation)')

    print('missing inputs before the archive can be wired:')
    print('  build/xnu-prelink/native-owner-mobile-graphics-candidate.macho and .json, a prelink of '
          'this kernelcache: "unixnu build kernel", then copy kernel/kernel.macho plus its .json')
    print('  build/darwinboot-owner-hvf-mobile-graphics-legacy-class/gdb-stop.json, the runtime '
          'observation of the original class (identity, parent metaclass, object size)')
    print('  the builder\'s own bindings: IOMobileFramebufferLegacy ctor/gMetaClass, the IOSurface '
          'getters at ~0xfffffff004b9ea**, the vsync sender, and slots 249/295/296/301')
    print('archive: not qualified; CONFIG_XNU_NATIVE_LEGACY_DISPLAY stays n')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
