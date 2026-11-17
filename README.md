# `Zero` (`.zr`) — Language Specification, Self-Hosted Compiler & `ZeroOS`

> **`Zero` (`.zr` source, `zero0` NASM seed -> `zero1`/`zero2` self-hosted `.zr` compiler)** is a systems programming language and direct native `x86-64` compiler designed from scratch — with **zero C, zero LLVM, zero libc, and zero external linker** — as the backbone for **`ZeroOS`**, a single-image, reactor-driven bare-metal operating system targeting **Intel 11th Gen Tiger Lake (`i7-1165G7` Willow Cove + `Iris Xe` Gen12LP + PCIe NVMe)**.
>
> **Core Engineering Rule:** The compiler never silently accepts syntax or attributes it does not lower. Any construct, memory space, field attribute, or loop lens not yet lowered to real machine instructions is a **hard compile-time error**.

---

## 1. Current Repository Tree & Implementation Status

```text
/home/arch-cetin/projects/zero/
├── README.md                          # Language specification & implementation status table
├── ZERO_OS_ARCHITECTURE.md            # Bare-metal ZeroOS boot, IRQ1/MWAIT Command Deck, NVMe @capsule fabric
├── PEER_MASTER_PLAN.md                # Phased execution record (Phase 0 -> Gate 3 -> Self-Hosting)
├── CHANGES_AND_VERIFICATION_REPORT.md # Track 1-3 verification proofs & measured telemetry
├── MESSAGE_TO_PEER_AGENT.md           # Initial technical handoff charter
├── Makefile                           # zero0 seed, self-hosted zero1/zero2, 7-stage `make test`, `make usb-img`, `make qemu`
├── bootstrap/                         # Zero-C, zero-linker x86-64 NASM Stage-0 seed (`zero0`) — only NASM in the tree
│   ├── seed0.asm                      # W^X host & guest ELF64 + `--target=efi-os` / `--target=capsule` CLI
│   └── *.inc                          # tokens, lexer, imports, codegen, efi_boot, ap_trampoline, efi/capsule writers,
│                                      # types, pointers, reactor, loops, intrinsics, views, parser
├── compiler/                          # Self-hosted `.zr` compiler (`zero0 -> zero1 -> zero2 == zero3`), 11 modules
│   ├── zero1.zr                       # Statement/lvalue compiler, prescan driver & CLI (`--lex`, `--target=efi-os|capsule`)
│   ├── defs.zr                        # Shared `G_*`/`TY_*`/`SKIND_*` constants, `@slab` arenas, interner, host I/O
│   ├── lexer.zr                       # Full `.zr` tokenizer + canonical `use "rel/path.zr"` module expander
│   ├── types_spaces.zr                # Structs/`@cell64`/BitPacks/`@ptr`, const fixpoint, all `@` spaces, `u32x16`/`u64x8`
│   ├── parser_expr.zr                 # Expressions, calls, ZMM vector operators, local-RHS register elision
│   ├── intrinsics_ptr.zr              # Ring-0/Reactor/Silicon intrinsics, `@view`/`@mmio`/`@port`/`@stream`/`@atomic`, `@vec_*`
│   ├── loops_match.zr                 # `for`/`loop` lenses incl. `<simd: 8|16>` + `k1` tails, `match`, `publish`, `wait`
│   ├── emit_x64.zr                    # x86-64 encoder (64B frames, `[gs:0]` faults, `<tile>` + `vzeroupper`, imm/local RHS)
│   ├── evex_zmm.zr                    # EVEX (`0x62`) AVX-512 emitter: ZMM0..7, `{k1}{z}`/`{k1}`, `vmovntdq`, splat/iota/sum
│   ├── efi_pe32.zr                    # Standalone PE32+ UEFI image builder (boot prelude, AP trampoline, ISR stubs, `.bss` fixups)
│   └── containers.zr                  # 2-segment W^X ELF64 & CRC32C-signed `.zcap` writers
├── examples/
│   ├── first.zr                       # Minimal standalone Linux `ELF64` `.zr` program
│   ├── capsule_compute.zr             # NVMe Extent 0 capsule (`CAP=777` boot check)
│   ├── capsules/
│   │   ├── cap_avx512_wave.zr         # NVMe Extent 1: Ring-0 AVX-512 `u64x8` wave kernel inside `tile { }`
│   │   └── cap_matrix_crc.zr          # NVMe Extent 2: CRC32C + PEXT/PDEP + rotate bit-matrix kernel inside `tile { }`
│   ├── gate1_os.zr                    # ZeroOS root: boot, W^X, SMP, NVMe, IRQ1, HUD + persistent Command Deck loop
│   └── os/
│       ├── hw_types.zr                # `BootHandoff`, `ReactorLocal`, `@cell64`, rings, `@dma_queue`s, 256 KiB `@capsule`, kbd/fabric state
│       ├── debug_io.zr                # `0xE9` telemetry (`elided when qemu_debug :: 0`)
│       ├── paging_wx.zr               # 4-level W^X paging, dynamic UC MMIO & `seal_capsule_wx_rx`
│       ├── smp_madt.zr                # MADT parser + x2APIC INIT-SIPI AP bring-up
│       ├── reactor_fabric.zr          # MOVDIR64B/MWAIT fabric, `CMD_EXEC_CAPSULE` dispatch, AVX-512 `<tile|simd:8>` bench, `@isr`
│       ├── nvme_capsule.zr            # PCIe MCFG/ECAM, NVMe `@dma_queue`s, multi-extent DMA + CRC32C + W^X sealing
│       ├── ioapic_kbd.zr              # MADT I/O APIC + ISO, 8259 mask, `@isr` IRQ1 -> vector 0x21, tickless `wait_next_scancode`
│       └── hud_render.zr              # AVX-512 `vmovntdq` GOP scanout, 5x7 font, LEDs/pips/HUD, Obsidian Command Deck
├── scripts/
│   └── build_usb_image.py             # `make usb-img`: 64 MiB GPT + FAT32 ESP with qemu_debug=0 `/EFI/BOOT/BOOTX64.EFI`
└── tests/
    ├── run_tests.sh                   # 7-stage verifier (fixed point, 8 golden suites, 54 negative probes, OS boot, USB image)
    ├── lib/                           # Multi-file `use "lib/..."` test modules
    ├── lex_showcase.zr                # Lexer token-stream verification input
    ├── step3_compute.zr / step4_systems.zr / phase0_scalar.zr / phase1_zero_lang.zr  (+ .expected)
    ├── phase2_zero_power.zr           # Slices, custom `@ptr`, `@stream`, `@atomic`, `@code_bytes`, silicon ops (+ .expected)
    ├── phase3_reactor_fabric.zr       # `@cell64`, `@ring`, `@dma_queue`, `publish`, `@movdiri`, `@cpuid_ecx` (+ .expected)
    └── phase4_avx512_simd.zr          # `u32x16`/`u64x8`, `@vec_*`, `{k1}` masks, `@vec_stream`, `<simd: 8|16>` tails (+ .expected)
```

