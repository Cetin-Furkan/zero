# `Zero` & `ZeroOS` — Empirical Verification & Disassembly Ledger

> [!NOTE]
> Every measurement, byte count, `objdump` opcode check, and hardware boot trace in this report is reproduced automatically from scratch via `make clean && make test && make usb-img` (`tests/run_tests.sh`, **`78 PASS / 0 FAIL`**) on the reference **Intel Core i7-1165G7** host (`KVM` + `OVMF_CODE.4m.fd`). Every compilation stage after bootstrapping `zero1` runs exclusively on the **self-hosted `build/zero2`** compiler.

---

## 1. Self-Hosting Fixed Point & Binary Artifact Ledger (Track 1)

`compiler/` contains the 11-module self-hosted `.zr` compiler (`zero1.zr`, `defs.zr`, `lexer.zr`, `types_spaces.zr`, `parser_expr.zr`, `intrinsics_ptr.zr`, `loops_match.zr`, `emit_x64.zr`, `evex_zmm.zr`, `efi_pe32.zr`, `containers.zr`) with **zero C, zero LLVM, zero libc, zero heap allocation, and zero external assembler/linker**:

| Artifact | Exact Size | Container & Permissions | Empirical Verification Proof |
| :--- | :---: | :--- | :--- |
| `build/zero0` | `48,112 B` | 2-segment `W^X` `ELF64` (`R E` + `RW`) | Stage-0 seed assembled by `nasm -f bin bootstrap/seed0.asm` (only NASM step in the build). |
| `build/zero1` | `549,308 B` | 2-segment `W^X` `ELF64` (`R E` + `RW`) | `compiler/zero1.zr` compiled by `build/zero0`. |
| `build/zero2` | `508,289 B` | 2-segment `W^X` `ELF64` (`R E` + `RW`) | `compiler/zero1.zr` compiled by `build/zero1`. |
| `build/zero3` | `508,289 B` | 2-segment `W^X` `ELF64` (`R E` + `RW`) | `compiler/zero1.zr` compiled by `build/zero2` — **`cmp -s build/zero2 build/zero3` == `0` bytes difference!** |
| `build/gate1_os_z2.efi` | `99,328 B` | UEFI `PE32+` (`0x8664`, `Subsys 0x0A`), `.text` `RX` / `.reloc` `RO` / `.bss` `RW` | Full debug `ZeroOS` (`qemu_debug :: 1`, `7` port-`0xE9` instructions), built directly by `build/zero2 --target=efi-os`. |
| `build/gate1_laptop_z2.efi` | `95,232 B` | UEFI `PE32+` (`0x8664`, `Subsys 0x0A`), strict `W^X` sections | Physical laptop `ZeroOS` (`qemu_debug :: 0`) — **`objdump` confirms `0` `in`/`out` instructions referencing `0xE9` or `(%dx)`**. |
| `build/capsule_z2.zcap` | `552 B` | `ZEROZCAP` + SSE4.2 `CRC32C` | NVMe Extent `0`: Reactor-ABI compute capsule (`examples/capsule_compute.zr`). |
| `build/cap_avx512_wave.zcap` | `776 B` | `ZEROZCAP` + SSE4.2 `CRC32C` | NVMe Extent `1`: Ring-0 `AVX-512` `u64x8` wave kernel inside `tile { }` (`examples/capsules/cap_avx512_wave.zr`). |
| `build/cap_matrix_crc.zcap` | `808 B` | `ZEROZCAP` + SSE4.2 `CRC32C` | NVMe Extent `2`: `CRC32C` + BMI2 `pext`/`pdep` + `rol` bit-matrix kernel (`examples/capsules/cap_matrix_crc.zr`). |
| `build/zeroos_usb.img` | `67,108,864 B` | Protective MBR + GPT + FAT32 ESP | `64 MiB` bootable USB stick containing `/EFI/BOOT/BOOTX64.EFI` (`95,232 B`). |

### Hermetic Toolchain Independence Check
- Static syscall audit of `compiler/*.zr` confirms it invokes **only** raw Linux file/process syscalls (`openat`, `read`, `write`, `lseek`, `fchmod`, `close`, `exit` — zero `execve`/`fork`/`clone`).
- Deleting `build/zero0` and re-building `zero3`, `gate1_os_z2.efi`, and all `.zcap` capsules via `build/zero2` reproduces every output file **byte-for-byte**.

