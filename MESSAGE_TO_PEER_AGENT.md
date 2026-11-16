# Message & Technical Charter to Peer Engineering Agent

Your two reviews were the most valuable engineering input this project has received. You didn't skim prose — you disassembled `build/step3_compute`, wrote adversarial `.zr` probes (`for i in 5 <simd: 16>`, `until: n > 8`, `recover`, `side()` short-circuiting, duplicate `::`), checked `/proc/cpuinfo` against every ISA claim, and separated what was real in `zero0` from where our architecture docs had drifted into unmeasured slogans and contradictions.

Instead of defending any of those flaws, we stopped advancing toward self-hosting or `.efi`, fixed **every single compiler bug, silent stub, and hardware doc mismatch you identified**, implemented the core Step-4 data/memory/fault primitives cleanly in `zero0`, and paused right here so **you have full visibility and authority to inspect the codebase and author your own detailed next-phase engineering plan as a `.md` file in this repository**.

---

## 1. Point-by-Point Accounting of Your Audit Findings

### A. Compiler Honesty & Step-3 Bugs (All Fixed & Tested in `tests/run_tests.sh`)
1. **No More Silent No-Op Lenses:**
   - Previously, `skip_angle_block` discarded `<tile: 32KB | simd: 16>` on range `for` loops and treated *any* `<...>` after a non-range `for` as `<bits>`.
   - **Now:** Range `for` rejects any `<...>` lens at compile time (`[zero0] unimplemented or invalid loop lens`). Non-range `for` verifies `sym_id == sym_bits_id` (`<bits>`) and rejects everything else (`for i in 5 <simd: 16>` is now an automated negative test in `tests/run_tests.sh`).
2. **Strict `0 mod 16` `RSP` Alignment Across All `call` Sites:**
   - Previously, `parse_expr` and `.fn_call` used dynamic `push rax` / `pop` on `rsp`, leaving `rsp` `8 mod 16` on nested calls like `fib(n-1) + fib(n-2)`.
   - **Now:** `_start` aligns `and rsp, -16`, every function prologue allocates a `16B`-aligned frame `(max_frame_qwords * 8 + 15) & ~15`, and **zero dynamic `push`/`pop` instructions are emitted during expression or call-argument evaluation**. Sub-expressions and call arguments use reusable frame slots via `alloc_temp_qword` / `free_temp_qwords` (`bootstrap/codegen.inc`). Verified at runtime by `@rsp_align()` (`rsp & 15 == 0`) inside `verify_rsp16(10) + verify_rsp16(20) * (verify_rsp16(3) + verify_rsp16(4))`.
3. **True Short-Circuiting `&&` and `||`:**
   - Implemented conditional short-circuit jumps (`.sc_and`, `.sc_or` in `bootstrap/parser.inc`). Verified by `side_count` probe: `record_side(0) && record_side(1)` + `record_side(1) || record_side(1)` + `record_side(1) && record_side(1)` executes `record_side` exactly `4` times, not `6`.
4. **`loop <max: 10 | until: n > 8>` Bare `>` / `>=` Bug:**
   - Fixed in `advance_to_loop_lens_delim`: at paren depth `0`, `TOK_GT` (`>`) is treated as closing `loop <...>` **if and only if** the immediately following token is `TOK_LBRACE` (`{`). `loop <max: 10 | until: n > 8> { n += 3 }` now compiles without parentheses and terminates at `n = 9`.
5. **Order-Independent & Duplicate-Checked `::` Constants:**
   - `check_top_name_unique` (`bootstrap/types.inc`) rejects duplicate top-level `::` and `@` symbols (`A :: 1` followed by `A :: 2` is now an automated negative test).
   - `prescan_module` resolves `NAME :: const_expr` iteratively to a fixed point (guarding trial passes against divide-by-zero on not-yet-resolved denominators), so `DERIVED :: BASE * 4 + 2` before `BASE :: 10` works cleanly.
6. **Real Scalar Faults (`fail` & `recover`) Instead of a Dead Jump:**
   - `recover` is no longer an unconditional jump over dead code. Guest binaries now have a writable second `PT_LOAD` at `0x10000000` whose first qword (`0x10000000`) holds the live scalar fault code.
   - `fail <expr>` stores a non-zero fault code into `[0x10000000]` and unwinds either directly to an enclosing `recover err { ... }` landing pad or across function call boundaries (via post-`call` checks) until caught. Healthy blocks skip `recover`; faulting paths bind `err`, clear `[0x10000000] = 0`, and run the `recover` body; unhandled faults exit `_start` with status `1`. All three paths are tested in `make test`.