### Honest Implementation Matrix (Self-Hosted `zero2` vs Target Spec)

| Construct | Status | Exact Lowering / Rule |
| :--- | :---: | :--- |
| **Strict `W^X` Static `ELF64` & `(argc, argv)`** | **Implemented** | `zero0`, `zero1`, `zero2`, and all guest binaries emit two `PT_LOAD` segments (`PF_R\|PF_X` code/rodata + `PF_R\|PF_W` `.bss` with `p_filesz = 0`). Zero `RWX` segments. |
| **Deterministic Self-Hosting & 100% NASM Retirement (`zero0 -> zero1 -> zero2 == zero3`)** | **Implemented** | `compiler/zero1.zr` (11 modular `.zr` files) reaches a byte-identical fixed point (`cmp -s build/zero2 build/zero3`, `508,289` bytes, strict W^X). `build/zero2` alone compiles every test suite, all three `.zcap` capsules, and the complete `ZeroOS` PE32+ image (`--target=efi-os`) — NASM is used only to assemble the `zero0` seed. With `build/zero0` deleted, `zero2` reproduces itself and the OS/capsule images byte-for-byte. |
| **Compact `x86-64` Codegen & Comptime `if`** | **Implemented** | `disp8` `[rbp+disp8]` local encoding for `[-128,127]`, 64-byte inductive `RSP` alignment (`RSP & 63 == 0` at every `<tile>` boundary), immediate-RHS `+ - << >> & *pow2` lowering, and constant-folded `if`/`else` dead-branch elimination (`qemu_debug :: 0`). |
| **Order-Independent `::` Constants & Query Intrinsics** | **Implemented** | Two-pass fixed-point resolution across bitpacks and structs; `@sizeof(T)` / `@alignof(T)` evaluate in constants and expressions. Duplicate or cyclic definitions fail compilation. |
| **Lexical `{}` Block Scoping (`:=`) & `\xHH` / Compound Ops** | **Implemented** | Variables die at closing `}` (including `for`, `loop`, and `recover` bindings). Supports `%=`, `<<=`, `>>=`, `<<<=`, `>>>=`, and `\xHH` / `\r` escapes in strings & chars. |
| **Signed vs Unsigned Scalar Rules** | **Implemented** | `i8..i64` signed; `u8..u64`, `bit`, `bool`, `mask64`, and bitpack fields unsigned; integer literals signed `i64` (truncated on `u*`/bitpack store). Unsigned `>>` emits `shr`, `/` & `%` emit `div`, and `< <= > >=` emit `setb/be/a/ae` when both operands are unsigned. Division/modulo by zero raises `fail 1`. |
| **Bit-Partitioned Layouts `<a: u32 \| ...>`** | **Implemented** | Field widths must sum to `8`, `16`, `32`, or `64` bits. `\|` separates fields only before `ident :`; `>` closes literals when not followed by a same-line expression operand. |
| **Structs `Name :: { ... } [<packed>]`, `Struct{...}`, `@Type` Params** | **Implemented** | Exact natural or `<packed>` field offsets, typed by-value `Struct{ field: val }` literals & `rep movsb` struct assignment, and non-null `@Type` pointer parameters. `@cold` is **rejected at compile time**. |
| **`@slab`, `@arena`, & `@capsule` Spaces** | **Implemented** | Disjoint virtual/physical regions (`@capsule` 4 KiB page-aligned). Every `space[i]` load/store checks `(u64)i < cap` first, raising `fail 2`. `--target=capsule` forbids mutable `@slab`/`@ring` spaces. |
| **`@view<T>(addr [, len])`, `str`, `bytes`, `[]T`, `@span<T>` & `s[lo..hi]`** | **Implemented** | First-class 16-byte `{ base, len }` bounded slices (`RAX:RDX` 2-reg slice returns, target-independent `"..."` inline RX `.text` string literals on `ELF64`, `UEFI`, and `.zcap`, and checked `s[lo..hi]` / `slab[lo..hi]` sub-slicing). |
| **`use "rel/path.zr"` Multi-File Modules** | **Implemented** | Recursive relative-path module expansion with canonical `./` & `../` directory resolution and `crc32` interner diamond-import deduplication. |
| **Programmable Custom Pointers `Name :: @ptr<T \| ...>`** | **Implemented** | Inline zero-call hardware pointer lenses combining `mask: M` (4K PTE pointers), power-of-2 `wrap: N` (ring-buffer index masking), `stride: B`, `bswap`, and `.raw`. |
| **`@stream<u32\|u64>(addr, len)` & `@atomic<u32\|u64>(addr)`** | **Implemented** | `@stream` lowers `s[i] = v` directly to cache-bypassing non-temporal `movnti` (`0F C3`). `@atomic` lowers `.load()`, `.store(v)`/`swap(v)` (`xchg`), `.add(v)`/`.sub(v)` (`lock xadd`), and `.cas(exp, des)` (`lock cmpxchg` -> `1`/`0`) inline. |
| **Live Code Pointers (`@fn_addr`, `@code_bytes`, `@call`, `@emit`)** | **Implemented** | `@fn_addr(fn)` (`lea rip`), `@code_bytes(fn)` (`bytes` slice `{ base, exact_byte_len }` over live compiled `x86-64` instructions of `fn`), `@call(fn_ptr, args...)` (`fail 3` null check + indirect `call` + fault unwind), and `@emit(b0, ...)` literal machine-byte injection. |
| **Hardware Loops (`for [idx,] x in span`, `<tile>`, `<rev>`, `<stride>`, `<unroll>`, `<prefetch>`, `<simd>`)** | **Implemented** | Direct 1- or 2-variable iteration over `str`/`bytes`/`[]T`/`@view`/`@slab`/`@arena`, cooperative `<tile>` loops, `<bits>` (`tzcnt`/`blsr`), `<rev>`, `<stride: S>`, `<prefetch: D>`, `<unroll: K>` (`K` in `{2, 4, 8}`), and `for i in lo..hi <simd: 16 \| 8>` (see AVX-512 row). `<tile: Size>` cache-blocking, non-range `<simd>`, and invalid widths remain **rejected at compile time**. |
| **True-Zero Syntax (`[hi:lo]`, `=>`, `\|>`, `<<<`/`>>>`, `match`) & Silicon Ops** | **Implemented** | Hardware bit-slice extraction `expr[hi:lo]` and RMW `lval[hi:lo] = v`; `fn f(...) T => expr`; `x \|> f(a)` pipelines; 64-bit `rol`/`ror` (`<<<`, `>>>`, `<<<=`, `>>>=`); `match x { ... }`; and inline `@popcnt`, `@lzcnt`, `@tzcnt`, `@bswap16/32/64`, `@pdep`, `@pext`, `@crc32c`, `@prefetch`, `@clflush`, `@lfence`, `@sfence`, `@mfence`. |
| **Phase-3 Reactor ABI (`r12..r15`, `gs:0`, `arg3 = r10`), `@cell64`, `@ring`, `@dma_queue`, `publish` (`movdir64b`), `@movdiri`, `wait` (`monitor`/`mwait`), `<tile>`, `@isr`/`@isr_ok`** | **Implemented** | `--target=efi-os` and `--target=capsule` use `rdi, rsi, rdx, r10, r8, r9`, pin `r12..r15` (`@reactor_id`, `@inbox_base`, `@scratch_base`, `@local_base`), isolate per-core `ReactorLocal` fault state at `gs:0`, unwind `<tile>` `[gs:8]`/`[gs:16]` across `return`/`break`/`continue`/`recover`, lower `publish` to `movdir64b` & `wait` to Intel SDM §9.10.4 race-free `monitor`/`mwait` + `IA32_TSC_DEADLINE`, and statically verify `@isr` (`iretq`, GPR-only, 0 XSAVE) & leaf `@isr_ok`. |
| **Gate-3 Bare-Metal PCIe `MCFG`/`ECAM` + Lock-Free NVMe `@dma_queue` + `@capsule` Engine** | **Implemented** | `examples/os/nvme_capsule.zr` discovers NVMe via ACPI `MCFG`/`ECAM`, binds Admin + I/O `@dma_queue` rings, queries `Identify Namespace` (`LBADS`), DMAs the Master Extent Table & self-hosted `capsule_z2.zcap`, verifies hardware SSE4.2 `@crc32c`, seals `boot_capsule` `PTE_RX` (`WX=0`), tests `gs:0` `recover` (`fail 88`), and executes `@call` (`CAP=777`). |
| **512-Bit AVX-512 (`EVEX`) Vectors `u32x16` / `u64x8`, `@vec_*`, `<simd: 8\|16>`** | **Implemented** | 64-byte first-class vector types live in `zmm0` (args spilled by address, returns in `zmm0`); `+ - * & \| ^ << >>` (imm8, scalar, and per-lane `vpsllv`/`vpsrlv`), unary `- ~`, compound `op=`; `@vec_splat32/64`, `@vec_iota32/64`, `@vec_load32/64(addr [, lanes])` (`{k1}{z}`), `@vec_store32/64(addr, v [, lanes])` (`{k1}`), `@vec_stream` (`vmovntdq`), overflow-free `@vec_sum32/64`. `<simd>` loops lower to a ZMM main loop plus one branchless `kmovw k1` masked tail pass (verified `N = 0, 1, 7, 19, 37`). **Ring-0 Zero-XSAVE law:** on `--target=efi-os`/`capsule` all AVX-512 must sit inside `<tile>` (outermost exit emits `vzeroupper`) and is banned in `@isr`/`@isr_ok`. |
| **Interactive `ZeroOS` Command Deck, IRQ1 + Multi-Capsule Fabric, USB Image** | **Implemented** | I/O APIC IRQ1 -> vector `0x21` via GPR-only `@isr`, tickless `MONITOR`/`MWAIT` (or atomic `sti; hlt` on hypervisors without MWAIT) key wait, three CRC32C-verified W^X-sealed NVMe extents dispatched across worker reactors with `MOVDIR64B` (`CMD_EXEC_CAPSULE`), live GOP Command Deck, and `make usb-img` producing a GPT + FAT32 stick image with a `qemu_debug :: 0` (zero port-`0xE9`) `BOOTX64.EFI`. |
| **`<tile: Size>` Cache-Blocking, `@cold`, `Iris Xe` `CCS0` GPU Compute** | **Target Spec (Not Yet Lowered)** | Rejected at compile time (or not yet present) until lowered to hardware instructions. |

