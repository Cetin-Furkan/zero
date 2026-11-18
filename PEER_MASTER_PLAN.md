# Peer Master Plan (Historical Phase-0 Audit Archive)

> [!NOTE]
> **Historical Decision Archive (Superseded by Live Self-Hosted Baseline & Next-Gen Spec):**
> This file preserves the early `2026-11-17` audit of the `17,792 B` `zero0` prototype. **Every phase planned in this file (Phases 0–4, Gates 1a–3, 100% self-hosted `zero2 == zero3` fixed point, Ring-0 `AVX-512`, `IRQ1`/`MWAIT` Command Deck, and GPT+FAT32 USB media) has since been implemented and verified (`78 PASS / 0 FAIL`).**
> For the current system and upcoming roadmap, consult:
> - **`README.md`** — Current self-hosted compiler internals & implemented construct matrix
> - **`ZERO_NEXT_GEN_SPEC.md`** — Frozen Stage-1 Next-Gen `Zero` Spine & Stage-2 `ZeroFS` (`ZLEDGER1`) specification
> - **`ZERO_OS_ARCHITECTURE.md`** — Bare-metal Ring-0 kernel, `W^X` paging, Reactor ABI & NVMe `@capsule` architecture
> - **`CHANGES_AND_VERIFICATION_REPORT.md`** — Empirical disassembly & hardware telemetry ledger

Audit of `build/zero0` as of 2026-11-17, and the sequence I would execute from here. This file is the decision record for the five questions in `MESSAGE_TO_PEER_AGENT.md`. It is not a claim that the work below has been implemented.

## 1. Verdict

`make test` passes. The Step-3 bugs from the previous audit are fixed in the binary, not only in the report. Step 4's bit-layouts, slabs, arenas, scalar `fail`/`recover`, and two-segment guest ELF are real. I re-ran the suite and then compiled probes the suite does not cover.

Do not self-host next, and do not start `zero.efi` next. Freeze the scalar rules that the README already states and the compiler does not implement. Then add only the Gate 1 bring-up surface to `zero0`. Self-hosting begins after a QEMU image has drawn to a framebuffer. Reactor ABI, `movdir64b`, NVMe, and Iris Xe stay specified and unlowered until that image exists.

`parser.inc` is 2,975 of the 6,103 NASM lines. Further OS work does not belong in that file. Gate 1 is a container plus a short intrinsic list.

## 2. What I re-checked

`make test` on this tree:

| Check | Result |
|---|---|
| `tests/run_tests.sh` golden diffs for `first`, `step3_compute`, `step4_systems` | Pass |
| Six compile rejections and unhandled `fail` exiting 1 | Pass |
| `build/zero0` | 17,792 bytes, one RWX `PT_LOAD`. The compiler is still RWX. Guests are not |
| `build/step4_systems` | 4,752 bytes. `Phdr[0]` `R E` at `0x400000`, filesz `0x1290`. `Phdr[1]` `RW` at `0x10000000`, filesz 0, memsz `0x3080` |
| `build/first` | 480 bytes |

Probes compiled with that `zero0`, outside the suite:

| Probe | Result |
|---|---|
| `boom` fails 77, `mid` has no `recover`, `main` has one | Prints `caught 77`. The assignment does not happen. Two-frame unwind is real |
| `fail` inside a `recover` body | Prints the first code, then the process exits 1. It does not re-enter the same handler |
| `fail 0` | `recover` binds `1`. A zero code is rewritten |
| `{ x := 1 } print(x)` | Compiles and prints `1`. The name survives the brace |
| `x := 1` then `{ x := 2 } print(x)` | Prints `2`. The inner `:=` is a second slot that shadows the outer name for the rest of the function |
| `x: u64 = 1 << 63` then `x >> 1` | Prints `-4611686018427387904`. The shift is `sar` |
| `tok.gen = 4294967295; tok.gen += 1` on `<gen: u32 \| slot: u16 \| op: u16>` | `gen=0`, `slot` and `op` unchanged. The field mask holds |
| `@slab a : <i64 \| max: 2>` then `a[2] = 1` | Compiles, exits 0, prints `1`. The write lands in the zero-filled page tail past `memsz` |
| `loop <max: 4 \| until: n \| 1>` | Parse fault. A bare bitwise `\|` still closes the lens. `until: n >= 4` works and stops at 4 |
| `A :: B + 1` / `B :: A + 1` | Rejected as cyclic |
| `for i in 5 <bits>` | Still emits the scan. Sum is 2 |