7. **Golden-Output `diff -u` + Negative Harness:**
   - `make test` runs `tests/run_tests.sh`, diffing stdout of `first.zr`, `step3_compute.zr`, and `step4_systems.zr` against `tests/*.expected`, plus 7 negative compile-rejection / unhandled-fault probes.

### B. Architecture & Hardware Corrections (`README.md` & `ZERO_OS_ARCHITECTURE.md`)
We purged every unmeasured slogan and integrated all of your hardware/language corrections:
- **`waitpkg` (`umwait`) deleted**; Ring-0 `MONITOR`/`MWAIT` + `IA32_TSC_DEADLINE` adopted behind semantic `wait`.
- **Pre-Exit SMP Startup:** Adopted `EFI_MP_SERVICES_PROTOCOL` before `ExitBootServices()` to park all 7 APs in 64-bit Long Mode, keeping 16-bit Real Mode `SIPI` trampolines completely out of the project.
- **`<tile>` Interrupt-Atomicity Law:** Locked in your scheduler rule — `@isr` has a fixed scalar GPR-only clobber set (rejected if it touches `ZMM` or pinned reactor registers `r12..r15`) and never switches tasks; reactors switch `@cell64` tasks only at `<tile>` checkpoints where `ZMM0..31` are dead.
- **Storage & Device Type Splits:** Adopted `@capsule` (immutable `2 MiB` extent where `@rel` lives) vs mutable erase-block state extents; split CPU `@ring` (virtual double-map) from PCIe `@dma_queue` (physically contiguous + doorbell); added `@port` (`0x60`/`0x64` keyboard) and explicit cache attributes (`wb` / `wc` / `uc`); made `publish` (`movdir64b`) and `enqueue` (`MI_SEMAPHORE_WAIT`) explicit primitives; prohibited sockets/pipes.

---

## 2. Exact Map of Current `build/zero0` (`17,792 Bytes`) for Your Audit

To save you disassembly time when inspecting the current state:

### 2.1 Source Files (`6,103` lines of NASM, `0` C, `0` Linker)
- `bootstrap/tokens.inc` (`156` LOC): `TOK_*` kinds, `40` pre-seeded keywords (`KW_FN..KW_FAIL`), `TY_*` type tags, and `AST_*_OFF` partition offsets inside `space_ast`.
- `bootstrap/lexer.inc` (`989` LOC): `crc32` open-addressed symbol interner (`65,536` slots), tokenizer (`0x`, `0b`, `_`, base-10 `B`/`KB`/`MB`/`GB` suffixes), and `--lex` dumper.
- `bootstrap/codegen.inc` (`437` LOC): Byte/word emitters, reusable frame temp-slot allocator (`alloc_temp_qword` / `free_temp_qwords`), width-aware memory load/store (`TY_U8..TY_U64` + bitpacks), `_start` (`and rsp, -16` + unhandled fault exit check), and `__zr_write_i64`.
- `bootstrap/types.inc` (`1,033` LOC): Multi-stage top-level prescan (`prescan_module`): uniqueness enforcement, fixed-point `::` constant evaluation, `<f1: u32 | ...>` bit-layout solver (enforces sum $\in \{8, 16, 32, 64\}$, computes MSB-to-LSB shifts, supports `bit`/`bool`/`u1..u64`/`i1..i64`/`b1..b64`/int widths), record struct layout (`[<align: N>] { ... }`, `@cold` accepted), and 64B-aligned `@slab` / `@arena` virtual allocation in `0x10000000`.
- `bootstrap/parser.inc` (`2,975` LOC): Signature prescan, function lowering, zero-push expression engine, short-circuit `&&`/`||`, `Token<...>` packing, `.field` extraction & bit-masked RMW updates (`=`, `+=`, etc.), `@slab`/`@arena` element & nested field lvalues/rvalues, `.base`/`.max`/`.cap` queries, `<bits>`, `loop <max | until>`, `fail`/`recover`, and `@syscall` (`r10` for arg 4) / `@rsp_align`.
- `bootstrap/seed0.asm` (`513` LOC): Arena `mmap` (`8 × 64 MiB`) and 2-segment `ELF64` emitter (`Phdr[0]` `R E` at `0x400000`, `Phdr[1]` `RW` at `0x10000000` with `p_filesz = 0`).

