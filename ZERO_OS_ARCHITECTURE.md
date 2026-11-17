# `ZeroOS` — Hardware-Verified Bare-Metal Architecture (`i7-1165G7`)

> **`ZeroOS`** is a single-image, reactor-driven Ring-0 operating system co-designed with the **`Zero` (`.zr`)** language and self-hosted compiler (`compiler/zero1.zr`) for this exact reference machine: **HP Intel Core i7-1165G7 (Tiger Lake Willow Cove, 4C/8T, `39-bit` physical / `48-bit` virtual addressing, `Iris Xe` Gen12LP, PCIe NVMe, `i8042` keyboard, `ELAN0712` LPSS I2C touchpad)**.
>
> Every instruction and architectural rule in this document is verified against `/proc/cpuinfo`, `/proc/bus/input/devices`, and live `QEMU Q35 + KVM + OVMF + NVMe` hardware execution on the host machine.

---

## 1. Verified Silicon Capabilities & Constraints (`i7-1165G7`)

| Hardware Feature | Verified Host Status | Architectural Role in `ZeroOS` |
| :--- | :--- | :--- |
| **CPU Topology & Caches** | `4C / 8T`, `48 KiB` L1D, `1.25 MiB` L2, `12 MiB` L3 | **Pinned permanent reactors** (`1` per hardware thread; zero thread migration). |
| **`AVX-512` (`F/BW/DQ/VL/VBMI2/VNNI`)** | **Present** (`XCR0 = 0xE7` unlocked at boot) | First-class in Ring 0; safe without `2.5 KiB` ISR saves via the **`<tile>` checkpoint rule**. |
| **`MOVDIR64B` & `MOVDIRI`** | **Present** | Backing instructions for `publish(slot_addr, cell64)` (`64B` atomic direct store without sender `RFO`) and `@movdiri`. |
| **Idle / Cacheline Wait** | **`monitor` (`MWAIT`) Present**; **`waitpkg` (`umwait`/`tpause`) Absent** | `wait(addr, deadline [, expected_old])` lowers to Intel SDM §9.10.4 race-free **Ring-0 `MONITOR` + recheck + `MWAIT`** + `IA32_TSC_DEADLINE` (with `CPUID.01H:ECX[3]` hypervisor fallback to `PAUSE`). |
| **Paging (`pdpe1gb`, `39b` phys / `48b` virt)** | **Present** (4-level `PML4`; no 5-level `la57`) | Usable DRAM mapped `WB+RW+NX`; `.text`, `<1 MiB` AP bridge, and sealed `@capsule` extents mapped `WB+RX`; ACPI mapped `RO+NX`; GOP mapped `WC+NX`; PCIe `ECAM` / NVMe BAR0 mapped `UC+NX` (`WX=0` verified). |
| **Protection Keys (`pku`, `ospke`)** | **Present** (`16` keys; `PKRU = 0` initialized on all cores) | Ring-0 compiler seatbelt catching stray stores into frozen spaces. |
| **`CAT_L2`, `x2APIC`, `TSC-Deadline`, `SSE4.2 CRC32C`** | **All Present** | Register-mode `x2APIC` (`MSR 0x800+`), tickless `IA32_TSC_DEADLINE` (`MSR 0x6E0`), and hardware `crc32` (`@crc32c`) `@capsule` integrity verification. |
| **Built-In Input & Storage** | Keyboard: `isa0060` (`0x60`/`0x64` `@port`); Storage: PCIe `NVMe` (`01h:08h:02h` via `MCFG`/`ECAM`) | Day-1 keyboard uses typed `@port` I/O; storage uses lock-free `@dma_queue` NVMe submission/completion rings. |

---

## 2. Boot, Multi-Core Takeover & Reactor Fabric (`zero.efi` — Gates 1a, 1b, 2 & 3 Verified)

1. **Gate 1a — Hardened Single-Core BSP Boot (Implemented & Verified):**
   - `PE32+` Entry (`efi_main` on BSP Core 0) queries **GOP** (`BGRX8888`, `RGBX8888`, or `PixelBitMask`; returns cleanly to firmware if GOP is absent), walks **ACPI `RSDP -> XSDT/RSDT`** *before* `ExitBootServices` to harvest physical **`MADT`** and **`MCFG`** addresses, calibrates `tsc_per_ms`, allocates `< 0xA0000` AP startup pages + `256 KiB` AP stacks, harvests `MemoryMap`, and calls **`ExitBootServices()`**.
   - Post-exit modular `ZeroOS` (`examples/gate1_os.zr` + `examples/os/*.zr`) installs per-core `GDT` + `<packed>` `Tss64`, unlocks `XCR0 = 0xE7` + `PKRU = 0`, programs `IA32_PAT` (`WC`) and `IA32_EFER.NXE`, builds strict hardware **`W^X` 4-level `CR3` page tables** (`verify_no_wx_pages() == 0` -> `WX=0`), and installs a 256-entry scalar `IDT`.