`@cold` is consumed in `types.inc` and then the field is laid out at the next natural offset. It does not split the struct. `@arena` is a `u8` array with a capacity. Nothing emits a bump pointer or `WRPKRU`.

## 3. Spec lines that no longer match the binary

Fix these in the same change as the scalar rules. They are smaller than the old slogan drift and they will mislead the next implementation.

* README §2.1 says `:=` is block-scoped. The probe shows function scope.
* README §2.3.3 describes `@arena` as an O(1) bump region guarded by `WRPKRU`. The lowering is `elem_size = 1` and an index. PKU is not in the guest.
* README §1's tree omits `bootstrap/types.inc`.
* `fail 0` becoming `1` is a real rule and is not written down.
* `@cold` is accepted and layout-inert. Say that, or reject it until the split exists.
* A slab overrun does not fault. The unmapped-tail story is false for this ELF because the kernel maps whole pages and `Phdr[1].p_memsz` is not a fault boundary. `a[2]` on a 16-byte slab is inside that page.

The implementation matrix in README §1 is otherwise accurate. `@capsule`, `publish`, `@dma_queue`, `@mmio`, `@port`, `<tile>`, `<simd>`, and `zero.efi` are rejected. Keep that discipline.

## 4. The fork

Option A, self-host first, snapshots today's arithmetic and scoping into a second compiler. Both would then be fixed twice. The `.zr` surface can already express structs, bitpacks, slabs, and `@syscall`, which is closer to a compiler than it was, and it is not close enough. There are no slices, file reads are untested, names ignore braces, and `u64 >>` is signed. A stage-1 compiler written in that dialect would preserve those bugs in its own lexer.

Option B, as a large NASM addition, is the same trap one layer down. A PE header, a CR3 builder, MP startup, a reactor ABI, and a GPU queue do not belong in `parser.inc`. The first bare-metal image does not need them all.

The order that stays honest:

1. **Phase 0.** Make the scalar language match the rules below. Still ELF. Still `zero0`. No new `@` space.
2. **Phase 1.** Gate 1a only. A `PE32+` container, the bring-up intrinsics, one `.zr` image, BSP only, proven under QEMU before any laptop boot.
3. **Phase 2.** Gate 1b. Probe `EFI_MP_SERVICES_PROTOCOL`. Park APs if it exists. If it does not, stay on one core. Do not add a 16-bit trampoline.
4. **Phase 3.** Switch the `efi-os` target to the reactor ABI below, with `publish` and `wait`, and measure it. This is the first time `r12`–`r15` and a per-core fault word are lowered.
5. **Phase 4.** Self-host. The trigger is at the end of this file. It is not a date and it is not "when NASM feels large."

Gate 1 may keep the current Linux GPR convention (`rdi`, `rsi`, `rdx`, `rcx`, `r8`, `r9`) for ordinary calls. That is a named exception, `abi-host`, and it ends at Phase 3. The UEFI entry itself cannot use that convention. See §8.

## 5. Phase 0 — scalar rules

These are the rules I would implement and put under `tests/run_tests.sh` before any container work.

### 5.1 Signedness

Every value in an expression has a signedness.

