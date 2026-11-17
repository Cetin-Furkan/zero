# `Zero` — Verification & Milestone Report (Phase 0 -> Gate 3 + Self-Hosting)

This report summarizes the verified implementation state across `/home/arch-cetin/projects/zero`, tested end-to-end via `make clean && make test`.

---

## 1. Stage-0 -> Stage-3 Deterministic Self-Hosting Proof (`Phase 4`)

`compiler/` implements the modular Stage-1 `Zero` compiler in pure `.zr` (`compiler/{zero1,defs,lexer,parser_expr,emit_x64,containers}.zr`) with zero C, zero LLVM, zero libc, zero heap/malloc, and zero external linker:

| Artifact | Size (Bytes) | Format & Segments | Verification |
| :--- | :---: | :--- | :--- |
| **`build/zero0`** | `48,112` | 2-segment `W^X` `ELF64` (`R E` + `RW`) | Assembled directly from `bootstrap/*.{asm,inc}` via `nasm -f bin` |
| **`build/zero1`** | `208,755` | 2-segment `W^X` `ELF64` (`R E` + `RW`) | Compiled from `compiler/zero1.zr` by `build/zero0` |
| **`build/zero2`** | `203,241` | 2-segment `W^X` `ELF64` (`R E` + `RW`) | Compiled from `compiler/zero1.zr` by `build/zero1` |
| **`build/zero3`** | `203,241` | 2-segment `W^X` `ELF64` (`R E` + `RW`) | Compiled from `compiler/zero1.zr` by `build/zero2` (**`cmp -s zero2 zero3` -> 0 diff**) |
| **`build/capsule_z2.zcap`** | `616` | `ZEROZCAP` + `CRC32C` + Reactor ABI | Compiled by self-hosted `build/zero2 --target=capsule`, executed on bare-metal NVMe (`CAP=777`) |

---

## 2. Bare-Metal `ZeroOS` Hardware Gates (`Gates 1a, 1b, 2, & 3`)

Booted and verified under `QEMU Q35 + KVM + OVMF + NVMe` (`-cpu host -overcommit cpu-pm=on -smp 4`):

1. **Gate 1a (Hardened BSP Boot & `W^X` Paging):**
   - Pre-`ExitBootServices` GOP (`BGRX8888`/`RGBX8888`/`PixelBitMask`), ACPI `RSDP -> XSDT/RSDT -> MADT/MCFG` harvest, TSC calibration, `<packed>` `Tss64` (`iomap_base = 104`), `PKRU = 0`, `XCR0 = 0xE7`, `IA32_PAT` (`WC`), `IA32_EFER.NXE`, and 4-level `CR3` page tables verified at runtime (`WX=0`).
2. **Gate 1b (`MADT` + `x2APIC` Multi-Core Bring-Up & Non-Temporal GOP HUD):**
   - Isolated `< 1 MiB` `PTE_RX` AP startup bridge (`bootstrap/ap_trampoline.inc`, `> 4 GiB` `CR3` safe), `MADT` (`Type 0`/`Type 9`) parsing, `x2APIC` `INIT-SIPI` wakeup into 64-bit `.zr` `fn ap_main(slot)`, `<unroll: 4>` + `@stream<u32>` (`movnti`) GOP scanout, and crisp 5x7 bitmap HUD.
3. **Gate 2 (Pinned `r12..r15` + `gs:0` Reactor Fabric, `MOVDIR64B`, `MWAIT`, `<tile>`, & `@isr`):**
   - Reactor ABI (`arg3 = r10`, `r12`=`@reactor_id`, `r13`=`@inbox_base`, `r14`=`@scratch_base`, `r15`/`gs:0`=`ReactorLocal`), concurrent per-core `gs:0` fault isolation, 64B `@cell64` + power-of-2 `@ring`/`@dma_queue`, `publish` (`movdir64b`), Intel SDM §9.10.4 race-free `wait` (`monitor` + recheck + `mwait` + `IA32_TSC_DEADLINE`), 64B-aligned `<tile>` checkpoints with unwind across `return`/`break`/`continue`/`recover`, and statically verified GPR-only `@isr` (`iretq`, 0 XSAVE) & leaf `@isr_ok`.
4. **Gate 3 (PCIe `MCFG`/`ECAM` + Lock-Free NVMe `@dma_queue` + Live `@capsule` Execution):**
   - Discovers NVMe (`01h:08h:02h`) via `MCFG`/`ECAM`, binds Admin & I/O `@dma_queue`s, queries `Identify Namespace 1` (`LBADS`), DMAs the `ZEXTENT1` Master Extent Table and `build/capsule_z2.zcap` into `@capsule boot_capsule`, verifies hardware SSE4.2 `@crc32c`, flips capsule PTEs from `PTE_RW_NX` to `PTE_RX` (`WX=0`), catches `fail 88` via `gs:0` `recover`, and executes `@call(entry, 40, 2)` returning **`CAP=777`**:
   ```text
   ZEROHAND W=1280 H=800 S=5120 F=0 C=4 M=20 PT=20 WX=0 ACPI=1 GS=1 FAB=1 CYC=1741 CAP=777 KBD
   SCAN=0x0000001E GATE1B_OK
   ```

---

## 3. True-Zero Language Capabilities Verified in `tests/run_tests.sh`

- **Modules & Slices:** `use "rel/path.zr"` (canonical `./`/`../` + `crc32` deduplication), `str`/`bytes`/`[]T` 16B `{ base, len }` bounded slices, `s[lo..hi]`, and inline RIP-relative `"..."` literals.
- **Programmable Hardware Pointers & Memory:** Custom `@ptr<T | mask | wrap | stride | bswap>`, `@stream<u32|u64>` (`movnti`), `@atomic<u32|u64>` (`xchg`, `lock xadd`, `lock cmpxchg`), `@code_bytes(fn)`, `@fn_addr(fn)`, `@call(ptr, ...)`, and `@emit(...)`.
- **Loops, Syntax & Silicon Ops:** `for [i,] x in span`, `<tile>`, `<bits>`, `<rev>`, `<stride: S>`, `<prefetch: D>`, `<unroll: 2|4|8>`, `[hi:lo]` bit-slices, `=>` arrow functions, `|>` pipelines, `<<<`/`>>>` rotates, `match`, and inline `@popcnt`, `@lzcnt`, `@tzcnt`, `@bswap16/32/64`, `@pdep`, `@pext`, `@crc32c`, `@prefetch`, `@clflush`, `@lfence`, `@sfence`, `@mfence`.
- **Adversarial Negative Discipline:** 39 negative compile-time rejection probes across `ELF64`, `--target=efi-os`, and `--target=capsule` ensure unlowered lenses (`<simd>`, `<tile: 32KB>`), `@cold`, invalid `@isr` clobbers, and target leaks remain hard compile-time errors.
