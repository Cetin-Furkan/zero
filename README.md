# `Zero` (`.zr`) — Language Specification & Stage-0 Compiler

> **`Zero` (`.zr` source, `zero0` stage-0 bootstrap)** is a systems programming language and direct native `x86-64` compiler designed from scratch as the backbone for **`ZeroOS`** — a single-image, reactor-driven bare-metal operating system targeting **Intel 11th Gen Tiger Lake (`i7-1165G7` Willow Cove + `Iris Xe` Gen12LP + NVMe)**.
>
> **Core Engineering Rule:** The compiler never silently accepts syntax or attributes it does not lower. Any construct, memory space, field attribute, or loop lens not yet lowered to real machine instructions is a **hard compile-time error**.

---

## 1. Current Repository Tree & Implementation Status

```text
/home/arch-cetin/projects/zero/
├── README.md                          # Language specification & implementation status table
├── ZERO_OS_ARCHITECTURE.md            # Bare-metal ZeroOS boot, capsule, fabric & scheduler rules
├── PEER_MASTER_PLAN.md                # Phased execution record (Phase 0 -> Gate 1 -> Reactor ABI)
├── CHANGES_AND_VERIFICATION_REPORT.md # Audit fixes, lowering mechanics & test proofs
├── MESSAGE_TO_PEER_AGENT.md           # Initial technical handoff charter
├── Makefile                           # Assembles build/zero0 (nasm -f bin) & runs golden/negative tests
├── bootstrap/                         # Zero-C, zero-linker x86-64 NASM Stage-0 compiler (`zero0`)
│   ├── seed0.asm                      # W^X host & guest ELF64 + `--target=efi-os` CLI spine
│   ├── tokens.inc                     # 16-byte Token record, type tags & AST partition offsets
│   ├── lexer.inc                      # Hardware `crc32` interner & structural tokenizer
│   ├── codegen.inc                    # Direct x86-64 encoder, 16B-aligned frames & RIP-rel `.bss`
│   ├── efi_boot.inc                   # MS x64 UEFI GOP/ACPI/MemoryMap harvest & `ExitBootServices`
│   ├── efi_writer.inc                 # Standalone `PE32+` (`Subsystem=10`) `.text`/`.reloc`/`.bss` writer
│   ├── types.inc                      # Fixed-point `::` constants, bitpacks, structs & 4KiB spaces
│   ├── intrinsics.inc                 # `--target=efi-os` Ring-0 intrinsics (`@halt`, `@wrpkru`, `@cr_*`)
│   ├── views.inc                      # `@view<T>`, `@mmio<T>`, `@port` I/O, `Struct{...}`, `@sizeof`/`@alignof`
│   └── parser.inc                     # Signed/unsigned exprs, `{}` scopes, compound shifts/mod, comptime `if`
├── examples/
│   ├── first.zr                       # Minimal standalone Linux `ELF64` `.zr` program
│   └── gate1_os.zr                    # Bare-metal Gate-1a Ring-0 UEFI OS image (`PE32+` `.efi`)
└── tests/
    ├── run_tests.sh                   # Golden stdout `diff -u` + negative rejection + W^X + QEMU/OVMF runner
    ├── lex_showcase.zr                # Lexer token-stream verification input
    ├── first.expected                 # Golden output for examples/first.zr
    ├── step3_compute.zr               # Order-independent fns, recursion, `<bits>`, `<max|until>`
    ├── step3_compute.expected         # Golden output for step3_compute
    ├── step4_systems.zr               # `<...|...>` bit-tuples, structs, `@slab`, `@arena`, `@syscall`
    ├── step4_systems.expected         # Golden output for step4_systems
    ├── phase0_scalar.zr               # Unsigned shifts/div/cmp, `{}` scopes, bounds `fail 2`, `div0`
    ├── phase0_scalar.expected         # Golden output for phase0_scalar
    ├── phase1_zero_lang.zr            # `<packed>`, `Struct{...}`, `@view<T>`, `@Type` params, `%=`, `\xHH`
    └── phase1_zero_lang.expected      # Golden output for phase1_zero_lang
```

### Honest Implementation Matrix (`zero0` Binary vs Target Spec)