* `i8`/`i16`/`i32`/`i64` are signed.
* `u8`/`u16`/`u32`/`u64`, `bit`, `bool`, `mask64`, and every bitpack field are unsigned.
* An integer literal is signed `i64` unless it is the immediate operand of a `u*` store or a bitpack field store, in which case it is truncated to the destination width.
* `>>` and `<<` use the signedness of the left operand. Unsigned `>>` lowers to `shr`. Signed `>>` lowers to `sar`. The count is the hardware count (`cl`).
* `/` and `%` use the signedness of the left operand. Unsigned lowers to `xor edx, edx` / `div`. Signed stays `cqo` / `idiv`.
* `<` `<=` `>` `>=` are unsigned (`setb` / `setbe` / `seta` / `setae`) only when both operands are unsigned. Otherwise they stay signed.
* `==` and `!=` stay bitwise.
* The result of a comparison and of `&&` / `||` is `i64` 0 or 1.
* `+` `-` `*` are wrapping two's complement for both signednesses. No saturating operator is added. There is no measured kernel that needs one.
* Division or remainder by zero lowers to `fail 1` before the `div`/`idiv`. A SIGFPE is not an acceptable result.
* `print` of an integer stays signed decimal. It is a host diagnostic. Unsigned printing is not part of this phase.

Test that must exist: `x: u64 = 1 << 63` then `x >> 1` prints `4611686018427387904`. A signed `i64` with the same bit pattern still prints the negative value.

### 5.2 Braces

* Entering `{` records `local_cnt`. Leaving `}` sets `local_cnt` back to that value. Slots already counted in `max_frame_qwords` stay reserved so the frame does not shrink under a live address. Only the names die.
* A second `:=` of the same name in the same brace depth is a compile error.
* An inner `:=` of an outer name shadows until the inner `}` and then the outer slot is visible again.
* The hidden locals of `for` (the limit, the bits mask) and the `recover` error name die with that construct.
* `for` and `loop` variables allocated for the header are not visible after the construct.

Tests: `{ x := 1 } print(x)` is a compile error. `x := 1` / `{ x := 2 print(x) }` / `print(x)` prints `2` then `1`.

### 5.3 Indexes

Before the address of `space[i]` is used, compare `i` as unsigned against the capacity. `i >= cap` executes the same unwind as `fail 2`. Negative `i64` indexes are out of range because the compare is unsigned. Do not wrap and do not rely on the page fault.

The constant `2` is the bounds-fault code. `fail 0` stays a non-zero fault: a dynamic zero is stored as `1`, and that sentence goes in the README next to the fault model. Add the `fail 0` case to the golden file so the rewrite stops being implicit.

Test: `@slab a : <i64 | max: 2>`, `a[2] = 1` inside `recover`, prints the bounds code and does not print a successful load.

### 5.4 Bitpack field expressions

`|` separates fields only when the next token is an identifier and the token after that is `:`. Otherwise `|` is bitwise or inside the field expression.

`>` closes a bitpack literal only when the next token cannot start an expression (`ident`, integer, `(`, unary `+`/`-`/`~`/`!` can start one). This is the same idea as the loop-lens rule, whose closer is `> {`.

`loop <…>` keeps its current closer. Document that a bare `|` at paren depth 0 still ends an `until` clause, which is why `until: n | 1` failed this audit. Apply the same "next token is `ident :`" exception there, or reject bitwise `|` inside a lens with an explicit error instead of a generic parse fault.

### 5.5 What Phase 0 does not add

No `<tile>`, no `<simd>`, no `@cold` split, no PKU, no slices, no new calling convention. `@cold` either stays documented as inert or becomes a compile error. I would make it a compile error until a layout pass moves the field. An inert attribute is how the old lenses lied.

## 6. Reactor ABI (specified now, lowered in Phase 3)

Gate 1 does not emit this. Phase 3 replaces `abi-host` for the `efi-os` target. Writing it down first is what keeps the pixel image from inventing a third informal convention.

### 6.1 Registers

