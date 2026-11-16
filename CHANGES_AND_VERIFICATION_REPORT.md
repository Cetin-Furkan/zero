# `Zero` — Audit Remediation, Hardened Step 3 & Step 4 Verification Report

This report documents every specification correction, compiler bug fix, Step-4 lowering addition, and automated verification gate implemented across `/home/arch-cetin/projects/zero`.

---

## 1. Documentation & Architecture Purge (`README.md` & `ZERO_OS_ARCHITECTURE.md`)

All speculative performance/size slogans and unverified hardware assumptions were removed and replaced with rules verified directly against `/proc/cpuinfo` and `/proc/bus/input/devices` on the host **Intel Core i7-1165G7**:

1. **Removed `waitpkg` (`umwait` / `umonitor` / `tpause`):**
   - Confirmed absent on Willow Cove (`monitor` is present; `waitpkg` is absent).
   - Replaced in the OS design with Ring-0 `MONITOR`/`MWAIT` + `IA32_TSC_DEADLINE` behind a semantic `wait` operation rather than freezing an ISA opcode into the syntax.
2. **Removed Unmeasured Slogans & Contradictory Paging Claims:**
   - Deleted `"5 ms for 1,000,000 lines"`, `"1-cycle / 2 ns WRPKRU"`, `"35–60 ns"`, `"< 0.8 W"`, `"~3,350 LOC"`, and `"zero TLB misses for the entire uptime"`.
   - Documented `WRPKRU` accurately as a serializing Ring-0 compiler seatbelt (`16` keys), and corrected physical memory mapping to map usable `EfiConventionalMemory` spans `WB` (`2 MiB` / `1 GiB` where hole-free) while keeping `MMIO` (`UC`) and GOP/GPU apertures (`WC` via `IA32_PAT`) strictly outside `WB` DRAM ranges.
3. **Locked the Five Core Language/OS Structural Distinctions:**
   - **`@capsule` vs Mutable State Extent:** Deleted leftover `4 KiB` Copy-on-Write page scattering; `@rel` offsets live strictly inside immutable `2 MiB`-aligned `@capsule` images, while mutable state writes sequentially across erase-block extents flipped via a 64-byte Master Extent record.
   - **CPU `@ring` vs PCIe `@dma_queue` + `@port`:** Separated software CPU rings (eligible for virtual double-mapping) from physically contiguous PCIe DMA queues (`NVMe` `SQ`/`CQ` + uncached doorbell), and added `@port` (`in`/`out` for `0x60`/`0x64` `i8042` keyboard).
   - **`@cell64` + `publish` (`movdir64b`) & `enqueue` (`MI_SEMAPHORE_WAIT`):** Defined as explicit operations; prohibited sockets, pipes, and byte-stream IPC across the grammar.
   - **`<tile>` Interrupt-Atomicity & Pre-Exit AP Startup:** Specified `EFI_MP_SERVICES_PROTOCOL` to park all 7 APs in 64-bit Long Mode *before* `ExitBootServices()` (zero 16-bit `SIPI` code), and locked the scheduler law: `@isr` handlers have a compiler-verified scalar GPR-only clobber set and never switch tasks; reactors switch `@cell64` tasks only at `<tile>` checkpoints where `ZMM0..31` are dead.
   - **Two-Tier Faults:** Scalar fault codes (`fail` / `recover`) handle bring-up, drivers, and control paths; `EVEX` `k7` write-masking is scoped strictly to `<simd>` vector tiles.
4. **Honest Repository Tree & Implementation Table:**
   - Updated [`README.md`](README.md) so the file tree matches the exact files on disk and explicitly marks every construct as **Implemented in `zero0`** vs **Planned Target Spec**.

---

## 2. Hardened Step-3 Compiler Fixes in `bootstrap/`

| Audit Finding | Root Cause in Earlier `zero0` | Exact Fix Implemented | Verification Test |
| :--- | :--- | :--- | :--- |
| **`8 mod 16` `RSP` Misalignment Across Calls** | `parse_expr` and `.fn_call` used dynamic `push rax` / `pop` on `rsp` while evaluating sub-expressions. | Eliminated all dynamic `push`/`pop` in expressions and calls. Added `alloc_temp_qword` / `free_temp_qwords` in [`codegen.inc`](bootstrap/codegen.inc) allocating reusable slots inside the `16B`-aligned `[rbp - disp]` frame + `and rsp, -16` in `_start`. | `verify_rsp16(10) + verify_rsp16(20) * (verify_rsp16(3) + verify_rsp16(4))` checks `@rsp_align() == 0` inside all 4 nested calls (`150`). |
| **Non-Short-Circuiting `&&` and `||`** | Both operands were evaluated eagerly before bitwise/boolean combination. | Implemented `.sc_and` (`jz` skip RHS) and `.sc_or` (`jnz` skip RHS) in [`parser.inc`](bootstrap/parser.inc). | `record_side(0) && record_side(1)`, `record_side(1) \|\| record_side(1)`, `record_side(1) && record_side(1)` increments side counter `4` times (not `6`). |
| **`loop <max: 10 \| until: n > 8>` Parse Fault** | `advance_to_lens_delim` treated any top-level `>` as closing `<...>`. | Added `advance_to_loop_lens_delim` in [`parser.inc`](bootstrap/parser.inc): top-level `>` closes `loop <...>` **only** when followed by `{` (`TOK_LBRACE`). | `loop <max: 10 \| until: n > 8> { n += 3 }` compiles and stops at `n = 9`. |
| **Ordered Constants & Silent Duplicate `A :: 2`** | Constants were evaluated in a single top-down pass without uniqueness checks. | Added `check_top_name_unique` (hard error on duplicate `::` / `@` names) and multi-pass fixed-point constant resolution in [`types.inc`](bootstrap/types.inc). | `DERIVED_CONST :: BASE_CONST * 4 + 2` before `BASE_CONST :: 10` (`= 42`); negative test rejects `A :: 1; A :: 2`. |
| **Silent No-Op Lenses (`<simd>`, `<tile>`)** | `skip_angle_block` discarded `<...>` on range `for` and ignored the identifier inside non-range `for <...>`. | Range `for` rejects any `<...>` lens; non-range `for` requires literally `<bits>` (`sym_id == sym_bits_id`) and rejects all other identifiers. | Negative tests `fake_nonrange_simd` (`for i in 5 <simd: 16>`) and `fake_range_tile` both fail compilation. |
| **Dead-Jump `recover` & Unchecked Test Output** | `recover` emitted an unconditional jump over its block; `make test` checked only exit code `0`. | Implemented real interprocedural scalar fault unwinding (`fail` -> `FAULT_WORD_VADDR` -> `recover`) and added `diff -u` golden tests + negative rejection probes in [`tests/run_tests.sh`](tests/run_tests.sh). | `make test` diffs all 3 positive suites and runs 7 adversarial/fault probes. |