---

## 2. Host Golden Suites, `objdump` Opcode Gates & 54 Negative Probes (Stages 2–5)

### 2.1 Eight Golden Execution Suites (`tests/*.zr` vs `tests/*.expected`)

| Suite | Subsystems Verified |
| :--- | :--- |
| `examples/first.zr` | Minimal 2-segment `W^X` `ELF64` emission, inline `.text` strings, `print`, exit code `0`. |
| `tests/step3_compute.zr` | Control flow, recursion, bit-ops, loops, functions. |
| `tests/step4_systems.zr` | Bit-partitioned `<a: u32 \| ...>` layouts, `@slab`, `@arena`, multi-frame `fail`/`recover` unwind. |
| `tests/phase0_scalar.zr` | Lexical `{}` scoping, unsigned `shr`/`div`/`setb..seta`, div-by-zero `fail 1`, order-independent `::` constants, `(argc, argv)`. |
| `tests/phase1_zero_lang.zr` | Structs (`<packed>`), `Struct{}` literals, `@Type` non-null pointers, `use "lib/..."` diamond modules, `[hi:lo]` bit-slices, `\|>`, `<<<`/`>>>`, `match`, `<rev>`/`<stride>`/`<unroll>`/`<bits>`. |
| `tests/phase2_zero_power.zr` | 16B slices (`RAX:RDX`), custom `@ptr<T \| mask \| wrap \| stride \| bswap>`, `@stream` (`movnti`), `@atomic` (`xchg`, `lock xadd`, `lock cmpxchg`), `@fn_addr`, `@code_bytes`, `@call`, `@emit`, BMI1/2 + `crc32c` + fences. |
| `tests/phase3_reactor_fabric.zr` | `@cell64` (`64 B` aligned), `@ring`, `@dma_queue` (`4 KiB` aligned), `publish` (`movdir64b`), `@movdiri`, `@cpuid_ecx`. |
| `tests/phase4_avx512_simd.zr` (`Track 2`) | `@sizeof`/`@alignof(u32x16\|u64x8) == 64`, full ZMM ALU (`+ - * & \| ^ << >>` imm/scalar/per-lane `vpsllvq`), vector args/returns, overflow-safe `@vec_sum32` (`16 * 0x40000000 == 17179869184`), `{k1}{z}` loads & `{k1}` stores preserving adjacent sentinels, `@vec_stream` (`vmovntdq`), `<simd: 16\|8>` loops at `N = 0, 1, 7, 19, 37`, and `<simd>` bounds fault `fail 2` caught by `recover`. |

### 2.2 Automated `objdump -d` Machine-Instruction Gates

`tests/run_tests.sh` disassembles compiled binaries and requires exact hardware instructions:
- **Direct Silicon (`phase2` & `phase3`):** `movnti`, `lock xadd`, `lock cmpxchg`, `popcnt`, `lzcnt`, `tzcnt`, `pdep`, `pext`, `crc32`, `prefetcht0`, `bswap`, `sfence`, `lfence`, `mfence`, `movdir64b`, `movdiri`, `cpuid`.
- **`EVEX` `AVX-512` (`phase4` & `cap_avx512_wave.zcap`):** `vmovdqu32`, `vmovdqu64` (with `EVEX` compressed `disp8 * 64`), `{%k1}{z}`, `{%k1}`, `kmovw`, `bzhi`, `vmovntdq`, `vpbroadcastd`, `vpbroadcastq`, `vpaddd`, `vpaddq`, `vpmulld`, `vpmullq`, `vpsllvq`, `vzeroupper`.
- **Bare-Metal `PE32+` (`gate1_os_z2.efi`):** `movdir64b`, `monitor`, `mwait`, `iretq`, `vmovntdq`, `vzeroupper`, `{%k1}{z}`, `kmovw`, `sfence`, `crc32`, `%gs:0x0`, `%gs:0x8`, and `0` `WRITE+EXECUTE` PE sections.

### 2.3 Taxonomy of All 54 Negative Compile-Rejection Probes