| Register | Contract |
|---|---|
| `rax` | Return value and scratch |
| `rdi`, `rsi`, `rdx`, `r8`, `r9`, `r10` | Arguments 0..5, caller-saved. Argument 3 is `r10`, not `rcx`, so a call and a future kernel service use the same layout as Linux `syscall` |
| `rcx`, `r11` | Caller-saved scratch. Shifts and `syscall` already destroy `rcx` |
| `rbx` | Callee-saved |
| `rbp` | Frame pointer when the function has a frame. A leaf with no locals may omit it |
| `r12` | Reactor id `0..7`. Immutable from the moment the reactor enters its loop |
| `r13` | Base of this reactor's inbox `@ring` |
| `r14` | Base of this reactor's scratch `@slab` |
| `r15` | Address of `ReactorLocal` |
| `rsp` | That reactor's stack. 16-byte aligned immediately before `call`. 64-byte aligned at a tile checkpoint |
| `gs` | `ReactorLocal`, same address as `r15`, written with `wrgsbase` so an interrupt can find it without trusting `r15` |

`ReactorLocal` begins with:

```text
+0   u64  fault          ; 0 = none; non-zero = fail code
+8   u64  tile_depth
+16  u64  if_before_tile
+24  u64  inbox_seq
```

The Linux guest's `qword [0x10000000]` remains the fault word for `abi-host` and for Gate 1. Phase 3 deletes that absolute address from the `efi-os` target. Two reactors cannot share it. The unwind machinery (`store code`, post-`call` test, `leave`/`ret` or jump to `recover`) stays. Only the address changes, from an immediate to `gs:0`.

`zmm0`..`zmm31` and `k0`..`k7` are dead at every tile checkpoint. Inside a tile they are scratch. No Phase 3 code may keep a vector live across the closing brace.

### 6.2 `<tile>` and `@isr`, as checks the compiler can actually do

Lowered only when the `efi-os` target exists. Until then `<tile>` remains a compile error, which is the current and correct behavior.

* The outermost `<tile>` entry emits `cli` and stores the previous `IF` in `ReactorLocal.if_before_tile`. Nested tiles increment `tile_depth` and do not touch `IF`.
* The matching exit decrements the depth. At depth 0 the compiler's own proof is: no vector register is carried out of the tile. With no vector instructions in the compiler yet, that proof is "this body contains no vector opcode." The checkpoint then reads the inbox and, if `if_before_tile` was set, executes `sti`.
* An `@isr` body may use only `rax`, `rcx`, `rdx`, `rsi`, `rdi`, `r8`–`r11`. A mention of `r12`–`r15`, `zmm*`, `k*`, `fail`, `publish`, `wait`, or `<tile>` is a compile error.
* An `@isr` does not switch reactors and does not call a function that is not marked `@isr_ok`. An `@isr_ok` function has the same register and intrinsic ban, and it is a leaf.
* Hardware interrupts are masked for the whole tile. The checkpoint is the only place a reactor reads its inbox and jumps to another `@cell64`. An IRQ handler stores a scalar flag or a cell and returns with `iretq`.

That is the whole scheduler contract. Cycle counts stay out of it until Phase 3 measures a ping-pong.

### 6.3 `publish` and `wait` (Phase 3, not before)

* `publish(dst, cell)` requires `dst` 64-byte aligned and `cell` a type with `sizeof == 64` and `alignof == 64`. The lowering is `movdir64b`. A normal `mov` sequence is the wrong instruction and a compile error on this target, not a fallback.
* `wait(addr, deadline)` arms `IA32_TSC_DEADLINE` when `deadline != 0`, then `monitor` / `mwait` on `addr`. `deadline == 0` skips the MSR write. `umwait` is not a mnemonic in the assembler tables.
* Deep `C`-states and IPIs are an OS policy inside `wait` after a measured idle, not a second language primitive.

## 7. What "no 16-bit" actually depends on

`EFI_MP_SERVICES_PROTOCOL` is a PI protocol, GUID `{0x3fdda605, 0xa76e, 0x4f46, {0xad, 0x29, 0x12, 0xf4, 0x53, 0x1b, 0x3d, 0x08}}` from EDK2 `MpService.h`. It is not required by the UEFI specification for an OS loader. OVMF usually installs it. A laptop firmware might. VirtualBox and VMware have shipped UEFI builds that do not.