2. **Gate 1b — `MADT` + `x2APIC` `INIT-SIPI` Multi-Core Bring-Up (Implemented & Verified):**
   - Because EDK2's `MpInitChangeApLoopCallback` broadcasts `INIT` during `ExitBootServices()`, `bootstrap/ap_trampoline.inc` provides an isolated `< 1 MiB` `PTE_RX` startup bridge (`>4 GiB` `CR3` safe) while keeping `Zero` 100% 64-bit.
   - `smp_madt.zr` parses `MADT` (`Type 0` & `Type 9`), wakes every AP via `x2APIC` `INIT-SIPI` into 64-bit `.zr` `fn ap_main(slot)` with dedicated `16 KiB` stacks, per-core `GDT`/`TSS`/`XCR0`/`PKRU`/`x2APIC`, and verifies online synchronization (`C=4` / `C=8`, plus `C=1` fallback).
3. **Gate 2 — Pinned `r12..r15` + `gs:0` Reactor Fabric, `MOVDIR64B`, `MWAIT`, `<tile>`, & `@isr` (Implemented & Verified):**
   - `--target=efi-os` and `--target=capsule` use `rdi, rsi, rdx, r10, r8, r9`, pin `r12` (`@reactor_id`), `r13` (`@inbox_base`), `r14` (`@scratch_base`), and `r15` / `gs:0` (`ReactorLocal`), maintain 64-byte `RSP` alignment at `<tile>` checkpoints, unwind `[gs:8]`/`[gs:16]` across `return`/`break`/`continue`/`recover`, statically enforce GPR-only `@isr` (`iretq`, 0 XSAVE) & leaf `@isr_ok`, and measure live cross-core `MOVDIR64B` -> `MWAIT` -> `<tile>` -> `MOVDIR64B` round-trip latency (`CYC:~1428` cycles on 4-core KVM `cpu-pm=on`).
4. **Gate 3 — PCIe `MCFG`/`ECAM` + Lock-Free NVMe `@dma_queue` + Self-Hosted `@capsule` Engine (Implemented & Verified):**
   - `nvme_capsule.zr` parses ACPI `MCFG`, maps PCIe `ECAM` & NVMe `BAR0` dynamically as `PTE_UC_NX` (`WX=0`), binds 4-KiB aligned `@dma_queue` Admin (`ASQ`/`ACQ`) and I/O (`SQ1`/`CQ1`) rings, queries `Identify Namespace 1` (`LBADS`), DMAs the Master Extent Table (`ZEXTENT1` at LBA 0) and `--target=capsule` `.zcap` image (compiled by self-hosted `build/zero2`!) straight into `@capsule boot_capsule`, verifies hardware SSE4.2 `@crc32c` against both superblock and capsule headers (`@halt()` on tamper), flips capsule PTEs from `PTE_RW_NX` to `PTE_RX` (`seal_capsule_wx_rx`, re-verifying `WX=0`), catches `fail 88` via `gs:0` `recover`, and executes `@call(entry_fn, 40, 2)` returning **`CAP=777`** on the live GOP HUD!

---

## 3. Storage & Immutable `@capsule` Container (`--target=capsule`)

`ZeroOS` replaces hierarchical VFS dentries, `FAT` chains, and `ext4` journals with two flat hardware structures:
1. **Master Extent Table (`MasterExtentSuper` `'ZEXTENT1'` + 32-Byte `MasterExtentEntry` Array at LBA 0):**
   - Maps `id_hash -> (start_byte, byte_len, crc32c, flags)` in `O(1)` without path traversal.
2. **Position-Independent `.zcap` `@capsule` (`ZeroCapsuleHeader` `'ZEROZCAP'`):**
   - 64-byte header (`magic`, `version=1`, `entry_off=64`, `code_bytes`, hardware SSE4.2 `crc32c`) followed by pure `RX` Reactor-ABI machine code (`r10` arg3, `r12` `@reactor_id()`, `gs:0` scalar fault word, `<tile>` `gs:8`/`gs:16` checkpoints, inline RIP-relative literals, zero mutable `.bss`).
   - Emitted identically by both Stage-0 `build/zero0 --target=capsule` and the self-hosted Stage-2 `.zr` compiler `build/zero2 --target=capsule`.

---

## 4. Bring-Up Progression Summary

- **Gate 1a & 1b (Completed & Verified):** Standalone `PE32+` `zero.efi`, GOP `movnti` scanout, pre-exit ACPI `MADT`/`MCFG`, strict hardware `W^X` `CR3` (`WX=0`), `<1 MiB` `PTE_RX` AP trampoline, `x2APIC` multi-core bring-up, and `@port` keyboard input.
- **Gate 2 (Completed & Verified):** Pinned `r12..r15` + `gs:0` Reactor ABI, `@cell64`, `@ring`, `publish` (`movdir64b`), race-free `wait` (`monitor`/`mwait`), `<tile>` quanta with full unwind safety, and zero-XSAVE `@isr`/`@isr_ok`.
- **Gate 3 & Phase 4 Self-Hosting (Completed & Verified):** Bare-metal PCIe `MCFG`/`ECAM` + NVMe `@dma_queue` driver executing `CRC32C`-verified, `W^X`-sealed `.zcap` capsules compiled by the deterministic self-hosted `.zr` compiler (`zero0 -> zero1 -> zero2 == zero3`, `CAP=777`).
- **Gate 4 (Future — `Iris Xe` `CCS0` Compute Submit):** Minimal `FORCEWAKE` + `PPGTT` + `CCS0` `COMPUTE_WALKER` writing directly into the GOP scanout buffer.