---

## 3. Strengthened Step-4 Capabilities Implemented in `zero0`

`build/zero0` (`17,792 bytes`, assembled from `6,103` lines across 6 NASM files with zero C and zero linker) now implements:

1. **Two-Segment Strict `W^X` `ELF64` Output (`176-Byte` Header):**
   - `Phdr[0]`: `PF_R | PF_X` at `0x00400000` (`.text` + `.rodata`).
   - `Phdr[1]`: `PF_R | PF_W` at `0x10000000` (`p_filesz = 0`, `p_memsz = [data_bss_off]`), hosting the runtime scalar fault word at `0x10000000` plus 64-byte-aligned `@slab` and `@arena` reservations with **zero disk bloat**.
2. **Bit-Partitioned Layouts (`Name :: <f1: u32 | f2: u16 | f3: u16>`):**
   - Supports `bit`/`bool` (`1`), standard `u8..u64`/`i8..i64`, odd widths (`u4`, `u11`, `u20`, `b12`), and integer bit counts (`1..64`).
   - Statically enforces total bit sum $\in \{8, 16, 32, 64\}$ (tested negatively on a 48-bit layout).
   - Lowers MSB-to-LSB construction (`Token<gen: 5 | slot: 1023 | op: 7>`), `.field` extraction (`shr` + `and`), and in-place `.field =` / `.field +=` read-modify-write updates.
3. **Record Structs (`Name :: [<align: N>] { ... }`):**
   - Computes natural field alignments, honors explicit `<align: 64B>`, accepts `@cold` annotations, and lowers nested bitpack fields (`world[3].tok.gen += 10`).
4. **Disjoint `@slab` & `@arena` Memory Spaces:**
   - `@slab name : <ElemType | max: ConstExpr>` and `@arena name : <cap: ConstExpr>`.
   - Supports typed element indexing (`world[i].vel`), byte-buffer indexing (`io_buf[k] = 'Z'`), and compile-time space properties (`.base`, `.max`, `.cap`).
5. **Interprocedural Scalar Faults (`!Fault`, `fail expr`, `recover err { ... }`):**
   - `fail code` writes non-zero `code` to `qword [0x10000000]` and unwinds either directly to an enclosing `recover` landing pad or across function call frames (via post-`call` checks) until caught; unhandled faults exit the guest process with status `1`.
6. **Host Intrinsics (`@syscall` & `@rsp_align`):**
   - `@syscall(nr, a0..a5)` places argument 4 into `r10` per the Linux `x86-64` kernel ABI.

---

## 4. Verified Test & Footprint Summary (`make test`)

| Binary / Artifact | Size on Disk | Virtual `.bss` (`Phdr[1]`) | Verification Gate |
| :--- | :---: | :---: | :--- |
| **`build/zero0`** (Stage-0 Compiler) | `17,792 B` | `144 B` (+ `512 MiB` `mmap` arena) | Assembled via `nasm -f bin` |
| **`build/first`** | `480 B` | `64 B` | `diff -u tests/first.expected` **PASS** |
| **`build/step3_compute`** | `2,379 B` | `64 B` | `diff -u tests/step3_compute.expected` **PASS** |
| **`build/step4_systems`** | `4,752 B` | `12,416 B` (`0x3080`) | `diff -u tests/step4_systems.expected` **PASS** |
| **7 Adversarial / Fault Probes** | — | — | All 6 invalid inputs rejected; unhandled `fail` exits `1` **PASS** |

### Known Remaining Stage-0 Boundaries (Explicitly Not Claimed Yet)
- **Register Allocation:** Expressions still evaluate through `rax`/`rcx` with frame-allocated temporaries (`[rbp - disp]`); there is no SSA or multi-GPR register allocator yet.
- **Scoping & Types:** Local variables are function-scoped (searched newest-to-oldest) rather than popped at inner `{}` block exit; floats (`f32`/`f64`), slice/string runtime variables (`{ ptr, len }`), pointers/references as function arguments, and unsigned vs signed comparison distinction (`setb`/`seta` vs `setl`/`setg`) are not yet separated in expression lowering.
- **OS / Hardware Target:** `zero0` currently emits only Linux `ELF64` binaries; `@capsule`, `@cell64`, `publish` (`movdir64b`), `@ring`, `@dma_queue`, `@mmio`, `@port`, `<tile>`, `<simd>`, and the `PE32+` `zero.efi` backend are not yet implemented.
