# Display ABI pins for the CPU presentation path

The software-render profile takes its display backend from `GopFramebuffer`
(`NativeLegacyDisplay.cpp` + `NativeGOPPresenter.cpp` + `NativeGOPWire.h`): the
compositor's surface is copied into the firmware GOP buffer on the CPU. The kernel side of
that backend is the late archive `scripts/build-native-legacy-display.py`, which binds the
**original** `IOMobileGraphicsFamily` class by exact address and vtable slot.

That archive is deliberately **not** in `board.toml ["kernel"] archives` and
`CONFIG_XNU_NATIVE_LEGACY_DISPLAY` stays `n`. This file records why, what the pins are,
what the target image gives us, and which inputs are still missing.

## Which script is which

| Piece | Where it lives | What it does |
| --- | --- | --- |
| Archive builder | `fuxi/scripts/build-native-legacy-display.py` | Generates the library's ABI header from the audit output, builds `NativeLegacyDisplay`/`NativeGOPPresenter` and the board probes, and links the late archive. |
| ABI audit | `unixnu/scripts/audit-mobile-graphics-legacy.py` (**UniXNU mainline**, not sheng) | Reads a prelinked candidate, checks that its `com.apple.iokit.IOMobileGraphicsFamily` is the exact binary it was qualified against, then records the vtable layout in `build/display-analysis/mobile-graphics-legacy-abi.json`. |

The device repository's `scripts/` directory is copied into the resolved tree **after** the
mainline one, so a device copy can override a mainline script. The audit is not duplicated
here: the pin below is the mainline one, and replacing it with a second copy that only
differs in constants would hide which binary was audited.

## What the audit pins today (sheng iPadOS 17.7.11)

Inputs it reads:

```
build/xnu-prelink/native-owner-mobile-graphics-candidate.macho   (+ .json prelink report)
build/darwinboot-owner-hvf-mobile-graphics-legacy-class/gdb-stop.json   (runtime observation)
```

Pins, all from the sheng iPadOS 17.7.11 kernelcache:

| Pin | Value |
| --- | --- |
| candidate sha256 (or the family text sha256 below) | `aded28d3c61dafaf7b8a7c96b14427a798e9e95470ae6f287bcd038c07626798` |
| `IOMobileGraphicsFamily` `__TEXT_EXEC` sha256 | `2e044ee68c1650397e1435234ca6459206262e4c7ed709cb7131c6f664f9bd9b` |
| base / service vtable | 262 slots each, differ only at slots `[0, 1, 7]` |
| legacy vtable | 358 slots, next aligned boundary empty |
| abstract leaves | slots `[211, 213, 245, 246, 306, 307, 337, 338, 345]` |
| generic overrides | 103, of which the ones below slot 168 are `[0,1,7,14,21,44,79,85,86,87,111,141,155]` |
| `newUserClient` | slot 141, four-argument `IOService` ABI |
| instance size | `0x18a0` |
| vptr sites (candidate addresses) | legacy `0xfffffff0057865d8`, base `0xfffffff00577b324`, service `0xfffffff00577b51c` |
| protected base calls | ctor `0xfffffff0057865fc` → `0xfffffff00577b510`, destructor `0xfffffff005786630` → `0xfffffff00577b55c` |
| runtime checkpoint | `3044c2beff552911605994c0dc7aa73182066cbb5aed0b396999ceeb5a96da2b` (identity, parent metaclass `0xfffffff006b31400`, size `0x18a0`) |

The builder pins more of the same image on its own: `IOMobileFramebufferLegacy` constructor
`0xfffffff0057865cc`, destructor `0xfffffff005786618`, `gMetaClass`
`0xfffffff006b35940`, the IOSurface getters at `0xfffffff004b9ea**`, the vsync sender
`0xfffffff005796720` (plus four instruction signatures), the refresh publisher
`0xfffffff00579ebb8`, and slots 249/295/296/301 checked against exact instructions.

## The target: iOS 16.7.16 (20H392)