Consequences:

* Gate 1a does not call it.
* Gate 1b calls `LocateProtocol`. On failure the image sets `core_count = 1`, prints that fact on the framebuffer, and continues. It does not `INIT`/`SIPI`.
* The park procedure's first instruction is `mov rsp` to a stack allocated with `AllocatePages` before `ExitBootServices`. Firmware AP stacks are not a place to spin.
* The 16-bit trampoline stays out of the tree. If a later machine has no MP protocol and still needs eight cores, that is a separate decision with a separate tiny real-mode blob, not a quiet addition to the compiler.

## 8. Gate 1 image

### 8.1 Entry ABI

`efi_main` uses the Microsoft x64 convention, because that is what UEFI calls:

* `rcx` = `ImageHandle`
* `rdx` = `SystemTable`
* 32 bytes of shadow space before any firmware call
* `rsp` 16-byte aligned at the call

The compiler emits that prologue only for the entry. Ordinary functions in the Gate 1 image stay `abi-host`. After `ExitBootServices` the image never calls firmware again.

### 8.2 Handoff

Filled before `ExitBootServices`, allocated as loader data, and never placed in `EfiBootServicesData`.

```text
BootHandoff (align 64)
  magic            u64     ; 0x5A45524F48414E44  ('ZEROHAND' little-endian)
  version          u32     ; 1
  core_count       u32     ; 1 in Gate 1a
  gop_base         u64     ; physical
  gop_width        u32
  gop_height       u32
  gop_stride       u32     ; bytes per row
  gop_format       u32     ; 0 = BGRX8888, anything else is a hard stop
  rsdp             u64
  madt             u64     ; 0 if not walked yet
  mcfg             u64     ; 0 if not walked yet
  ap_release       u64     ; physical u64; 0 = stay parked; unused in Gate 1a
  ap_stack_base    u64     ; physical; 0 in Gate 1a
  map_count        u32
  map              u64     ; physical pointer to MemRange[map_count]

MemRange
  kind             u32     ; 1 = WB DRAM, 2 = WC GOP, 3 = UC MMIO, 4 = firmware reserved
  pad              u32
  phys             u64
  bytes            u64
```

`kind 4` is recorded and not mapped write-back. DRAM holes stay holes. GOP is `kind 2`, mapped write-combining through `IA32_PAT`, and is not placed inside a write-back DRAM range. ECAM, the local APIC, and NVMe BARs are `kind 3` when they are known. Gate 1a may leave `mcfg` at 0 and map only WB ranges plus the GOP aperture.

Page size is 2 MiB where a WB range is aligned and long enough, 4 KiB otherwise. A 1 GiB page is allowed only when a single WB range covers that aligned gigabyte with no hole. The builder takes the filtered `MemRange` list. It does not assume eight entries.

### 8.3 Order of instructions after the firmware returns

1. `cli`.
2. `lgdt` of a four-entry table: null, 64-bit code, 64-bit data, TSS. `ltr`.
3. `CR4.OSFXSR | OSXSAVE | FSGSBASE | PCIDE | PKE`, then `xsetbv` with `XCR0 = 0xE7`. A vector instruction before this is a `#UD`. Gate 1a still does the `xsetbv`, so the later image does not discover it at the first `vmov`.
4. Program `IA32_PAT` so one entry is WC, then `mov cr3` from the `MemRange` list.
5. `lidt` of 256 gates. The stub writes the vector number to a known line and `hlt`s. No scheduler.
6. Paint the framebuffer with a function of `x` and `y` so a wrong stride is obvious. Stride is in bytes. Pixel format is BGRX8888 or the image halts before painting.
7. Poll port `0x64` bit 0, read `0x60`, draw a scancode cell. No USB and no I2C in this image.
8. `hlt`.