---

## 2. Building, Testing, Booting & Making a USB Stick

```bash
make test        # from scratch: zero0 -> zero1 -> zero2 == zero3, then the 7-stage verifier (~13 s with KVM)
make usb-img     # build/zeroos_usb.img: 64 MiB GPT + FAT32 ESP, qemu_debug=0 BOOTX64.EFI (dd to a USB stick)
make qemu        # interactive 4-core Q35 + KVM + OVMF + NVMe boot with all 3 capsules (SDL window, 0xE9 on stdio)
```

`make test` (`tests/run_tests.sh`) runs every stage with the **self-hosted `build/zero2`**:

1. **Fixed point & W^X** — `zero0 -> zero1 -> zero2 == zero3`, zero `RWE` segments, `--lex`.
2. **8 golden suites** — `first`, `step3_compute`, `step4_systems`, `phase0..4` compiled by `zero2`, stdout diffed, plus `objdump` checks for inline silicon (`movdir64b`, `movdiri`, `movnti`, `lock xadd/cmpxchg`, BMI2, `crc32`, fences, `cpuid`) and AVX-512 (`vmovdqu32/64`, `{%k1}{z}`, `{%k1}`, `kmovw`, `bzhi`, `vmovntdq`, ZMM ALU).
3. **54 negative probes** across `ELF64`, `--target=efi-os`, and `--target=capsule` (unlowered lenses, target leaks, `@isr` clobbers, Ring-0 AVX-512 outside `<tile>`, …).
4. **Runtime fault exit** — unhandled `fail` exits with status `1`.
5. **Images** — three `.zcap` capsules + `gate1_os_z2.efi` (PE32+, W^X sections, required opcodes) + `qemu_debug :: 0` laptop build with zero port-`0xE9` I/O.
6. **Live OS** — 4-core Q35 + KVM + OVMF + NVMe: keys `1 2 3 b a q` must yield exactly `CAP1=2337`, `CAP2=1207754`, `CAP3=35277270`, `BENCH=OK`, `SCAN=0x0000001E GATE1B_OK`, `SHUTDOWN_OK`, with GOP/LED/HUD/Command-Deck pixel checks; plus `-smp 1` with NVMe (`777 / 381953 / 978462`), `-smp 1` without NVMe (`CAP=0`, `CAP2=0`), and `-vga none` safety.
7. **USB image** — protective MBR + CRC-valid primary/backup GPT + FAT32 walk extracting `BOOTX64.EFI` (byte-identical to the `zero2` laptop build), then an OVMF boot from xHCI `usb-storage` reaching the Command Deck with zero bytes written to `0xE9`.

### Command Deck keys

| Key | Action |
| :--- | :--- |
| `1` / `2` / `3` | Dispatch NVMe Extent 0 / 1 / 2 (`(40, 2)`) across worker reactors via `MOVDIR64B`; Deck shows sum + `rdtsc` cycles |
| `B` | Re-run the Ring-0 AVX-512 `<tile \| simd: 8>` vs scalar benchmark |
| `A` (any other key on laptop builds) | Light LED 4 and paint the scancode cell |
| `Q` / `Esc` | Park all worker reactors and halt |