`profiles/os/ios-16.7.16-20H392/profile.toml` already pins this image and prelinks
`IOMobileGraphicsFamily` and `IOSurface` from it:

| Item | Value |
| --- | --- |
| IPSW (Apple CDN, `unixnu ipsw fetch KernelCache`) | `https://updates.cdn-apple.com/2025FallFCS/fullrestores/122-77006/1158E502-D76D-4226-AA06-ED5E30CD1111/iPhone10,3,iPhone10,6_16.7.16_20H392_Restore.ipsw` |
| `KernelCache` member | `kernelcache.release.iphone10b`, 18,204,358 bytes, IM4P payload is LZFSE |
| decompressed kernelcache sha256 | `2ec297bac95bcba01046fd0148e97e7e5885c05a6c18827f39f534ba375c1221` (equals the profile pin) |
| `IOMobileGraphicsFamily` | load `0xfffffff004f60400`, `__TEXT` `0x8378`, `__TEXT_EXEC` `0xfffffff005bf3000` size `0x34544`, `__DATA_CONST` `0xfffffff006deae00` size `0x60d0` |
| family `__TEXT_EXEC` sha256 | `03e4c4d80e4f9c78d4ad03366cd3f1d5894d054ae53cdf06b1a43c81a2eab1cd` |

Its class ABI is **not** the sheng one:

| Pin | 17.7.11 (assumed by the archive) | 16.7.16 (this board) |
| --- | --- | --- |
| family `__TEXT_EXEC` sha256 | `2e044ee6…f9bd9b` | `03e4c4d8…eab1cd` |
| base / service vtable | 262 | 252 |
| legacy vtable | 358 | 338 |
| abstract leaves | `[211,213,245,246,306,307,337,338,345]` | `[211,212,242,243,292,293,321,322,329]` |
| vptr (kernelcache address, slide `-0x1000000` on top) | – | base `0xfffffff006ded3f0`, service `0xfffffff006dedc80`, legacy `0xfffffff006def8e0` |

The method still applies (the base pair still differs only at `[0, 1, 7]`; the concrete
subclass still ends at an empty boundary and owns every entry from slot 168 up except the
abstract leaves), but every index the builder uses — 184 `isMainDisplay`, 188
`getDotPitch`, 213 `supportedFrameInfo`, 249 display dimensions, 288 `startHardware`,
290 `kickSwapQueue`, 295 `completeCurrentSwap`, 296 `validateSwap`, 301 refresh
attributes, 337 completion reports — shifts, and the offsets the generated header copies
(`0x18a0` instance, queue head `+5480`, tail `+5488`, current `+5472`, dimensions
`+5808/+5812`, request links `+2960/+2968`) have to be re-measured. The sheng values are
therefore not a hash swap away; re-pinning means re-deriving this table from the 16.7.16
binary plus one runtime observation.

## Missing inputs

| Input | Comes from | Blocks |
| --- | --- | --- |
| `build/xnu-prelink/native-owner-mobile-graphics-candidate.macho` and `.json` | `unixnu build kernel` prelinks the 16.7.16 kernelcache into `kernel/kernel.macho`; copy that plus its `.json` to the candidate name (`docs/building.md`, "bootstrap candidate") | the audit, and every address check in the builder |
| a working kernel toolchain | Linux: `sudo scripts/linux/setup-toolchain.py` (installer puts it in `/opt/unixnu-linux`); macOS: Xcode with the iPhoneOS SDK | the kernel build that produces the candidate |
| runtime checkpoint `build/darwinboot-owner-hvf-mobile-graphics-legacy-class/gdb-stop.json` | a darwinboot run of the original class (identity `IOMobileFramebufferLegacy`, parent metaclass, object size) against the same kernelcache | the audit's object-size/identity check, and `0x18a0`-style offsets |
| 16.7.16 semantics for the builder's slots and byte signatures | read out of the candidate above (`__TEXT_EXEC` disassembly) | the builder's own `bindings` table and its `require()` instruction signatures |
| macOS-only helpers in the builder | `xcrun --sdk iphoneos` and `brew --prefix llvm` | on Linux the same objects need the toolchain's `clang`/`llvm-objcopy` instead |

