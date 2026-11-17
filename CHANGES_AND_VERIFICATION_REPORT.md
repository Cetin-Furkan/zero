# `Zero` — Verification & Milestone Report (Tracks 1–3: NASM Retirement, Ring-0 AVX-512, Interactive ZeroOS)

All results below come from `make clean && make test && make usb-img` in `/home/arch-cetin/projects/zero` on the reference host (i7-1165G7, KVM, OVMF `OVMF_CODE.4m.fd`). The verifier is `tests/run_tests.sh`. Every stage uses the **self-hosted `build/zero2`** compiler; `build/zero0` (NASM seed) only bootstraps `zero1`.

---

## 1. Self-Hosting Fixed Point & 100% NASM Retirement (Track 1)

`compiler/` holds the 11-module Stage-1 compiler written in pure `.zr`: `zero1`, `defs`, `lexer`, `types_spaces`, `parser_expr`, `intrinsics_ptr`, `loops_match`, `emit_x64`, `evex_zmm`, `efi_pe32`, `containers`. It has no C, no LLVM, no libc, and no external linker.

| Artifact | Size (Bytes) | Format | Verification |
| :--- | :---: | :--- | :--- |
| `build/zero0` | `48,112` | 2-segment W^X ELF64 (`R E` + `RW`) | `nasm -f bin bootstrap/seed0.asm`. This is the only NASM step. |
| `build/zero1` | `549,308` | 2-segment W^X ELF64 | `compiler/zero1.zr` compiled by `zero0` |
| `build/zero2` | `508,289` | 2-segment W^X ELF64 | `compiler/zero1.zr` compiled by `zero1` |
| `build/zero3` | `508,289` | 2-segment W^X ELF64 | Compiled by `zero2`. **`cmp -s zero2 zero3` gives 0 bytes of difference.** |
| `build/gate1_os_z2.efi` | `99,328` | PE32+ (`Subsystem 0x0a`), `.text` RX / `.reloc` RO / `.bss` RW | Full `ZeroOS`, built with `zero2 --target=efi-os` |
| `build/gate1_laptop_z2.efi` | `95,232` | PE32+ | `qemu_debug :: 0` build. It contains no port-`0xE9` I/O; the debug build is the positive control with 7. |
| `build/capsule_z2.zcap` | `552` | `ZEROZCAP` + CRC32C | NVMe Extent 0 (compute) |
| `build/cap_avx512_wave.zcap` | `776` | `ZEROZCAP` + CRC32C | NVMe Extent 1 (AVX-512 `u64x8` wave) |
| `build/cap_matrix_crc.zcap` | `808` | `ZEROZCAP` + CRC32C | NVMe Extent 2 (CRC32C + PEXT/PDEP matrix) |
| `build/zeroos_usb.img` | `67,108,864` | Protective MBR + GPT + FAT32 ESP | Built by `make usb-img` |

**Independent audit:** `compiler/*.zr` issues only file-I/O syscalls (`openat`, `lseek`, `read`, `write`, `fchmod`, `close`, `exit`) and never launches an external tool. With `build/zero0` deleted, `zero2` rebuilds itself, `gate1_os_z2.efi`, and `capsule_z2.zcap` byte for byte.

---

## 2. Language Verification (Stages 1–4)

- **8 golden suites compiled by `zero2`:** `first`, `step3_compute`, `step4_systems`, `phase0_scalar`, `phase1_zero_lang`, `phase2_zero_power`, `phase3_reactor_fabric`, `phase4_avx512_simd`. Every binary has strict W^X segments and its stdout matches the `.expected` file exactly.
- **Opcodes checked with `objdump`:**
  - Silicon: `movnti`, `lock xadd`, `lock cmpxchg`, `popcnt`, `lzcnt`, `tzcnt`, `pdep`, `pext`, `crc32`, `prefetcht0`, `bswap`, `sfence`/`lfence`/`mfence`, `movdir64b`, `movdiri`, `cpuid`.
  - AVX-512: `vmovdqu32`, `vmovdqu64` (including compressed `disp8*64`), `{%k1}{z}` loads, `{%k1}` stores, `kmovw`, `bzhi`, `vmovntdq`, `vpbroadcastd/q`, `vpaddd/q`, `vpmulld/q`, `vpsllvq`.
  - Scalar loop step: `incq`.
- **`tests/phase4_avx512_simd.zr` covers:**
  - `@sizeof`/`@alignof(u32x16|u64x8) == 64`.
  - The vector ALU, vector function arguments and returns, and a 64-bit-safe `@vec_sum32` (`16 × 0x40000000 = 17179869184`).
  - `{k1}` masked loads and stores that leave sentinel values intact.
  - `@vec_stream`, plus `<simd>` loops over an `@stream` span.
  - `<simd: 16>` and `<simd: 8>` loops at `N = 0, 1, 7, 19, 37`.
  - A `<simd>` bounds fault (`fail 2`) caught by `recover`.