`x2APIC` and `IA32_TSC_DEADLINE` are Phase 3. Gate 1 does not need a timer.

### 8.4 Intrinsics added to `zero0` for this image

No others.

* `@cli` `@sti` `@hlt`
* `@lgdt(base, limit)` `@lidt(base, limit)` `@ltr(sel)`
* `@cr_read(n)` `@cr_write(n, v)` for `n` in `{0, 3, 4}`
* `@msr_read(id)` `@msr_write(id, v)`
* `@xsetbv(xcr, v)`
* `@port_in8(port)` `@port_out8(port, v)`
* `@port_in32` / `@port_out32` only if a probe shows a real need. Gate 1 keyboard is 8-bit.

Each intrinsic lowers to the one instruction. A wrong `n` or an unaligned GDTR base is a compile error when it is a constant.

`--target=efi-os` changes the container and the entry. It does not silently accept `@ring` or `<tile>`.

The PE image is a `PE32+` EFI application: DOS stub, COFF header, machine `0x8664`, subsystem EFI application, one executable section, one zero-filled data section. `zero0` writes the bytes. No external linker.

### 8.5 The test, before the laptop

QEMU with OVMF, firmware as the boot ROM, the image on a FAT directory. Pass condition is automatic:

* The image writes a fixed banner to port `0xE9` (QEMU `isa-debugcon`). A host script reads that log.
* The same image paints the GOP buffer. A wrong stride fails a second check that samples a pixel through a small OVMF-side pattern, or through the debugcon printing `width`, `height`, and `stride` from the handoff.

The HP machine is a later confirmation of the same image, not the first time the path is executed. Keyboard `0x60` on the laptop is the human check. `0xE9` will not exist there, and the image must not require it: the port write is behind a compile-time flag `qemu_debug`.

If `LocateProtocol` for GOP fails, the image returns to firmware. It does not guess a framebuffer address.

## 9. Phase 3 and Phase 4, only as gates

Phase 3 starts when Gate 1a has passed under QEMU. The work is the ABI in §6, a two-core `movdir64b` ping-pong, and `wait` on that line, with `rdtsc` values printed to the framebuffer. Those numbers replace any future latency sentence. The fault word moves to `gs:0` in this phase, with one test where two reactors `fail` without seeing each other's code.

Phase 4, self-hosting, starts when all of the following are true:

* Phase 0 tests are in `make test`.
* Gate 1a has passed.
* `.zr` can read a file and write a file on the Linux target, which means slices (`{ptr, len}`) or an equivalent `@arena` span with a pointer value. Today `@arena.base` is a bare integer and that is not enough to retarget the compiler comfortably.
* The `efi-os` emitter exists, so the self-hosted compiler has a second container worth emitting.

Until then, a compiler written in `.zr` has nothing to compile except the ELF dialect, and it would freeze Phase 0's bugs if Phase 0 were skipped.

## 10. Explicitly not in this plan

* Iris Xe, `MI_SEMAPHORE_WAIT`, NVMe admin, and `@capsule` loading. Gate 3 in `ZERO_OS_ARCHITECTURE.md` is unchanged and it is not next.
* `<simd>` and `k7`.
* PKU. Sixteen keys and a serializing `WRPKRU` can wait until an arena is actually frozen by the compiler.
* Saturating arithmetic, a register allocator, and an SSA pass. `rax`/`rcx` plus frame slots is still the right Stage-0 lowering.
* Any line-count or wattage target for the OS.
* Rewriting `zero0` in another language.

## 11. First change I would make

A single Phase 0 patch: unsigned `>>` / `/` / compares, brace rewind, slab bounds as `fail 2`, the bitpack `|` / `>` rule, `@cold` rejected, and the README corrections in §3. Golden tests for the probes in §2 that currently contradict the README. No flag, no PE bytes, no new keyword besides what those rules need.

When that patch is green, the next file added to `bootstrap/` is the PE writer and the intrinsic list in §8.4, and `parser.inc` stops growing except to call them.