On this host the first two are absent (no `/opt/unixnu-linux`, no Xcode), so no candidate
can be produced and the archive stays off. Nothing above is fabricated: the 16.7.16 row was
read from the pinned kernelcache.

## Check the inputs that exist

`scripts/check-display-abi-inputs.py` reads the pinned kernelcache directly and verifies
the 16.7.16 row of the table above, then prints the delta against the sheng pin and the
missing inputs. It writes nothing into the build.

```sh
# one-time, on a host without the UniXNU toolchain: Apple's LZFSE tool
git clone --depth 1 https://github.com/lzfse/lzfse.git
make -C lzfse && install -m755 lzfse/build/bin/lzfse ~/.local/bin/

cd unixnu
./unixnu ipsw fetch KernelCache -D ../fuxi     # 18 MB member, HTTP range reads
python3 out/fuxi/ios-16.7.16-20H392/fuxi_softrender/tree/scripts/check-display-abi-inputs.py \
    --kernelcache out/ipsw/20H392/kernelcache.decompressed
```

`unixnu ipsw fetch` reads the zip with 8 MiB range requests. When a proxy resets those
(a 5.4 GB IPSW makes the CDN transfer slow enough to hit that), read the same member with
smaller blocks through the same reader and decode it yourself; the local working copy of
this derivation lives in `.cache/ipsw-20H392/`, outside either repository:

```sh
python3 - <<'EOF'
import struct, sys, zipfile
from pathlib import Path
sys.path.insert(0, 'unixnu/scripts')
from unixnu.ipsw import RangeFile, im4p_payload
url = 'https://updates.cdn-apple.com/2025FallFCS/fullrestores/122-77006/1158E502-D76D-4226-AA06-ED5E30CD1111/iPhone10,3,iPhone10,6_16.7.16_20H392_Restore.ipsw'
member = zipfile.ZipFile(RangeFile(url, block=1 << 20)).read('kernelcache.release.iphone10b')
Path('kernelcache.lzfse').write_bytes(im4p_payload(member))
EOF
lzfse -decode -i kernelcache.lzfse -o kernelcache.fat
python3 - <<'EOF'
import struct
from pathlib import Path
fat = Path('kernelcache.fat').read_bytes()
if fat[:4] == b'\xca\xfe\xba\xbe':
    count = struct.unpack('>I', fat[4:8])[0]
    for index in range(count):
        cpu, _, offset, size, _ = struct.unpack('>5I', fat[8 + 20 * index:28 + 20 * index])
        if cpu == 0x0100000c:
            fat = fat[offset:offset + size]
Path('kernelcache.macho').write_bytes(fat)
EOF
# sha256(kernelcache.macho) must equal the profile pin, then:
python3 out/fuxi/ios-16.7.16-20H392/fuxi_softrender/tree/scripts/check-display-abi-inputs.py \
    --kernelcache kernelcache.macho
```

## Provenance

- Archive builder: adapted from `UniXNU/UniXNU-sheng` `scripts/build-native-legacy-display.py`
  (renamed to `build-gop-legacy-display.py` in sheng `1cab151`, 2026-10-25) and then edited
  for this board. The sheng file at `9871f4e` and this copy are not byte-identical, so this
  is a derived file, not a vendored one. Both repositories are BSD-2-Clause-Patent,
  Copyright (c) 2026, UniXNU contributors, so no extra licence text is needed; the source
  naming is kept in the file header.
- Audit and prelink helper: `UniXNU/UniXNU` mainline (`scripts/audit-mobile-graphics-legacy.py`,
  `kits/XnuPrelink/native_prelink.py`), unchanged by this board.
- LZFSE tool for the local check: apple/lzfse, BSD-3-Clause, used only on the host.
