# `ZeroOS` — Hardware-Verified Bare-Metal Architecture (`i7-1165G7`)

> **`ZeroOS`** is a single-image, reactor-driven Ring-0 operating system co-designed with the **`Zero` (`.zr`)** compiler for this exact reference machine: **HP Intel Core i7-1165G7 (Tiger Lake Willow Cove, 4C/8T, `39-bit` physical / `48-bit` virtual addressing, `Iris Xe` Gen12LP, PCIe NVMe, `i8042` keyboard, `ELAN0712` LPSS I2C touchpad)**.
>
> Every instruction and architectural rule in this document is verified against `/proc/cpuinfo` and `/proc/bus/input/devices` on the host machine. Unmeasured latency, wattage, and line-count slogans have been removed.

---

## 1. Verified Silicon Capabilities & Constraints (`i7-1165G7`)

| Hardware Feature | Verified Host Status | Architectural Role in `ZeroOS` |
| :--- | :--- | :--- |
| **CPU Topology & Caches** | `4C / 8T`, `48 KiB` L1D, `1.25 MiB` L2, `12 MiB` L3 | **8 permanent pinned reactors** (`1` per hardware thread `0..7`; zero thread migration). |
| **`AVX-512` (`F/BW/DQ/VL/VBMI2/VNNI`)** | **Present** (`XCR0` unlock required at boot) | First-class in Ring 0; safe without `2.5 KiB` ISR saves via the **`<tile>` checkpoint rule**. |
| **`MOVDIR64B` & `MOVDIRI`** | **Present** | Backing instruction for `publish(ring, cell64)` (`64B` atomic direct store without sender `RFO`). |
| **Idle / Cacheline Wait** | **`monitor` (`MWAIT`) Present**; **`waitpkg` (`umwait`/`tpause`) Absent** | Idle lowers to **Ring-0 `MONITOR`/`MWAIT`** (`C1` light wait vs deeper `C`-states) or `HLT` + `IA32_TSC_DEADLINE`. `umwait` does not exist on Willow Cove and is excluded from the grammar. |
| **Paging (`pdpe1gb`, `39b` phys / `48b` virt)** | **Present** (4-level `PML4`; no 5-level `la57`) | Usable DRAM ranges mapped `WB` (`2 MiB` / `1 GiB` where hole-free); MMIO (`UC`) and GOP/GPU apertures (`WC` via `IA32_PAT`) mapped strictly outside `WB` DRAM ranges. |
| **Protection Keys (`pku`, `ospke`)** | **Present** (`16` keys; `WRPKRU` serializing) | Optional Ring-0 compiler seatbelt catching stray stores into frozen `@arena`s / `@capsule`s (not a hostile security boundary). |
| **`CAT_L2`, `x2APIC`, `TSC-Deadline`, `HWP`, `Intel PT`** | **All Present** | Register-mode `x2APIC` (`MSR 0x800+`), tickless `IA32_TSC_DEADLINE` (`MSR 0x6E0`), `IA32_HWP_REQUEST` (`MSR 0x774`), and L2 way masks. |
| **Built-In Input Buses** | Keyboard: `isa0060` (`0x60`/`0x64` `@port`); Touchpad: `i2c-ELAN0712` (`MMIO`) | Day-1 keyboard input uses `@port` I/O (`in`/`out`), distinct from `@mmio`. |

---

## 2. Boot & Multi-Core Takeover Without 16-Bit Code (`zero.efi`)

Starting Application Processors (APs) *after* `ExitBootServices()` via raw `x2APIC` `INIT`/`SIPI` forces APs to wake in **16-bit Real Mode** below `1 MiB`, which would pollute the project with a 16->32->64-bit assembly trampoline.

`ZeroOS` keeps 16-bit code completely out of the compiler and OS via a **two-subgate UEFI take-over**:

1. **Gate 1a — Hardened Single-Core BSP Boot (Implemented & QEMU/OVMF/KVM-Verified):**
   - `PE32+` Entry (`efi_main` on BSP Core 0, already in 64-bit Long Mode) queries **GOP (`GraphicsOutputProtocol`)** (`BGRX8888`, `RGBX8888`, or bitmask framebuffer base, dimensions, and byte stride; cleanly returns to firmware without guessing if GOP is absent).
   - Queries **ACPI `RSDP`** and walks `XSDT`/`RSDT` *before* `ExitBootServices` to harvest physical addresses of **`MADT` (`'APIC'`)** and **`MCFG` (`'MCFG'`)** into `BootHandoff`.
   - Records the image's executable `.text` span (`code_base`, `code_bytes`), classifies UEFI `MemoryMap` descriptors (`Conventional`/Loader `1..4, 7` as `WB`, ACPI `6, 9, 10` as read-only `ACPI RO`, `MMIO 11` as `UC`, GOP framebuffer as `WC`), calls **`ExitBootServices()`**, and sets `core_count = 1`.
   - Post-exit `examples/gate1_os.zr` (written using `<packed>`, `@port`, `@view<T>`, `@mmio<T>`, `@wrpkru`, and `@halt`) installs 64-bit `GDT`/`TSS` (`iomap_base = sizeof(Tss64)`), unlocks `XCR0 = 0xE7` + `PKRU = 0`, programs `IA32_PAT` (`WC`) and `IA32_EFER.NXE`, builds strict hardware **`W^X` 4-level `CR3` page tables** (`RX` `.text`, `RO+NX` ACPI `MADT`/`MCFG`, `RW+NX` DRAM, `WC+NX` GOP, `UC+NX` MMIO, verified at runtime via `verify_no_wx_pages()`), installs a 256-entry scalar `IDT`, paints checkpoint LEDs + deterministic gradient onto the GOP scanout, and reads `0x60`/`0x64` `@port` PS/2 scancodes.
2. **Gate 1b — Optional Pre-Exit Long-Mode AP Park (Next Stage):**
   - Calls **`EFI_MP_SERVICES_PROTOCOL`** *before* exiting boot services (when present; falling back cleanly to `core_count = 1` if firmware omits it) to wake AP threads directly in 64-bit Long Mode, parking each AP in a spin-wait loop with `cli`.
   - After BSP installs `IDT`, `x2APIC`, and `IA32_TSC_DEADLINE`, flips the aligned release word releasing all parked APs straight into per-core Ring-0 stacks, `CR3`, `XCR0`, `PKRU = 0`, and pinned reactor loops (`r12..r15`).

The compiler provides `@cli`/`@sti`, `@halt`, `@wrpkru`, `@lgdt`, `@ltr`, `@lidt`, `@cr_read`/`@cr_write`, `@msr_read`/`@msr_write`, `@xsetbv`, `@isr_stub_base`, typed `@port` (`in`/`out`), and `@mmio<T>` for this bring-up (with `mwait`/`movdir64b` deferred to Gate 2).

---

## 3. Storage: `@capsule` Monoliths & Mutable Erase-Block Extents

`ZeroOS` eliminates hierarchical VFS paths, directory dentries, `FAT` chains, `ext4` journals, and `4 KiB` Copy-on-Write page scattering:

1. **Master Extent Table (`O(1)` Array of 64-Byte Records):**
   - Stored in mirrored `2 MiB`-aligned super-extents at the start of the partition and held in RAM after boot as ordinary typed data (`id_hash`, `start_lba`, `byte_len`, `generation`, `crc32c`, `flags`, `name`).
2. **`@capsule` (Immutable `2 MiB`-Aligned Image):**
   - Every program or read-only asset bundle is **one single physically contiguous extent** (`code + Gen12 kernels + read-only slabs`). `@rel` offsets resolve purely within that capsule and stream into RAM in a single contiguous NVMe DMA read.
3. **Mutable State Extents:**
   - Persistent mutable state is stored in dedicated `2 MiB`-aligned extents written sequentially across full NAND erase-block boundaries and committed by atomically incrementing `generation` in the 64-byte Master Extent record.

---

## 4. Communication Surface: No Sockets, No Pipes, No Byte-Stream IPC

All communication on `ZeroOS` uses **three typed hardware mechanisms** and nothing else:

1. **Reactor ↔ Reactor (`@cell64` + `publish` via `MOVDIR64B`):**
   - Fixed `8 × 8` SPSC `@ring` matrix of `64-byte` `@cell64` slots. `publish` emits `movdir64b` directly into the destination reactor's slot, waking a sleeping target reactor whose Ring-0 `MONITOR`/`MWAIT` is armed on that cacheline without an IPI when in `MWAIT` snoop range (falling back to a register `x2APIC` `SELF`/`IPI` write only when the target core is in a non-snooping deep state).