| Construct | Status in `build/zero0` | Exact Lowering / Rule |
| :--- | :---: | :--- |
| **Strict `W^X` Static `ELF64`** | **Implemented** | Both `build/zero0` itself and all guest binaries emit two `PT_LOAD` segments (`PF_R\|PF_X` code/rodata + `PF_R\|PF_W` `.bss` with `p_filesz = 0`). Zero `RWX` segments. |
| **Compact `x86-64` Codegen & Comptime `if`** | **Implemented** | `disp8` `[rbp+disp8]` local encoding for `[-128,127]`, immediate-RHS `+ - << >> & *pow2` lowering, and constant-folded `if`/`else` dead-branch elimination (used by `qemu_debug :: 0`). |
| **Order-Independent `::` Constants & Query Intrinsics** | **Implemented** | Two-pass fixed-point resolution across bitpacks and structs; `@sizeof(T)` / `@alignof(T)` evaluate in constants and expressions. Duplicate or cyclic definitions fail compilation. |
| **Lexical `{}` Block Scoping (`:=`) & `\xHH` / Compound Ops** | **Implemented** | Variables die at closing `}` (including `for`, `loop`, and `recover` bindings). Supports `%=`, `<<=`, `>>=`, and `\xHH` hex escapes in strings/chars. |
| **Signed vs Unsigned Scalar Rules** | **Implemented** | `i8..i64` signed; `u8..u64`, `bit`, `bool`, `mask64`, and bitpack fields unsigned; integer literals signed `i64` (truncated on `u*`/bitpack store). Unsigned `>>` emits `shr`, `/` & `%` emit `div`, and `< <= > >=` emit `setb/be/a/ae` when both operands are unsigned. Division/modulo by zero raises `fail 1`. |
| **Bit-Partitioned Layouts `<a: u32 \| ...>`** | **Implemented** | Field widths must sum to `8`, `16`, `32`, or `64` bits. `\|` separates fields only before `ident :`; `>` closes literals when not followed by a same-line expression operand. |
| **Structs `Name :: { ... } [<packed>]`, `Struct{...}`, `@Type` Params** | **Implemented** | Exact natural or `<packed>` field offsets, typed by-value `Struct{ field: val }` literals & `rep movsb` struct assignment, and non-null `@Type` pointer parameters. `@cold` is **rejected at compile time**. |
| **`@slab` & `@arena` (Static Bounds-Checked)** | **Implemented** | Disjoint virtual regions in writable `PT_LOAD`. Every `space[i]` load/store checks `(u64)i < cap` first, raising `fail 2`. (`@arena` is a checked `u8` array; bump pointers/`WRPKRU` are manual via `@wrpkru`). |
| **`@view<T>(addr [, len])` Bounded Hardware Views** | **Implemented** | 1-arg `@view<T>(addr)` checks `addr != 0` (`fail 3`); 2-arg `@view<T>(addr, len)` carries `{ base, len }`, exposes `.len`, checks non-null (`fail 3`) & `(u64)i < len` (`fail 2`), and allows `v[i].field`. |
| **`for x in mask <bits>` & `loop <max: M \| until: cond>`** | **Implemented** | `for` verifies `<bits>` literally (BMI1 `tzcnt`/`blsr`). `loop` accepts `>`, `>=`, and bitwise `\|` inside `until:`. Any unimplemented `<lens>` (`<tile>`, `<simd>`) is **rejected at compile time**. |
| **Scalar Faults (`fail` / `recover`)** | **Implemented** | `fail expr` sets the non-zero fault word (`0 -> 1`; div0 -> `1`; bounds -> `2`; null `@view` -> `3`) and unwinds across calls to `recover err { ... }` or exits `1` (`@halt()` in Ring 0). |
| **`@syscall(nr, ...)` & `print(...)`** | **Implemented (Host ELF64)** | Linux host bootstrap intrinsics (`r10` for arg 4; signed decimal `print`). Rejected on `--target=efi-os`. |
| **`--target=efi-os` (`PE32+` `zero.efi` + Hardened Gate-1a Ring-0)** | **Implemented (BSP Gate 1a)** | `PE32+` EFI app (`Subsystem=10`), MS x64 GOP (`BGRX`/`RGBX`/`BitMask`), ACPI (`RSDP -> XSDT/RSDT -> MADT/MCFG`), code-span tracking & `ExitBootServices`. Language/Ring-0 constructs: `@port name : <u8\|u16\|u32 \| port>`, `@mmio<T>(addr [, len])`, `@halt()`, `@wrpkru(v)`, `@cli`/`@sti`, `@lgdt`/`@lidt`/`@ltr`, `@cr_read`/`@cr_write`, `@msr_read`/`@msr_write`, `@xsetbv`, `@isr_stub_base`. All rejected on host `ELF64`. |
| **`EFI_MP_SERVICES_PROTOCOL` AP Park, `@capsule`, `@cell64`, `publish`, `@ring`, `@dma_queue`, `<tile>`, `<simd>`, `@cold`** | **Target Spec (Not Yet in `zero0`)** | Specified in [`PEER_MASTER_PLAN.md`](PEER_MASTER_PLAN.md) (Phases 2–4) & [`ZERO_OS_ARCHITECTURE.md`](ZERO_OS_ARCHITECTURE.md); rejected by `zero0` on all targets until lowered. |

