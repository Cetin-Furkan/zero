# `Zero` (`.zr`) — Language Specification, Self-Hosted Compiler & `ZeroOS`

> **`Zero` (`.zr` source, `zero0` NASM seed -> `zero1`/`zero2` self-hosted `.zr` compiler)** is a systems programming language and direct native `x86-64` compiler designed from scratch — with **zero C, zero LLVM, zero libc, and zero external linker** — as the backbone for **`ZeroOS`**, a single-image, reactor-driven bare-metal operating system targeting **Intel 11th Gen Tiger Lake (`i7-1165G7` Willow Cove + `Iris Xe` Gen12LP + PCIe NVMe)**.
>
> **Core Engineering Rule:** The compiler never silently accepts syntax or attributes it does not lower. Any construct, memory space, field attribute, or loop lens not yet lowered to real machine instructions is a **hard compile-time error**.

---

## 1. Current Repository Tree & Implementation Status

```text
/home/arch-cetin/projects/zero/
├── README.md                          # Language specification & implementation status table
├── ZERO_OS_ARCHITECTURE.md            # Bare-metal ZeroOS boot, NVMe @capsule, fabric & scheduler rules
├── PEER_MASTER_PLAN.md                # Phased execution record (Phase 0 -> Gate 3 -> Self-Hosting)
├── CHANGES_AND_VERIFICATION_REPORT.md # Audit fixes, lowering mechanics & test proofs
├── MESSAGE_TO_PEER_AGENT.md           # Initial technical handoff charter
├── Makefile                           # Builds zero0, self-hosted zero1/zero2, & runs 6-stage test suite
├── bootstrap/                         # Zero-C, zero-linker x86-64 NASM Stage-0 seed (`zero0`)
│   ├── seed0.asm                      # W^X host & guest ELF64 + `--target=efi-os` / `--target=capsule` CLI
│   ├── tokens.inc                     # 16-byte Token record, type/lens tags & AST partition offsets
│   ├── lexer.inc                      # Hardware `crc32` interner & structural tokenizer (`=>`, `|>`, `<<<`, `>>>`)
│   ├── imports.inc                    # Recursive `use "rel/path.zr"` module loader with diamond deduplication
│   ├── codegen.inc                    # Direct x86-64 encoder (`movnti`, `disp8`, 64B frames, RIP-rel `.bss`)
│   ├── efi_boot.inc                   # MS x64 UEFI GOP/ACPI/AP-page harvest & `ExitBootServices`
│   ├── ap_trampoline.inc              # Isolated `<1 MiB` `RX` AP startup bridge (`.zr` stays 100% 64b)
│   ├── efi_writer.inc                 # Standalone `PE32+` (`Subsystem=10`) `.text`/`.reloc`/`.bss` writer
│   ├── capsule_writer.inc             # Position-independent `CRC32C`-signed `.zcap` `@capsule` writer
│   ├── types.inc                      # Fixed-point `::` constants, `@cell64`, `@ring`, `@dma_queue`, `@capsule`, structs
│   ├── pointers.inc                   # Custom `@ptr<T|mask|wrap|stride|bswap>`, `@stream`, `@atomic`, `@code_bytes`, silicon ops
│   ├── reactor.inc                    # Pinned Reactor ABI (`r12..r15`, `gs:0`), `publish` (`movdir64b`), `wait` (`mwait`), `<tile>`, `@isr`
│   ├── loops.inc                      # Hardware loops (`for [i,] x in span`, `<tile>`, `<rev>`, `<stride>`, `<unroll>`, `<prefetch>`)
│   ├── intrinsics.inc                 # Ring-0 intrinsics (`@halt`, `@wrpkru`, `@rdtsc`, `@pause`, `@fn_addr`)
│   ├── views.inc                      # `str`/`bytes`/`[]T`, `@view<T>`, `@mmio<T>`, `s[lo..hi]`, `@port` I/O, `Struct{...}`
│   └── parser.inc                     # Signed/unsigned exprs, `[hi:lo]` bit-slices, `=>`, `|>`, `match`, `<<<`/`>>>`, `{}` scopes
├── compiler/                          # Phase-4 Self-Hosted Stage-1 `.zr` Compiler (`zero1 -> zero2 == zero3`)
│   ├── zero1.zr                       # Recursive-descent statement compiler, prescan driver & CLI entry
│   ├── defs.zr                        # Disjoint `@slab` compiler arenas, `@crc32c` interner & host `@syscall` I/O
│   ├── lexer.zr                       # Full `.zr` tokenizer + canonical `use "rel/path.zr"` module expander
│   ├── emit_x64.zr                    # Direct x86-64 encoder (64B frames, `[0x10000000]`/`[gs:0]` faults, `<tile>`)
│   ├── containers.zr                  # Strict 2-segment `W^X` `ELF64` & `CRC32C`-signed `--target=capsule` `.zcap` writers
│   └── parser_expr.zr                 # Fixed-point `::` constants, `<...>` BitPacks, `|>`, `[hi:lo]`, silicon ops & exprs
├── examples/
│   ├── first.zr                       # Minimal standalone Linux `ELF64` `.zr` program
│   ├── capsule_compute.zr             # Position-independent `--target=capsule` workload (`CAP=777` on NVMe)
│   ├── gate1_os.zr                    # Modular Gate-3 Multi-Core Reactor + NVMe `@capsule` UEFI OS (`PE32+` `.efi`)
│   └── os/                            # Modular `ZeroOS` subsystems (`use "os/..."`)
│       ├── hw_types.zr                # `ReactorLocal`, `@cell64`, `@ring`, `@dma_queue`, `@capsule`, `NvmeCmd`, ACPI/TSS
│       ├── debug_io.zr                # First-class `str` slice `0xE9` telemetry (`elided on laptop`)
│       ├── paging_wx.zr               # `[hi:lo]` bit-slice 4-level `W^X` paging, dynamic `UC` MMIO & `seal_capsule_wx_rx`
│       ├── reactor_fabric.zr          # `MOVDIR64B` + `MWAIT` + `<tile>` + `gs:0` + `@isr`/`@isr_ok` cross-core benchmark
│       ├── smp_madt.zr                # `match` MADT parser + `@atomic<u64>` / `x2APIC` `INIT-SIPI` AP bring-up
│       ├── nvme_capsule.zr            # PCIe `MCFG`/`ECAM` discovery, lock-free NVMe `@dma_queue`s, `CRC32C` & live `@capsule` exec
│       └── hud_render.zr              # `<unroll: 4>` + `@stream<u32>` (`movnti`) GOP & live `CYC:` / `CAP:` HUD badge
└── tests/
    ├── run_tests.sh                   # 6-stage golden, opcode, negative, self-host fixed-point (`zero2==zero3`) & Q35+NVMe runner
    ├── lib/                           # Multi-file `use "lib/..."` test modules (diamond + `../` canonicalization verified)
    │   ├── math_ops.zr                # `=>` arrow fns, `<<<` rotates, `[hi:lo]` bit-slices & `bytes` FNV-1a
    │   └── ptr_lenses.zr              # `MaskedU64Ptr`, `Ring4Ptr`, `BeStridePtr`, `BeI16Ptr` custom `@ptr<...>` lenses
    ├── lex_showcase.zr                # Lexer token-stream verification input
    ├── first.expected                 # Golden output for examples/first.zr
    ├── step3_compute.zr               # Order-independent fns, recursion, `<bits>`, `<max|until>`
    ├── step3_compute.expected         # Golden output for step3_compute
    ├── step4_systems.zr               # `<...|...>` bit-tuples, structs, `@slab`, `@arena`, `@syscall`
    ├── step4_systems.expected         # Golden output for step4_systems
    ├── phase0_scalar.zr               # Unsigned shifts/div/cmp, `{}` scopes, bounds `fail 2`, `div0`
    ├── phase0_scalar.expected         # Golden output for phase0_scalar
    ├── phase1_zero_lang.zr            # `<packed>`, `Struct{...}`, `@view<T>`, `@Type` params, `%=`, `\xHH`
    ├── phase1_zero_lang.expected      # Golden output for phase1_zero_lang
    ├── phase2_zero_power.zr           # Slices, `use`, custom `@ptr`, `@stream`, `@atomic`, `@code_bytes`/`@call`/`@emit`, loops, silicon ops
    ├── phase2_zero_power.expected     # Golden output for phase2_zero_power
    ├── phase3_reactor_fabric.zr       # `@cell64`, `@ring`, `@dma_queue`, `publish` (`movdir64b`), `@movdiri`, `@cpuid_ecx`
    └── phase3_reactor_fabric.expected # Golden output for phase3_reactor_fabric
```