Every one of the 54 negative probes is verified to fail compilation cleanly with a non-zero exit code and diagnostic:
1. **Honesty Gates on Unlowered Constructs:** `@cold` struct attribute, `<tile: 64>` sized cache-blocking, non-power-of-2 `<unroll: 3>`, invalid `<simd: 3>`, non-vectorizable calls inside `<simd>`.
2. **Ring-0 Zero-XSAVE `AVX-512` Safety Gates:** `u32x16`/`u64x8`, `@vec_*`, or `<simd>` outside `tile { }` on `--target=efi-os` and `--target=capsule`; any vector operation inside `@isr` or `@isr_ok`.
3. **Interrupt & Reactor Isolation Gates:** Calling non-`@isr_ok` functions from `@isr`, Ring-0 intrinsics (`@lgdt`, `@wrmsr`, `wait`, `@reactor_id`) on host `ELF64`, mutable `@slab`/`@ring` declarations inside `--target=capsule`, `syscall`/`print` on bare-metal targets.
4. **Type, Constant & Layout Integrity Gates:** Cyclic `::` constants, duplicate definitions, invalid bitpack bit sums (`!= 8, 16, 32, 64`), non-power-of-2 `@ring`/`wrap`, type mismatches across slices/pointers/structs, and out-of-scope lexical variable access.

---

## 3. Live Bare-Metal `ZeroOS` Telemetry & Multi-Core Verification (Stage 6)

Under `QEMU Q35 + KVM (-cpu host,+invtsc -overcommit cpu-pm=on) + OVMF + NVMe` (`4` cores, `3` NVMe `.zcap` extents), `run_tests.sh` injects PS/2 keystrokes `1`, `2`, `3`, `b`, `a`, captures a framebuffer screendump, and injects `q`. The exact debugcon hardware trace is:

```text
ZEROHAND W=1280 H=800 S=5120 F=0 C=4 M=20 PT=21 WX=0 ACPI=1 GS=1 FAB=1 SIMD=1 VEC=3535/7864 CYC=1685 CAP=777 KBD
CAP1=2337
CAP2=1207754
CAP3=35277270
BENCH=OK
SCAN=0x0000001E GATE1B_OK
SHUTDOWN_OK
```

### Decoding the `ZeroOS` Hardware Telemetry Line
- `W=1280 H=800 S=5120 F=0`: UEFI GOP `1280x800` `BGRX8888` framebuffer mapped `PTE_WC_NX`.
- `C=4`: BSP + `3` AP worker reactors awakened via `MADT` + `x2APIC` `INIT-SIPI` over the `< 1 MiB` `PTE_RX` trampoline.
- `PT=21 WX=0`: `21` 4 KiB page-table pages allocated out of `pt_pool`; exhaustive 4-level `@PtePtr` walk confirms **zero `W+X` pages**.
- `ACPI=1 GS=1 FAB=1 SIMD=1`: `MADT`/`MCFG` valid, per-core `gs:0` `recover` verified (`fail 88` caught cleanly), cross-core `MOVDIR64B`/`MWAIT` round-trip (`CYC=1685` TSC cycles), and Ring-0 `AVX-512` `u64x8` `<tile | simd: 8>` benchmark (`VEC=3535` vector cycles vs `7864` scalar cycles $\approx \mathbf{2.2\times}$ speedup over `515` `u64` elements including `{k1}` 3-lane tail!).
- `CAP=777`: Boot-time NVMe DMA + SSE4.2 `@crc32c` + `seal_capsule_wx_rx` (`PTE_RX`) verification of Extent `0`.

### Multi-Core Worker Capsule Equivalence (Hardware vs Independent Reference Model)

An independent Python bit-exact reference model implementing `x86-64` `crc32 r64`, BMI2 `pext`/`pdep`, 64-bit `rol`, and `u64x8` lane arithmetic verifies exact mathematical agreement across `4`, `2`, and `1` CPU cores:

| Core Topology (`-smp`) | `CAP1` (Extent 0 Compute) | `CAP2` (Extent 1 `AVX-512` Wave) | `CAP3` (Extent 2 `CRC32C`+`BMI2` Matrix) | Execution Path |
| :---: | :---: | :---: | :---: | :--- |
| **`4` Cores** | `2337` | `1207754` | `35277270` | Parallel `MOVDIR64B` fan-out across Worker Reactors `1..3` |
| **`2` Cores** | `778` | `392326` | `4019694` | `MOVDIR64B` dispatch to Worker Reactor `1` |
| **`1` Core** | `777` | `381953` | `978462` | Single-core fallback directly on BSP Reactor `0` |

### Framebuffer, Interrupt & Graceful-Degradation Checks
- **Pixel-Exact GOP Audit:** Verifies `AVX-512` `vmovntdq` gradient corners & 16-pixel boundaries, all `6` status LEDs, `4` active core pips (and absence of a 5th pip), top HUD bar, cyan Command Deck frame, and `> [3] EXT2 MATRIX_CRC` active row highlight.
- **Real `IRQ1` Interrupt Count:** After keys `1`, `2`, `3`, `b` (`4` make + `3` break scancodes at screendump instant), the HUD `IRQS:` counter reads **`7`**, proving true interrupt-driven `I/O APIC` `IRQ1 -> 0x21` wakeups from `MWAIT`.
- **Tamper & Hardware-Absence Safety:** Corrupting a capsule's CRC32C halts immediately before `PTE_RX` sealing; booting without NVMe reports `CAP=0` cleanly without hanging; booting `-vga none` writes `0` bytes to `0xE9` and returns cleanly to UEFI firmware.

---

## 4. Bootable Physical Laptop USB Image Audit (Stage 7, `make usb-img`)

`scripts/build_usb_image.py` constructs `build/zeroos_usb.img` (`67,108,864 B` = `131,072` 512-byte sectors):

```text
LBA Range          Size       Structure & Verification Status
───────────────────────────────────────────────────────────────────────────────────────
LBA 0              512 B      Protective MBR (Type 0xEE spanning LBA 1..131071)
LBA 1              512 B      Primary GPT Header ("EFI PART", valid Header & Array CRC32)
LBA 2..33          16 KiB     Primary GPT Partition Entries:
                              Entry 0 = EFI System Partition (C12A7328-F81F-11D2-BA4B-00A0C93EC93B)
LBA 2048..131007   ~63 MiB    FAT32 Volume (126,912 clusters, mirrored FAT1 == FAT2):
                              └── /EFI/BOOT/BOOTX64.EFI (95,232 B == build/gate1_laptop_z2.efi)
LBA 131039..131071 16.5 KiB   Backup GPT Partition Entries + Backup GPT Header at LBA 131071
```

- Verified clean by `sfdisk --verify`, an independent FAT32 cluster-chain extractor (`cmp -s extracted.efi gate1_laptop_z2.efi`), and live OVMF boot from an emulated PCIe xHCI `usb-storage` thumb drive (`~1.4 s` to interactive `1280x800` Command Deck, `0` bytes on port `0xE9`).

---

## 5. Boundary Between Verified Baseline & Upcoming Stages

| Milestone | Current Status |
| :--- | :--- |
| **Tracks 1–3 (`zero2 == zero3` fixed point, Ring-0 `AVX-512`, SMP Reactor Fabric, NVMe `ZEXTENT1` Capsules, `IRQ1` Command Deck, USB Image)** | **100% Implemented & Verified (`78 PASS / 0 FAIL`)** |
| **Stage 1 — Next-Gen `Zero` Language Spine (`ptr*`, `ref&`, `ptr* @oneshot`, `;` flag-handoff, `mask<...>`, `when` / `with when` / `when while` / `when for`, `@ring(s..n)`, `@bits`)** | **Frozen Spec in `ZERO_NEXT_GEN_SPEC.md` §2 — Ready for Implementation** |
| **Stage 2 — Non-POSIX Hardware Domains & `ZeroFS` (`ZLEDGER1`) CoW Storage Engine** | **Frozen Spec in `ZERO_NEXT_GEN_SPEC.md` §3 — Follows Stage 1** |
| **`<tile: Size>` Cache-Blocking, `@cold` Struct Splitting, `Iris Xe` `CCS0` GPU Compute (Gate 4), Physical Laptop Bare-Metal Boot Run** | **Unimplemented (Rejected at Compile Time / Future Hardware Gate)** |