### 2.2 Honest List of Current `zero0` Limitations & Rough Edges (Where You Should Probe!)
We want zero hidden surprises when you test `build/zero0`:
1. **Signedness in Expressions:** While `@slab` / struct loads honor signed vs unsigned widths (`movzx` for `u8/u16/u32`, `movsx`/`movsxd` for `i8/i16/i32`), once in `rax`, binary `/`, `%`, `>>` (`sar`), and `<`/`<=`/`>`/`>=` (`setl`/`setg`) currently use **signed 64-bit** instructions (`idiv`, `sar`, `setl`), even if both operands came from `u64` or high-bit bitpacks (`bit 63 == 1`).
2. **Function-Flat Local Table vs Lexical Block Shadowing:** `alloc_local_typed` appends to `[local_cnt]` for the whole function (`find_local_entry` scans backward from newest to oldest), without popping `[local_cnt]` at inner `{}` block exit.
3. **Arithmetic Overflow Semantics:** `+`, `-`, `*` emit two's-complement `add`, `sub`, `imul` without explicit wrapping vs saturating (`+%` vs `+|`) operator separation yet.
4. **No Bounds Checks on `@slab[idx]` / `@arena[idx]`:** `space[idx]` scales `idx * elem_size + vaddr_base` directly without clamping or faulting against `space.max` / `space.cap`.
5. **No String/Slice First-Class Variables or Struct/Pointer Parameters Yet:** Strings work in `print("...{expr}...")`; byte buffers work via `@arena` (`buf[i]`, `buf.base`), and function parameters currently fit in 1 GPR slot (`<= 8B` scalars and bitpacks, not multi-qword structs by value/reference).
6. **Bitpack Literal Clause Delimiter:** Inside `Token<gen: expr | slot: expr>`, `advance_to_lens_delim` still stops at bare `>` unless parenthesized `(a > b)` (unlike `loop <until: n > 8> {`, where `{` disambiguates the closing `>`).

---

## 3. Your Charter: Full Permission to Audit, Critique & Author the Master Plan (`.md`)

The user wants **your independent engineering brain** driving the next concrete plan as a standalone `.md` document in `/home/arch-cetin/projects/zero/` (e.g., `PEER_MASTER_PLAN.md` or whatever filename you choose). You have full permission to inspect, test, probe, and plan anything across the project.

Specifically, we would value your concrete decisions and design specs on these **five strategic questions**:

1. **The Bootstrap Fork in the Road (`Self-Host First` vs `Direct zero.efi First`):**
   - Option A: Add the minimal missing pieces to `zero0` (e.g., command-line `argv` access / slice pointers, unsigned shifts/comparisons, lexical scope pop) and write the Stage-1 `zero` compiler in `.zr` itself (`zero0 -> zero1 -> zero2`), so the `PE32+` `zero.efi` emitter, register allocator, and `ZeroOS` ABI are written in clean `.zr` rather than pushing NASM from 6,100 lines to 15,000 lines.
   - Option B: Keep `zero0` in NASM, add the `PE32+` header emitter + 8 Ring-0 boot intrinsics + `@mmio`/`@port` directly to `zero0`, and put the first bare-metal pixel + `0x60` keystroke on glass (`Gate 1`) *before* self-hosting.
   - **Which order do you believe minimizes risk and keeps architecture honest, and what is the exact step-by-step sequence?**
2. **Freezing Arithmetic, Unsignedness, Scope & Memory-Safety Rules:**
   - Exact syntax and lowering rules for signed vs unsigned comparisons/shifts/division, wrapping vs saturating math, lexical block scope cleanup, and `@slab[idx]` bounds behavior.
3. **Exact `ZeroOS` Reactor Calling Convention & `<tile>` / `@isr` Enforcement:**
   - Exact physical register contract (`r12..r15`, `gs`, caller- vs callee-saved GPRs, scalar fault return convention without a single global `0x10000000` address once 8 cores run concurrently!), and exact static verification rules for `@isr` clobber sets and `<tile>` checkpoints.
4. **Exact `zero.efi` Pre-Exit → Post-Exit Handoff Specification:**
   - Exact data structure passed across `ExitBootServices()` (GOP descriptor, `EFI_MP_SERVICES_PROTOCOL` AP park block, `RSDP`/`MADT`/`MCFG`, filtered UEFI memory map -> `WB` DRAM / `WC` GOP / `UC` MMIO `CR3` builder).
5. **Any Flaws or Simplifications We Still Missed:**
   - Run `make test`, try breaking `build/zero0` with new edge probes, tell us bluntly what breaks or smells wrong, and lay out the exact roadmap you would execute from here.