### Honest Implementation Matrix (`zero0` & Self-Hosted `zero2` vs Target Spec)

| Construct | Status | Exact Lowering / Rule |
| :--- | :---: | :--- |
| **Strict `W^X` Static `ELF64` & `(argc, argv)`** | **Implemented** | `zero0`, `zero1`, `zero2`, and all guest binaries emit two `PT_LOAD` segments (`PF_R\|PF_X` code/rodata + `PF_R\|PF_W` `.bss` with `p_filesz = 0`). Zero `RWX` segments. |
| **Phase-4 Deterministic Self-Hosting (`zero0 -> zero1 -> zero2 == zero3`)** | **Implemented** | `compiler/zero1.zr` (5 modular `.zr` files) compiles itself deterministically (`cmp -s build/zero2 build/zero3` passes byte-for-byte, `203,241` bytes) and emits both `W^X` `ELF64` binaries and `CRC32C`-signed `--target=capsule` `.zcap` images executed by bare-metal `ZeroOS` off NVMe. |
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
| **Hardware Loops (`for [idx,] x in span`, `<tile>`, `<rev>`, `<stride>`, `<unroll>`, `<prefetch>`)** | **Implemented** | Direct 1- or 2-variable iteration over `str`/`bytes`/`[]T`/`@view`/`@slab`/`@arena`, cooperative `<tile>` loops (`for ... <tile>`, `loop <tile \| ...>`), `<bits>` (`tzcnt`/`blsr`), `<rev>`, `<stride: S>`, `<prefetch: D>`, and `<unroll: K>` (`K` in `{2, 4, 8}`). `<tile: Size>` cache-blocking and `<simd>` remain **rejected at compile time**. |
| **True-Zero Syntax (`[hi:lo]`, `=>`, `\|>`, `<<<`/`>>>`, `match`) & Silicon Ops** | **Implemented** | Hardware bit-slice extraction `expr[hi:lo]` and RMW `lval[hi:lo] = v`; `fn f(...) T => expr`; `x \|> f(a)` pipelines; 64-bit `rol`/`ror` (`<<<`, `>>>`, `<<<=`, `>>>=`); `match x { ... }`; and inline `@popcnt`, `@lzcnt`, `@tzcnt`, `@bswap16/32/64`, `@pdep`, `@pext`, `@crc32c`, `@prefetch`, `@clflush`, `@lfence`, `@sfence`, `@mfence`. |
| **Phase-3 Reactor ABI (`r12..r15`, `gs:0`, `arg3 = r10`), `@cell64`, `@ring`, `@dma_queue`, `publish` (`movdir64b`), `@movdiri`, `wait` (`monitor`/`mwait`), `<tile>`, `@isr`/`@isr_ok`** | **Implemented** | `--target=efi-os` and `--target=capsule` use `rdi, rsi, rdx, r10, r8, r9`, pin `r12..r15` (`@reactor_id`, `@inbox_base`, `@scratch_base`, `@local_base`), isolate per-core `ReactorLocal` fault state at `gs:0`, unwind `<tile>` `[gs:8]`/`[gs:16]` across `return`/`break`/`continue`/`recover`, lower `publish` to `movdir64b` & `wait` to Intel SDM §9.10.4 race-free `monitor`/`mwait` + `IA32_TSC_DEADLINE`, and statically verify `@isr` (`iretq`, GPR-only, 0 XSAVE) & leaf `@isr_ok`. |
| **Gate-3 Bare-Metal PCIe `MCFG`/`ECAM` + Lock-Free NVMe `@dma_queue` + `@capsule` Engine** | **Implemented** | `examples/os/nvme_capsule.zr` discovers NVMe via ACPI `MCFG`/`ECAM`, binds Admin + I/O `@dma_queue` rings, queries `Identify Namespace` (`LBADS`), DMAs the Master Extent Table & self-hosted `capsule_z2.zcap`, verifies hardware SSE4.2 `@crc32c`, seals `boot_capsule` `PTE_RX` (`WX=0`), tests `gs:0` `recover` (`fail 88`), and executes `@call` (`CAP=777`). |
| **`<tile: Size>` Cache-Blocking, `<simd>`, `@cold`, `Iris Xe` `CCS0` Gate 4** | **Target Spec (Not Yet Lowered)** | Rejected at compile time until lowered to hardware instructions. |

---

## 2. Building & Running the Complete 6-Stage Verification Suite

```bash
make test
```

`make test` assembles `build/zero0`, verifies strict `W^X` (`0` `RWE` segments) on host and guest binaries, runs `--lex`, verifies all golden suites & inline silicon opcodes (`movdir64b`, `movdiri`, `movnti`, `lock xadd/cmpxchg`, `pdep/pext`, `crc32`), runs all negative rejection probes, proves the **Stage-2 == Stage-3 (`zero2 == zero3`) self-hosting fixed point**, compiles `capsule_z2.zcap` with the self-hosted `zero2` compiler, and boots 4-core `ZeroOS` on `QEMU Q35 + KVM + OVMF + NVMe` to verify `C=4`, `WX=0`, `GS=1`, `FAB=1`, `CYC`, and live NVMe `@capsule` execution (`CAP=777`).