- **54 negative probes**, all rejected by `zero2` with the expected diagnostic on `ELF64`, `--target=efi-os`, and `--target=capsule`. Track 2 added 7 of them: AVX-512 outside `<tile>` on EFI and capsule targets, AVX-512 in `@isr`/`@isr_ok`, `<simd: 3>`, and non-vectorizable calls inside `<simd>`.
- **Runtime:** an unhandled `fail 99` exits with status `1`.

---

## 3. Live Bare-Metal `ZeroOS` (Stage 6) — QEMU Q35 + KVM + OVMF + NVMe

The test disk holds a `ZEXTENT1` table with 3 extents. The run boots on 4 cores with `-overcommit cpu-pm=on`, then sends the keys `1 2 3 b a`, takes a screendump, and sends `q`. The full debugcon log must match exactly:

```text
ZEROHAND W=1280 H=800 S=5120 F=0 C=4 M=20 PT=21 WX=0 ACPI=1 GS=1 FAB=1 SIMD=1 VEC=3535/7864 CYC=1685 CAP=777 KBD
CAP1=2337
CAP2=1207754
CAP3=35277270
BENCH=OK
SCAN=0x0000001E GATE1B_OK
SHUTDOWN_OK
```

- **Capsule results checked against an independent model:** I wrote a separate Python reference model of each capsule (x86 `crc32 r64` semantics, BMI2 `pext`/`pdep`, `u64x8` lane arithmetic). Its output matches the hardware results for every core count tested:

  | Cores | CAP1 | CAP2 | CAP3 | Who runs the capsule |
  | :--- | :---: | :---: | :---: | :--- |
  | 4 | `2337` | `1207754` | `35277270` | worker reactors 1–3 |
  | 2 | `778` | `392326` | `4019694` | worker reactor 1 |
  | 1 | `777` | `381953` | `978462` | BSP |

- **Pixel checks on the screendump:**
  - AVX-512 `vmovntdq` gradient scanout, including 16-pixel chunk boundaries and the frame corners.
  - All 6 LEDs, 4 core pips with no 5th pip, and the top HUD badge.
  - Command Deck: cyan border, header bar, obsidian body, and the `> [3] EXT2 MATRIX_CRC` highlight and marker.
  - The rendered image is `gate4_live_os_hud.png`.
- **IRQ1 is really interrupt-driven:** after the keys `1 2 3 b`, the Deck's `IRQS:` counter reads `7`. That is one IRQ per scancode byte up to that moment (4 make codes + 3 break codes).
- **Stability:** six consecutive interactive 4-core runs all passed with identical capsule sums.
- **Fallbacks:**
  - `-smp 1` with NVMe runs all three capsules on the BSP.
  - `-smp 1` on i440FX without NVMe gives `C=1 CAP=0`, and the capsule key safely reports `CAP2=0`.
  - `-vga none` writes 0 bytes to `0xE9` and returns to firmware.
- **Ring-0 AVX-512 benchmark:** `<tile | simd: 8>` over 515 `u64` elements (64 full vectors plus a 3-lane `{k1}` tail and a sentinel). Best-of-4 timing with `@lfence` + `@rdtsc` shows the vector path about **2× faster** than scalar (e.g. `VEC=3535/7864`).

---

## 4. Physical Laptop USB Image (Stage 7, `make usb-img`)

- **Partition table:** protective MBR (`0xEE`), primary and backup GPT headers with valid header and entry-array CRC32s, and one ESP partition (`C12A7328-F81F-11D2-BA4B-00A0C93EC93B`, LBA 2048–131007). `sfdisk --verify` reports it clean.
- **Filesystem:** spec-compliant FAT32 with 126,912 clusters and identical FAT1/FAT2. A FAT32 reader written separately from the image builder extracts `/EFI/BOOT/BOOTX64.EFI` (95,232 B). That file is byte-identical to the independently compiled `zero2` `qemu_debug :: 0` build.
- **Boot test:** OVMF boots the raw image from an emulated xHCI `usb-storage` stick in about 1.4 s and reaches the 1280×800 Command Deck with 4 core pips. The i8042 key `a` paints the scancode cell, and the image writes 0 bytes to port `0xE9`.

---

## 5. Honest Remaining Gaps

- `<tile: Size>` cache blocking and `@cold` field splitting are still rejected at compile time.
- `Iris Xe` `CCS0` GPU compute (Gate 4) is not implemented.
- The USB image is verified under OVMF emulation only. It has not been booted on the physical Tiger Lake laptop by this test suite.