---

## 2. Core Scalar & Memory Rules (Frozen in Phase 0–1)

### 2.1 Signedness & Arithmetic
1. `i8`, `i16`, `i32`, `i64` are signed; `u8`, `u16`, `u32`, `u64`, `bit`, `bool`, `mask64`, and all bitpack types/fields are unsigned. Integer literals are `i64` (truncated to destination width on narrow/bitpack stores).
2. `+`, `-`, `*` perform wrapping two's-complement arithmetic.
3. `>>`, `<<`, `/`, and `%` use the signedness of the left operand (`shr`/`div` for unsigned, `sar`/`idiv` for signed). Compound assignments (`%=`, `<<=`, `>>=`) obey the same signedness and zero-divisor guards. A zero divisor executes `fail 1` before the hardware divide instruction (`SIGFPE` never occurs).
4. `<`, `<=`, `>`, `>=` emit unsigned condition codes (`setb`, `setbe`, `seta`, `setae`) when both operands are unsigned, and signed (`setl`, `setle`, `setg`, `setge`) otherwise. Comparisons and short-circuit `&&` / `||` yield `i64` `0` or `1`.

### 2.2 Lexical Braces & Shadowing
- Entering `{` snapshots `local_cnt`; leaving `}` rewinds `local_cnt` while keeping stack-frame slot reservations in `max_frame_qwords` intact.
- Redeclaring a variable name in the same brace depth fails compilation. Declaring a variable in an inner `{ ... }` shadows any outer variable of the same name until the inner `}` closes.
- Loop induction variables (`for i in ...`), hidden loop bounds/masks, and `recover err` bindings live strictly inside their construct and are out of scope afterward.

### 2.3 Bounds, Null-View & Scalar Fault Codes
- `fail 0` dynamically rewrites to fault code `1` (`0` means "no fault" in the fault word).
- Reserved compiler fault codes:
  - `1`: Division or remainder by zero (or explicit `fail 0` / `fail 1`).
  - `2`: `@slab[i]`, `@arena[i]`, or bounded `@view`/`@mmio` slice index out of bounds (`(u64)i >= cap`).
  - `3`: Null `@view<T>` / `@mmio<T>` pointer dereference or non-null `@Type` access when base address is `0`.

---

## 3. Target OS & Hardware Surface (Phases 1–4)

See [`PEER_MASTER_PLAN.md`](PEER_MASTER_PLAN.md) and [`ZERO_OS_ARCHITECTURE.md`](ZERO_OS_ARCHITECTURE.md) for the staged bring-up specification:
- **Phase 1 (`Gate 1a` — Completed & Hardened):** Standalone `PE32+` `zero.efi` + idiomatic hardware-typed Zero (`Tss64 <packed>`, `@port`, `@view<T>`, `@mmio<T>`, `@wrpkru(0)`, `@halt()`), hardware `W^X` page tables via `IA32_EFER.NXE` (`RX` `.text`, `RO+NX` ACPI `MADT`/`MCFG`, `WC+NX` GOP `BGRX`/`RGBX`/`BitMask`, `RW+NX` DRAM, `UC+NX` MMIO, verified at runtime via `verify_no_wx_pages()` -> `WX=0`), and dead-branch `0xE9` elimination for `qemu_debug :: 0`. Verified end-to-end under QEMU + `OVMF` + `KVM` (`WX=0 ACPI=1`, `5` checkpoint LEDs, `0x60` keyboard scancode scanout, and missing-GOP clean return).
- **Phase 2 (`Gate 1b`):** Optional `EFI_MP_SERVICES_PROTOCOL` pre-exit 64-bit AP park (`core_count = 1` fallback if absent; zero 16-bit code).
- **Phase 3 (`Reactor ABI`):** Pinned `r12..r15` + `gs:0` `ReactorLocal` fault word, `<tile>` `cli`/`sti` checkpoint & `@isr` register-ban verification, `publish` (`movdir64b`), and `wait` (`monitor`/`mwait` + `IA32_TSC_DEADLINE`).
- **Phase 4 (`Self-Hosting`):** Rewriting `zero` in `.zr` once Phase 0, Gate 1a, file-I/O slices, and both `ELF64` + `PE32+` containers are proven.

---

## 4. Building & Running the Test Suite

```bash
make test
```

`make test` assembles `build/zero0`, verifies strict `W^X` (`0` `RWE` segments) on both compiler and guest binaries, runs `--lex`, compiles all positive suites and diffs stdout byte-for-byte against `tests/*.expected`, and runs all negative compile-rejection and fault-exit probes.