2. **CPU ↔ PCIe Bus Masters (`@dma_queue` + `@mmio<uc>` Doorbells):**
   - Physically contiguous power-of-2 rings (`NVMe` `SQ1..8`/`CQ1..8`, one pair per reactor for lock-free storage I/O).
3. **Device → Device Chaining (`enqueue` + `MI_SEMAPHORE_WAIT`):**
   - Enqueues an Intel Gen12 `MI_SEMAPHORE_WAIT` (`0x1C`) packet ahead of a compute walker in `CCS0` watching a physical/PPGTT fence word written by NVMe or a reactor.

---

## 5. The `<tile>` Scheduler & Interrupt Law

A preemptive OS must save ~`2.5 KiB` of `AVX-512` state on any interrupt that might switch threads, which is why traditional kernels ban vector instructions in Ring 0. `ZeroOS` solves this with **one compiler-enforced language contract**:

1. **8 Pinned Reactors, Zero Preemptive Task Switching, Zero Periodic Timer Tick:**
   - Hardware interrupts (`@isr`: keyboard `IRQ1`, VBlank, error faults) execute with a **compiler-verified fixed scalar GPR-only clobber set**. The compiler rejects any `@isr` that touches `ZMM` registers or pinned reactor registers (`r12..r15`).
   - An `@isr` **never switches tasks** — it only acknowledges hardware and posts scalar state / ring flags.
2. **Tile Checkpoints Are the Only Scheduling Boundaries:**
   - Reactor work runs in bounded `<tile>` quanta. At the end of a `<tile>`, all `ZMM0..31` vector registers are architecturally dead.
   - Only at that tile boundary does a reactor check `rdtsc` / pending inbox `@cell64` items and jump to the next `@cell64` continuation.
3. **Semantic `wait(addr, tsc_deadline)`:**
   - When a reactor's rings are empty, `wait` arms `IA32_TSC_DEADLINE` (if a deadline exists) and executes Ring-0 `MONITOR` + `MWAIT` on its inbox cacheline.

---

## 6. Bring-Up Progression (Realistic Staged Gates)

1. **Gate 1 (`zero.efi` Glass + Input — Split into 1a / 1b):**
   - **Gate 1a (Done & QEMU/OVMF-Verified):** BSP-only `zero.efi` (`core_count = 1`), GOP (`BGRX`/`RGBX`/`BitMask`) `WC` scanout, pre-exit ACPI `RSDP -> XSDT/RSDT -> MADT/MCFG` discovery, `ExitBootServices()`, `GDT`/`TSS`, `XCR0=0xE7`, `PKRU=0`, hardware `W^X` `CR3` via `IA32_EFER.NXE` (`RO+NX` ACPI, `RX` `.text`, `RW+NX` DRAM, `WC+NX` GOP, `UC+NX` MMIO), `IDT`, checkpoint LEDs, and `0x60` `@port` PS/2 keyboard input.
   - **Gate 1b (Next):** Optional `EFI_MP_SERVICES_PROTOCOL` pre-exit 64-bit AP park with graceful single-core fallback.
2. **Gate 2 (8-Core `MOVDIR64B` + `MWAIT` Fabric):** Release all 7 APs into pinned reactors (`r12..r15`), verify `movdir64b` `@cell64` ping-pong across cores with `MONITOR`/`MWAIT` wakeups and cycle counts measured via `rdtsc`/`rdpmc`.
3. **Gate 3 (PCIe `ECAM` + Lock-Free Per-Core NVMe `@capsule` Loader):** Enumerate NVMe via `MCFG`, initialize Admin + I/O `@dma_queue`s, and stream a `2 MiB`-aligned `@capsule` straight into DRAM.
4. **Gate 4 (`Iris Xe` `CCS0` Compute Submit & Symbiote Option):** Bring up a minimal `FORCEWAKE` + `PPGTT` + `CCS0` `COMPUTE_WALKER` writing pixels into the GOP scanout buffer; keep closed-firmware Wi-Fi 7 & SOF audio isolated in optional core-split symbiote mode rather than blocking core OS progress.
