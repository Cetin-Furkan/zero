#!/usr/bin/env bash
set -euo pipefail

ZERO0="${1:-build/zero0}"
BUILD="build"
ZERO2="$BUILD/zero2"
mkdir -p "$BUILD/neg"

echo "=== [1/7] Self-Hosting Fixed-Point Bootstrap & Strict W^X Verification (zero0 -> zero1 -> zero2 == zero3) ==="
if readelf -lW "$ZERO0" | grep -q "RWE"; then
    echo "  [FAIL] $ZERO0 contains an RWX segment!" >&2
    exit 1
fi
echo "  [PASS] seed compiler $ZERO0 has strict W^X PT_LOAD segments (no RWE)"

"$ZERO0"       "compiler/zero1.zr" "$BUILD/zero1" > /dev/null
"$BUILD/zero1" "compiler/zero1.zr" "$ZERO2"       > /dev/null
"$ZERO2"       "compiler/zero1.zr" "$BUILD/zero3" > /dev/null
for zbin in "$BUILD/zero1" "$ZERO2" "$BUILD/zero3"; do
    if readelf -lW "$zbin" | grep -q "RWE"; then
        echo "  [FAIL] self-hosted compiler $zbin contains an RWX segment!" >&2
        exit 1
    fi
done
if ! cmp -s "$ZERO2" "$BUILD/zero3"; then
    echo "  [FAIL] Stage-2 ($ZERO2) and Stage-3 ($BUILD/zero3) differ (fixed-point failed)!" >&2
    exit 1
fi
echo "  [PASS] deterministic self-hosting fixed point proven: $ZERO2 == $BUILD/zero3 ($(stat -c%s "$ZERO2") bytes, strict W^X)"

"$ZERO2" --lex tests/lex_showcase.zr > "$BUILD/lex_showcase_z2.log"
grep -q "lex ok:" "$BUILD/lex_showcase_z2.log"
echo "  [PASS] [zero2] --lex verified on tests/lex_showcase.zr"

echo "=== [2/7] Full 10-Suite Golden Parity, Inline Silicon, AVX-512 EVEX, Next-Gen Spine, & ZeroFS Verification ==="
run_z2_golden() {
    local src="$1"
    local bin="$2"
    local expected="$3"
    local out_log="${bin}.stdout"

    "$ZERO2" "$src" "$bin" > /dev/null
    if readelf -lW "$bin" | grep -q "RWE"; then
        echo "  [FAIL] [zero2] guest binary $bin contains an RWX segment!" >&2
        exit 1
    fi
    "$bin" > "$out_log"
    diff -u "$expected" "$out_log"
    echo "  [PASS] [zero2] $src -> $bin (W^X + stdout matches $expected)"
}

run_z2_golden "examples/first.zr"              "$BUILD/z2_first"                 "tests/first.expected"
run_z2_golden "tests/step3_compute.zr"         "$BUILD/z2_step3_compute"         "tests/step3_compute.expected"
run_z2_golden "tests/step4_systems.zr"         "$BUILD/z2_step4_systems"         "tests/step4_systems.expected"
run_z2_golden "tests/phase0_scalar.zr"         "$BUILD/z2_phase0_scalar"         "tests/phase0_scalar.expected"
run_z2_golden "tests/phase1_zero_lang.zr"      "$BUILD/z2_phase1_zero_lang"      "tests/phase1_zero_lang.expected"
run_z2_golden "tests/phase2_zero_power.zr"     "$BUILD/z2_phase2_zero_power"     "tests/phase2_zero_power.expected"
run_z2_golden "tests/phase3_reactor_fabric.zr" "$BUILD/z2_phase3_reactor_fabric" "tests/phase3_reactor_fabric.expected"
run_z2_golden "tests/phase4_avx512_simd.zr"    "$BUILD/z2_phase4_avx512_simd"    "tests/phase4_avx512_simd.expected"
run_z2_golden "tests/phase5_nextgen_spine.zr"  "$BUILD/z2_phase5_nextgen_spine"  "tests/phase5_nextgen_spine.expected"
run_z2_golden "tests/phase6_zerofs_ledger.zr"  "$BUILD/z2_phase6_zerofs_ledger"  "tests/phase6_zerofs_ledger.expected"

# Verify literal silicon & AVX-512 EVEX opcodes emitted directly in zero2 guest `.text` (offset 176 = 64B Ehdr + 2*56B Phdr)
DIS_Z2_POWER=$(objdump -D -b binary -m i386:x86-64 --start-address=176 "$BUILD/z2_phase2_zero_power")
for op in "movnti" "lock xadd" "lock cmpxchg" "popcnt" "lzcnt" "tzcnt" "pdep" "pext" "crc32" "prefetcht0" "bswap" "sfence" "lfence" "mfence"; do
    if ! grep -q "$op" <<< "$DIS_Z2_POWER"; then
        echo "  [FAIL] $BUILD/z2_phase2_zero_power missing inline silicon instruction: $op" >&2
        exit 1
    fi
done
DIS_Z2_FAB=$(objdump -D -b binary -m i386:x86-64 --start-address=176 "$BUILD/z2_phase3_reactor_fabric")
for op in "movdir64b" "movdiri" "cpuid"; do
    if ! grep -q "$op" <<< "$DIS_Z2_FAB"; then
        echo "  [FAIL] $BUILD/z2_phase3_reactor_fabric missing direct-silicon instruction: $op" >&2
        exit 1
    fi
done
DIS_Z2_SIMD=$(objdump -D -b binary -m i386:x86-64 --start-address=176 "$BUILD/z2_phase4_avx512_simd")
for op in "vmovdqu32" "vmovdqu64" "{%k1}{z}" "kmovw" "bzhi" "vmovntdq" "vpbroadcastd" "vpbroadcastq" "vpaddd" "vpaddq" "vpmulld" "vpmullq" "vpsllvq" "incq"; do
    if ! grep -q "$op" <<< "$DIS_Z2_SIMD"; then
        echo "  [FAIL] $BUILD/z2_phase4_avx512_simd missing AVX-512 EVEX / GPR instruction: $op" >&2
        exit 1
    fi
done
if ! grep -Eq 'vmovdqu(32|64).*\{%k1\}$' <<< "$DIS_Z2_SIMD"; then
    echo "  [FAIL] $BUILD/z2_phase4_avx512_simd missing write-masked {%k1} store!" >&2
    exit 1
fi
echo "  [PASS] [zero2] verified inline silicon + 512-bit AVX-512 EVEX opcodes (movdir64b, movdiri, movnti, vmovntdq, {%k1}{z}/{%k1}, kmovw, bzhi, ZMM ALU, incq)"

# Verify Stage-1 Next-Gen Spine machine instructions (`bextr`, `bt`, `blsr`, `tzcnt`, `pause`, `@oneshot` relay/wipe, & zero-`cmp` after `mask<...>`)
DIS_Z2_SPINE=$(objdump -D -b binary -m i386:x86-64 -M intel --start-address=176 "$BUILD/z2_phase5_nextgen_spine")
for op in "bextr" "bt " "blsr" "tzcnt" "pause" "crc32" "mov    r9,r10" "xor    r10d,r10d" "xor    r9d,r9d"; do
    if ! grep -q "$op" <<< "$DIS_Z2_SPINE"; then
        echo "  [FAIL] $BUILD/z2_phase5_nextgen_spine missing Stage-1 hardware instruction/sequence: $op" >&2
        exit 1
    fi
done
# Prove ZERO `cmp` sits between `pause; dec rcx; jne` spin timeout and `when ZF => fail` branch:
if ! awk '/\tpause$/{p=1; c=0} p{c++; if (/\tcmp/) exit 1; if (c==6) { if ($0 ~ /\tjne/) ok=1; p=0 }} END{exit(ok?0:1)}' <<< "$DIS_Z2_SPINE"; then
    echo "  [FAIL] $BUILD/z2_phase5_nextgen_spine emitted a redundant cmp after 3-part spin mask<...>!" >&2
    exit 1
fi
# Prove ZERO `cmp` sits between fused `bextr` (`0x32e` = `PIECES_BITS >> 46`) and `when ZF` branch:
if ! awk '/0x32e/{p=1; c=0} p{c++; if (/cmp/) exit 1; if (c==4) { if ($0 ~ /jne/) ok=1; p=0 }} END{exit(ok?0:1)}' <<< "$DIS_Z2_SPINE"; then
    echo "  [FAIL] $BUILD/z2_phase5_nextgen_spine emitted a redundant cmp after fused bextr mask<...>!" >&2
    exit 1
fi
echo "  [PASS] [zero2] verified Stage-1 Next-Gen Spine opcodes (bextr, bt, blsr, tzcnt, pause, @oneshot mov+xor register wipes, & zero-cmp RFLAGS fusion)"

# Verify Stage-2 ZeroFS (`ZLEDGER1`) 128-bit XMM SHA-NI + AES-NI + XMM key-wipe + MOVDIR64B + @oneshot opcodes
DIS_Z2_ZFS=$(objdump -D -b binary -m i386:x86-64 -M intel --start-address=176 "$BUILD/z2_phase6_zerofs_ledger")
for op in "sha256rnds2" "sha256msg1" "sha256msg2" "aeskeygenassist" "aesenc " "aesenclast" "pxor   xmm4,xmm4" "pxor   xmm5,xmm5" "movdir64b" "bextr" "blsr" "mov    r9,r10" "xor    r10d,r10d" "xor    r9d,r9d"; do
    if ! grep -q "$op" <<< "$DIS_Z2_ZFS"; then
        echo "  [FAIL] $BUILD/z2_phase6_zerofs_ledger missing Stage-2 ZeroFS hardware instruction: $op" >&2
        exit 1
    fi
done
echo "  [PASS] [zero2] verified Stage-2 ZeroFS opcodes (sha256rnds2/msg1/msg2, aeskeygenassist/aesenc/aesenclast, pxor XMM wipes, movdir64b, @oneshot)"

echo "=== [3/7] Adversarial Negative Rejection Probes on Self-Hosted zero2 (ELF64, EFI, & Capsule) ==="
expect_compile_reject() {
    local name="$1"
    local code="$2"
    local expected_err="$3"
    local src="$BUILD/neg/${name}.zr"
    local err_log="$BUILD/neg/${name}.z2.err"

    printf "%s\n" "$code" > "$src"
    if "$ZERO2" "$src" "$BUILD/neg/${name}.z2.bin" 2> "$err_log" > /dev/null; then
        echo "  [FAIL] $name unexpectedly compiled under $ZERO2!" >&2
        exit 1
    fi
    grep -q -- "$expected_err" "$err_log"
    echo "  [PASS] [zero2] rejected $name ($(cat "$err_log"))"
}

expect_efi_reject() {
    local name="$1"
    local code="$2"
    local expected_err="$3"
    local src="$BUILD/neg/${name}.zr"
    local err_log="$BUILD/neg/${name}.z2.err"

    printf "%s\n" "$code" > "$src"
    if "$ZERO2" --target=efi-os "$src" "$BUILD/neg/${name}.z2.efi" 2> "$err_log" > /dev/null; then
        echo "  [FAIL] $name unexpectedly compiled under $ZERO2 --target=efi-os!" >&2
        exit 1
    fi
    grep -q -- "$expected_err" "$err_log"
    echo "  [PASS] [zero2] rejected $name ($(cat "$err_log"))"
}

expect_capsule_reject() {
    local name="$1"
    local code="$2"
    local expected_err="$3"
    local src="$BUILD/neg/${name}.zr"
    local err_log="$BUILD/neg/${name}.z2.err"

    printf "%s\n" "$code" > "$src"
    if "$ZERO2" --target=capsule "$src" "$BUILD/neg/${name}.z2.zcap" 2> "$err_log" > /dev/null; then
        echo "  [FAIL] $name unexpectedly compiled under $ZERO2 --target=capsule!" >&2
        exit 1
    fi
    grep -q -- "$expected_err" "$err_log"
    echo "  [PASS] [zero2] rejected $name ($(cat "$err_log"))"
}

expect_compile_reject "fake_nonrange_simd" \
    'fn main() { for i in 5 <simd: 16> { print(i) } }' \
    "unimplemented or invalid loop lens"

expect_compile_reject "fake_range_tile" \
    'fn main() { for i in 0..8 <tile: 32KB> { print(i) } }' \
    "unimplemented or invalid loop lens"

expect_compile_reject "duplicate_const" \
    $'A :: 1\nA :: 2\nfn main() { print(A) }' \
    "duplicate top-level symbol"

expect_compile_reject "cyclic_const" \
    $'A :: B + 1\nB :: A + 1\nfn main() { print(A) }' \
    "unresolved or cyclic constant"

expect_compile_reject "bad_bitpack_width_48" \
    $'Bad :: <a: u32 | b: u16>\nfn main() { }' \
    "bit-layout width must sum to 8, 16, 32, or 64"

expect_compile_reject "unimpl_bogus_space" \
    $'@bogus_space cap0 : <cap: 2MB>\nfn main() { }' \
    "unimplemented '@' space"

expect_compile_reject "bad_capsule_unaligned" \
    $'@capsule cap0 : <cap: 1000>\nfn main() { }' \
    "@capsule requires 4KiB-aligned cap"

expect_capsule_reject "bad_slab_in_capsule_target" \
    $'@slab s : <u64 | max: 4>\nfn main() i64 { return 0 }' \
    "forbids mutable @slab/@ring"

expect_compile_reject "unimpl_cold_attr" \
    $'S :: { x: i64, @cold y: i64 }\nfn main() { }' \
    "unimplemented '@cold' split"

expect_compile_reject "dead_block_var" \
    'fn main() { { x := 1 } print(x) }' \
    "parse fault"

expect_compile_reject "dup_same_block_var" \
    'fn main() { x := 1 x := 2 }' \
    "duplicate variable in same block"

expect_compile_reject "dup_param_in_body" \
    $'fn f(p: i64) { p := 2 }\nfn main() { f(1) }' \
    "duplicate variable in same block"

expect_compile_reject "dead_for_var" \
    'fn main() { for k in 0..3 { } print(k) }' \
    "parse fault"

expect_compile_reject "dead_recover_var" \
    'fn main() { { fail 5 } recover e { } print(e) }' \
    "parse fault"

expect_compile_reject "undefined_fn_call" \
    'fn main() { ghost_fn(1) }' \
    "undefined function"

expect_compile_reject "undefined_fn_addr" \
    'fn main() { p := @fn_addr(ghost_fn) }' \
    "undefined function"

# Gate-1a/1b Target Isolation & Intrinsic Static Validation (§8.4)
expect_compile_reject "ring0_cli_on_host_elf" \
    'fn main() { @cli() }' \
    "ring-0 intrinsic requires --target=efi-os"

expect_compile_reject "pause_on_host_elf" \
    'fn main() { @pause() }' \
    "ring-0 intrinsic requires --target=efi-os"

expect_compile_reject "rdtsc_on_host_elf" \
    'fn main() { t := @rdtsc() }' \
    "ring-0 intrinsic requires --target=efi-os"

expect_compile_reject "port_on_host_elf" \
    $'@port dbg : <u8 | 0xE9>\nfn main() {}' \
    "ring-0 intrinsic requires --target=efi-os"

expect_compile_reject "mmio_on_host_elf" \
    'fn main() { m := @mmio<u32>(0x1000) }' \
    "ring-0 intrinsic requires --target=efi-os"

expect_efi_reject "host_print_on_efi" \
    'fn main(h: u64) i64 { print("no") return 0 }' \
    "host print/@syscall unavailable in --target=efi-os"

expect_efi_reject "host_syscall_on_efi" \
    'fn main(h: u64) i64 { @syscall(60, 0) return 0 }' \
    "host print/@syscall unavailable in --target=efi-os"

expect_efi_reject "bad_cr2_read" \
    'fn main(h: u64) i64 { x := @cr_read(2) return x }' \
    "invalid or unaligned ring-0 intrinsic constant"

expect_efi_reject "bad_cr_const_write" \
    $'BAD_CR :: 5\nfn main(h: u64) i64 { @cr_write(BAD_CR, 0) return 0 }' \
    "invalid or unaligned ring-0 intrinsic constant"

expect_efi_reject "unaligned_lgdt_literal" \
    'fn main(h: u64) i64 { @lgdt(3, 39) return 0 }' \
    "invalid or unaligned ring-0 intrinsic constant"

expect_efi_reject "unaligned_lgdt_const_expr" \
    $'BAD_BASE :: 0x1004\nfn main(h: u64) i64 { @lgdt(BAD_BASE, 39) return 0 }' \
    "invalid or unaligned ring-0 intrinsic constant"

expect_efi_reject "unaligned_lidt_literal" \
    'fn main(h: u64) i64 { @lidt(0x2002, 4095) return 0 }' \
    "invalid or unaligned ring-0 intrinsic constant"

expect_efi_reject "efi_rejects_bogus_space" \
    $'@bogus cap0 : <cap: 2MB>\nfn main(h: u64) i64 { return 0 }' \
    "unimplemented '@' space"

expect_efi_reject "efi_still_rejects_tile_size" \
    'fn main(h: u64) i64 { for i in 0..8 <tile: 32KB> { } return 0 }' \
    "unimplemented or invalid loop lens"

expect_efi_reject "bad_port_width_i64" \
    $'@port bad : <u64 | 0x60>\nfn main(h: u64) i64 { return 0 }' \
    "invalid @port width or port number"

expect_efi_reject "bad_port_range" \
    $'@port bad : <u8 | 0x10000>\nfn main(h: u64) i64 { return 0 }' \
    "invalid @port width or port number"

expect_compile_reject "bad_bitslice_order" \
    'fn main() { x := 5 y := x[2:5] }' \
    "parse fault"

expect_compile_reject "bad_bitslice_64" \
    'fn main() { x := 5 y := x[64:0] }' \
    "parse fault"

expect_compile_reject "bad_ptr_nonpow2_wrap" \
    $'BadP :: @ptr<u64 | wrap: 6>\nfn main() {}' \
    "parse fault"

expect_compile_reject "bad_unroll_factor_3" \
    'fn main() { for i in 0..9 <unroll: 3> {} }' \
    "unimplemented or invalid loop lens"

expect_compile_reject "bad_stream_u8" \
    'fn main() { s := @stream<u8>(0x1000, 4) }' \
    "parse fault"

# Phase-3 / Gate-2 Reactor ABI, `@cell64`, `@ring`, `publish`, `wait`, `<tile>`, & `@isr` Verification
expect_compile_reject "tile_on_host_elf" \
    'fn main() { tile { } }' \
    "ring-0 intrinsic requires --target=efi-os"

expect_compile_reject "wait_on_host_elf" \
    'fn main() { wait(0x1000, 0) }' \
    "ring-0 intrinsic requires --target=efi-os"

expect_compile_reject "bad_cell64_overflow" \
    $'Big :: @cell64 { a: u64, b: u64, c: u64, d: u64, e: u64, f: u64, g: u64, h: u64, i: u64 }\nfn main() {}' \
    "@cell64 layout exceeds 64-byte hardware cacheline"

expect_compile_reject "bad_ring_non64_elem" \
    $'Small :: { x: u64 }\n@ring r : <Small | cap: 4>\nfn main() {}' \
    "@ring/@dma_queue requires valid aligned element size and power-of-2 cap"

expect_compile_reject "bad_ring_nonpow2_cap" \
    $'C :: @cell64 { x: u64 }\n@ring r : <C | cap: 6>\nfn main() {}' \
    "@ring/@dma_queue requires valid aligned element size and power-of-2 cap"

expect_compile_reject "bad_publish_non_cell64" \
    $'S :: { x: u64 }\nfn main() { s := S{ x: 1 } publish(0x1000, s) }' \
    "publish requires 64-byte @cell64"

expect_efi_reject "bad_isr_touches_fail" \
    $'@isr fn irq() { fail 1 }\nfn main(h: u64) i64 { return 0 }' \
    "@isr/@isr_ok forbids"

expect_efi_reject "bad_isr_touches_r12" \
    $'@isr fn irq() { x := @reactor_id() }\nfn main(h: u64) i64 { return 0 }' \
    "@isr/@isr_ok forbids"

expect_efi_reject "bad_isr_calls_normal_fn" \
    $'fn norm() {}\n@isr fn irq() { norm() }\nfn main(h: u64) i64 { return 0 }' \
    "@isr/@isr_ok forbids"

expect_efi_reject "bad_isr_ok_non_leaf" \
    $'@isr_ok fn a() {}\n@isr_ok fn b() { a() }\nfn main(h: u64) i64 { return 0 }' \
    "@isr/@isr_ok forbids"

# Phase-4 / Track 2 Zero-XSAVE Ring-0 AVX-512 `<tile>` Law & `<simd>` Lens Validation
expect_efi_reject "simd_outside_tile_on_efi" \
    $'@slab s : <u64 | max: 16>\nfn main(h: u64) i64 { for i in 0..8 <simd: 8> { s[i] = i } return 0 }' \
    "AVX-512 <simd> in Ring-0 requires enclosing <tile>"

expect_efi_reject "vec_outside_tile_on_efi" \
    'fn main(h: u64) i64 { v: u32x16 = @vec_iota32() return @vec_sum32(v) }' \
    "AVX-512 <simd> in Ring-0 requires enclosing <tile>"

expect_capsule_reject "vec_outside_tile_on_capsule" \
    'fn main() i64 { v: u64x8 = @vec_iota64() return @vec_sum64(v) }' \
    "AVX-512 <simd> in Ring-0 requires enclosing <tile>"

expect_efi_reject "simd_in_isr" \
    $'@isr fn irq() { v: u64x8 = @vec_iota64() }\nfn main(h: u64) i64 { return 0 }' \
    "@isr/@isr_ok forbids"

expect_efi_reject "vec_in_isr_ok" \
    $'@isr_ok fn helper() u64 { v := @vec_splat64(1) return @vec_sum64(v) }\nfn main(h: u64) i64 { return 0 }' \
    "@isr/@isr_ok forbids"

expect_compile_reject "bad_simd_width_3" \
    $'@slab s : <u64 | max: 16>\nfn main() { for i in 0..8 <simd: 3> { s[i] = i } }' \
    "unimplemented or invalid loop lens"

expect_compile_reject "bad_simd_nonvec_call" \
    $'@slab s : <u64 | max: 16>\nfn foo(x: i64) u64 { return x }\nfn main() { for i in 0..8 <simd: 8> { s[i] = foo(i) } }' \
    "unimplemented or invalid loop lens"

# Phase-5 / Stage-1 Next-Gen `Zero` Spine (`;`, `struct @cell64`, `ptr* @oneshot`, `ref&`, `mask<...>`, `RFLAGS` liveness, `@ring`)
expect_compile_reject "semi_end_of_line" \
    $'fn main() {\n    x := 1;\n    print(x)\n}' \
    "';' at end of line is forbidden"

expect_compile_reject "semi_stmt_separator" \
    'fn main() { x := 1 ; y := 2 }' \
    "';' is only legal as an inline stage-to-flag/loop handoff"

expect_compile_reject "bad_struct_cell64_48b" \
    $'struct BadTicket @cell64 {\n    a: u64\n    b: u64\n    c: u64\n    d: u64\n    e: u64\n    f: u64\n}\nfn main() {}' \
    "struct @cell64 must be exactly 64 bytes"

expect_compile_reject "oneshot_escape_plain_ptr" \
    $'@slab s : u64[8]\nfn main() {\n    p : ptr* @oneshot u64 = s.base\n    q : ptr* u64 = p\n}' \
    "@oneshot pointer cannot be copied"

expect_compile_reject "oneshot_double_deref" \
    $'@slab s : u64[8]\nfn main() {\n    p : ptr* @oneshot u64 = s.base\n    a := p[0]\n    b := p[0]\n}' \
    "@oneshot pointer already consumed"

expect_compile_reject "oneshot_use_after_relay" \
    $'@slab s : u64[8]\nfn main() {\n    p : ptr* @oneshot u64 = s.base\n    q : ptr* @oneshot u64 = p\n    a := p[0]\n    b := q[0]\n}' \
    "@oneshot pointer already consumed"

expect_compile_reject "oneshot_across_call" \
    $'@slab s : u64[8]\nfn helper() u64 => 1\nfn main() {\n    p : ptr* @oneshot u64 = s.base\n    _ := helper()\n    a := p[0]\n}' \
    "@oneshot pointer cannot remain live across a function call"

expect_compile_reject "oneshot_unconsumed_scope" \
    $'@slab s : u64[8]\nfn main() {\n    p : ptr* @oneshot u64 = s.base\n}' \
    "unconsumed @oneshot pointer"

expect_compile_reject "oneshot_asymmetric_branch" \
    $'@slab s : u64[8]\nfn f(c: u64) u64 {\n    p : ptr* @oneshot u64 = s.base\n    when c == 1 {\n        _ := p[0]\n    }\n    return 0\n}\nfn main() { _ := f(0) }' \
    "@oneshot pointer consumed inside single-arm"

expect_compile_reject "oneshot_in_struct" \
    $'struct Leak { p : ptr* @oneshot u64 }\nfn main() {}' \
    "@oneshot pointer cannot be stored in a struct"

expect_compile_reject "ref_rvalue_literal" \
    $'fn bump(x: ref& u64) { x += 1 }\nfn main() { bump(42) }' \
    "ref& requires an addressable mutable variable"

expect_compile_reject "ref_oneshot_target" \
    $'@slab s : u64[8]\nfn main() {\n    p : ptr* @oneshot u64 = s.base\n    r : ref& u64 = p\n}' \
    "cannot take ref& of register-only @oneshot"

expect_compile_reject "dead_zf_clobbered" \
    $'@slab db : u32[4]\nfn main() {\n    p : ptr* u32 = db.base\n    s := mask<p[0] | 1 | until !ZF ; max 8>\n    clobber := s + 1\n    when ZF => fail 5\n}' \
    "CPU flag referenced in 'when' is dead, clobbered, or mismatched"

expect_compile_reject "cf_multibit_mask" \
    $'fn main() {\n    x : u64 = 3\n    with when mask<x | 0b11> ; CF {\n        print(1)\n    }\n}' \
    "mask<...> ; CF requires a single-bit mask"

expect_compile_reject "ring_loop_on_slab" \
    $'@slab s : u64[16]\nfn main() {\n    for idx, v in s @ring(0 .. 4) { print(v) }\n}' \
    "@ring(start .. len) loop requires an @ring space"

expect_compile_reject "spin_mask_on_scalar_local" \
    $'fn main() {\n    x : u32 = 0\n    _ := mask<x | 1 | until !ZF ; max 16>\n}' \
    "3-part spin mask<...> requires a dereferenced memory/pointer/space target"

# --- Stage-2 Hardware `@domain` & `128-Bit XMM` Crypto Negative Probes ---
expect_compile_reject "tigerlake_npu_domain_rejected" \
    $'@domain bad_accel : npu\nfn main() { }' \
    "Tiger Lake (i7-1165G7) has no NPU silicon block; use 'gna' domain"

expect_compile_reject "unknown_hw_domain_kind" \
    $'@domain bad_dev : quantum_tpu\nfn main() { }' \
    "unknown hardware @domain kind"

expect_compile_reject "domain_indexed_as_dram" \
    $'@domain d : nvme\nfn main() {\n    x := d[0]\n    print(x)\n}' \
    "hardware @domain is not DRAM"

expect_capsule_reject "domain_in_capsule_target" \
    $'@domain d : gna\nfn main(a: u64, b: u64) u64 { return a + b }' \
    "--target=capsule forbids mutable"

expect_efi_reject "sha256_in_isr_rejected" \
    $'@isr fn bad_isr() {\n    _ := @sha256_tag(1, 2)\n}\nfn main() i64 => 0' \
    "@isr/@isr_ok forbids 128-bit XMM crypto intrinsics"

expect_efi_reject "aes128_in_isr_ok_rejected" \
    $'@slab b : u8[64]\n@isr_ok fn bad_leaf() u64 {\n    p := ptr* u8 (b.base)\n    return @aes128_ctr64(p, p, 1, 2)\n}\nfn main() i64 => 0' \
    "@isr/@isr_ok forbids 128-bit XMM crypto intrinsics"

expect_compile_reject "extent_word_indexed_without_staging" \
    $'fn main() {\n    ext_word : u64 = 0x18000\n    v := ext_word[0]\n    print(v)\n}' \
    "parse fault"

echo "=== [4/7] Runtime Unhandled Fault Exit Check (Self-Hosted zero2) ==="
printf 'fn main() { fail 99 }\n' > "$BUILD/neg/unhandled_fail.zr"
"$ZERO2" "$BUILD/neg/unhandled_fail.zr" "$BUILD/neg/unhandled_fail_z2.bin" > /dev/null
set +e
"$BUILD/neg/unhandled_fail_z2.bin"
rc=$?
set -e
if [[ "$rc" -ne 1 ]]; then
    echo "  [FAIL] unhandled_fail_z2 exited with $rc instead of 1" >&2
    exit 1
fi
echo "  [PASS] [zero2] unhandled runtime fail exited cleanly with status $rc"

echo "=== [5/7] 100% Zero-NASM Self-Hosted PE32+ UEFI OS & Multi-@capsule Image Verification (build/zero2) ==="
# Three CRC32C-signed Ring-0 `.zcap` capsules (NVMe Extents 0..2), all compiled by self-hosted `zero2`
"$ZERO2" --target=capsule "examples/capsule_compute.zr"           "$BUILD/capsule_z2.zcap"      > /dev/null
"$ZERO2" --target=capsule "examples/capsules/cap_avx512_wave.zr"  "$BUILD/cap_avx512_wave.zcap" > /dev/null
"$ZERO2" --target=capsule "examples/capsules/cap_matrix_crc.zr"   "$BUILD/cap_matrix_crc.zcap"  > /dev/null
for zcap in "$BUILD/capsule_z2.zcap" "$BUILD/cap_avx512_wave.zcap" "$BUILD/cap_matrix_crc.zcap"; do
    if [[ "$(head -c 8 "$zcap")" != "ZEROZCAP" ]]; then
        echo "  [FAIL] $zcap missing ZEROZCAP header magic!" >&2
        exit 1
    fi
done
DIS_WAVE=$(objdump -D -b binary -m i386:x86-64 --start-address=64 "$BUILD/cap_avx512_wave.zcap")
for op in "vpbroadcastq" "vpmullq" "vpaddq" "vpxorq" "vzeroupper"; do
    if ! grep -q "$op" <<< "$DIS_WAVE"; then
        echo "  [FAIL] $BUILD/cap_avx512_wave.zcap missing AVX-512 instruction: $op" >&2
        exit 1
    fi
done
DIS_MTX=$(objdump -D -b binary -m i386:x86-64 --start-address=64 "$BUILD/cap_matrix_crc.zcap")
for op in "crc32" "pext" "pdep" "rol"; do
    if ! grep -q "$op" <<< "$DIS_MTX"; then
        echo "  [FAIL] $BUILD/cap_matrix_crc.zcap missing silicon instruction: $op" >&2
        exit 1
    fi
done
echo "  [PASS] [zero2] emitted 3 CRC32C-signed --target=capsule extents (compute $(stat -c%s "$BUILD/capsule_z2.zcap")B, AVX-512 wave $(stat -c%s "$BUILD/cap_avx512_wave.zcap")B, matrix-CRC $(stat -c%s "$BUILD/cap_matrix_crc.zcap")B)"

# Verify Stage-1 Next-Gen Spine (`ptr*`, `ref&`, `ptr* @oneshot`, `mask<...>`, `when`, `@bits`) on bare-metal `--target=capsule` Reactor ABI (`%gs:0`, `r12`)
cat > "$BUILD/neg/cap_spine_probe.zr" << 'EOF'
CRYPT_BIT   :: 1 << 44
PIECES_BITS :: 0b111 << 46
ADDR42_MASK :: 0x0000_03FF_FFFF_FFFF

struct CapNode @cell64 {
    exact_len : u64
    key_tag   : u64
    pieces    : u64[5]
    next_ext  : u64
}

fn cap_unlock(arena: ptr* u8, ext: u64, key: u64, out_len: ref& u64) u64 {
    with pcs := mask<ext | PIECES_BITS> >> 46 ; when !ZF {
        node := ptr* CapNode (arena + (mask<ext | ADDR42_MASK> << 6))
        out_len = node[0].exact_len + pcs
    }
    with when mask<ext | CRYPT_BIT> ; !ZF {
        s0 : ptr* @oneshot u64 = arena + 64
        s1 : ptr* @oneshot u64 = s0
        return s1[0] ^ key ^ @reactor_id()
    }
    fail 4
    return 0
}

fn main(base_raw: u64, ext: u64) u64 {
    p : ptr* u8 = base_raw
    seen : u64 = 0
    acc  : u64 = cap_unlock(p, ext, 0x5A, seen)
    when for b in seen @bits {
        acc = @crc32c(acc, b)
    }
    return acc
}
EOF
"$ZERO2" --target=capsule "$BUILD/neg/cap_spine_probe.zr" "$BUILD/cap_spine_probe.zcap" > /dev/null
DIS_CAP_SPINE=$(objdump -D -b binary -m i386:x86-64 -M intel --start-address=64 "$BUILD/cap_spine_probe.zcap")
for op in "bextr" "mov    r9,r10" "xor    r10d,r10d" "xor    r9d,r9d" "blsr" "gs:0x0"; do
    if ! grep -q "$op" <<< "$DIS_CAP_SPINE"; then
        echo "  [FAIL] $BUILD/cap_spine_probe.zcap missing Ring-0 Next-Gen Spine instruction: $op" >&2
        exit 1
    fi
done
echo "  [PASS] [zero2] verified Stage-1 Next-Gen Spine on Ring-0 --target=capsule Reactor ABI (ZEROZCAP + gs:0 + r12 + @oneshot wipes + bextr)"

"$ZERO2" --target=efi-os "examples/gate1_os.zr" "$BUILD/gate1_os_z2.efi" > /dev/null
HDR_EFI=$(objdump -p "$BUILD/gate1_os_z2.efi")
DIS_EFI=$(objdump -d "$BUILD/gate1_os_z2.efi")
grep -q "Subsystem[[:space:]]*0000000a[[:space:]]*(EFI application)" <<< "$HDR_EFI"
if objdump -x "$BUILD/gate1_os_z2.efi" | grep -E "^\s*[0-9]+\s+\." -A1 | paste - - | grep -q "EXECUTE.*WRITE\|WRITE.*EXECUTE"; then
    echo "  [FAIL] $BUILD/gate1_os_z2.efi contains a writable+executable PE32+ section!" >&2
    exit 1
fi
for efi_op in "movdir64b" "monitor" "mwait" "iretq" "vmovntdq" "vzeroupper" "{%k1}{z}" "kmovw" "sfence" "crc32" "sha256rnds2" "aesenc" "aesenclast" "%gs:0x0" "%gs:0x8"; do
    if ! grep -q "$efi_op" <<< "$DIS_EFI"; then
        echo "  [FAIL] $BUILD/gate1_os_z2.efi missing hardware instruction/prefix: $efi_op" >&2
        exit 1
    fi
done
echo "  [PASS] [zero2] examples/gate1_os.zr -> $BUILD/gate1_os_z2.efi (strict W^X PE32+ + AVX-512 vmovntdq/{%k1}/vzeroupper + movdir64b + monitor/mwait + iretq + %gs:0)"

# Verify `qemu_debug :: 0` laptop build under `zero2` eliminates all 0xE9 port I/O (direct imm8 AND `mov dx` forms)
ln -sfn ../examples/os "$BUILD/os"
sed 's/^qemu_debug[[:space:]]*:: 1/qemu_debug          :: 0/' examples/gate1_os.zr > "$BUILD/gate1_laptop.zr"
"$ZERO2" --target=efi-os "$BUILD/gate1_laptop.zr" "$BUILD/gate1_laptop_z2.efi" > /dev/null
dbg_e9=$(grep -Ec '\b(in|out)\b.*0xe9' <<< "$DIS_EFI" || true)
if [[ "$dbg_e9" -eq 0 ]]; then
    echo "  [FAIL] positive control failed: debug gate1_os_z2.efi shows 0 port-0xE9 instructions (regex broken?)" >&2
    exit 1
fi
DIS_LAPTOP=$(objdump -d "$BUILD/gate1_laptop_z2.efi")
if grep -Eq '\b(in|out)\b.*0xe9' <<< "$DIS_LAPTOP" || grep -Eq '\b(in|out)\b +[^,]*,?\(%dx\)|\b(in|out)\b +\(%dx\)' <<< "$DIS_LAPTOP"; then
    echo "  [FAIL] gate1_laptop_z2.efi still contains port 0xE9 / %dx-indirect port I/O with qemu_debug :: 0" >&2
    exit 1
fi
echo "  [PASS] [zero2] laptop variant (qemu_debug :: 0) compiled -> $BUILD/gate1_laptop_z2.efi (0xE9 eliminated; debug build control = $dbg_e9 port-0xE9 insns)"

echo "=== [6/7] Interactive Tickless ZeroOS Command Deck: IRQ1/MWAIT + 3 NVMe @capsule Extents on 4 Reactors (QEMU Q35 + OVMF) ==="
python3 - <<'PY'
import hashlib, os, re, shutil, socket, struct, subprocess, sys, time

ovmf = "/usr/share/edk2/x64/OVMF_CODE.4m.fd"
if not os.path.exists(ovmf):
    print("  [FAIL] OVMF firmware missing at", ovmf, file=sys.stderr)
    sys.exit(1)

os.makedirs("build/esp/EFI/BOOT", exist_ok=True)
shutil.copyfile("build/gate1_os_z2.efi", "build/esp/EFI/BOOT/BOOTX64.EFI")

def crc32c_u64(crc, val64):
    crc &= 0xFFFFFFFF
    for i in range(8):
        crc ^= (val64 >> (8 * i)) & 0xFF
        for _ in range(8):
            crc = (crc >> 1) ^ (0x82F63B78 if (crc & 1) else 0)
    return crc

def crc32c_seq_u64(words):
    acc = 0xFFFFFFFF
    for w in words:
        acc = crc32c_u64(acc, w & 0xFFFFFFFFFFFFFFFF)
    return (~acc) & 0xFFFFFFFF

# 16 MiB Unified NVMe disk (`ZERO_NEXT_GEN_SPEC.md` §3.2):
#   • Bucket 0 (`0x0000_0000`): `ZEXTENT1` Master Extent Table + 3 CRC32C-signed `.zcap` boot capsules
#   • Bucket 1 (`0x0020_0000`): `ZLEDGER1` Superblock Mirror A (`epoch=1`, `snap=0xFFFFFFFE`)
#   • Bucket 2 (`0x0040_0000`): `ZLEDGER1` Superblock Mirror B (initially 0; populated on first CoW commit!)
#   • Bucket 3 (`0x0060_0000`): Initial `4 KiB` Eytzinger `CRC32C` B-Tree Root (`ZEYTZND1`, `count=0`)
CAPS = [("build/capsule_z2.zcap",      0x5A32434150, 4096),
        ("build/cap_avx512_wave.zcap", 0x5A32574156, 8192),
        ("build/cap_matrix_crc.zcap",  0x5A324D5458, 12288)]
disk = bytearray(16 * 1024 * 1024)
struct.pack_into("<QIIQ", disk, 0, 0x31544E455458455A, 1, len(CAPS), 1)
for i, (path, id_hash, off) in enumerate(CAPS):
    cap = open(path, "rb").read()
    assert len(cap) <= 4096, f"{path} exceeds its 4 KiB extent slot"
    crc = struct.unpack_from("<I", cap, 20)[0]
    struct.pack_into("<QQQII", disk, 64 + i * 32, id_hash, off, len(cap), crc, 1)
    disk[off:off + len(cap)] = cap

BUCKET_BYTES = 2 * 1024 * 1024
ZLEDGER_MAGIC = 0x3152454744454C5A
ZEYTZ_MAGIC   = 0x31444E5A5459455A

# Stamp initial Eytzinger root at `0x0060_0000` (`Bucket 3`, `epoch=1`, `count=0`)
root_off = 3 * BUCKET_BYTES
eytz_qwords = [ZEYTZ_MAGIC, 1, 0, 0, 0, 0, 0]
eytz_crc = crc32c_seq_u64([*eytz_qwords, 0])
struct.pack_into("<7QII", disk, root_off, *eytz_qwords, 0, eytz_crc)

# Encode `root_ext` for `(byte_off=0x600000, byte_len=4096, perm=1[RO], crypt=2[CRC_SEAL], pieces=0, hdr=0)`
root_ext = (root_off >> 6) | (1 << 42) | (2 << 44) | (64 << 50) | (1 << 61)
gen_pack = 0x0001000100010001
sb_hdr_bytes = struct.pack("<QIIQQIIQQ", ZLEDGER_MAGIC, 1, 1, 1, root_ext, 0xFFFFFFFE, 8, (1 << 3), gen_pack)
sb_qwords = list(struct.unpack("<7Q", sb_hdr_bytes))
sb_crc = crc32c_seq_u64([*sb_qwords, 0])
disk[1 * BUCKET_BYTES : 1 * BUCKET_BYTES + 64] = sb_hdr_bytes + struct.pack("<II", 0, sb_crc)
bucket0_sha_before = hashlib.sha256(disk[:BUCKET_BYTES]).hexdigest()
open("build/nvme_disk.img", "wb").write(disk)

kvm = os.access("/dev/kvm", os.R_OK | os.W_OK)
accel = ["-enable-kvm", "-cpu", "host"] if kvm else ["-cpu", "max"]
pm_flags = ["-overcommit", "cpu-pm=on"] if kvm else []

# Expected per-dispatch return sums for `(arg0, arg1) = (40, 2)`, cross-checked against an independent
# Python reference model of each capsule (x86 `crc32 r64`, BMI2 `pext`/`pdep`, `u64x8` lane math):
#   C=4 -> workers rid 1..3 ; C=1 -> BSP rid 0
EXPECT = {4: (2337, 1207754, 35277270), 1: (777, 381953, 978462)}

def run_deck(tag, qemu_args, keys, dump_after=None, boot_tmo=12.0):
    log_p, sock_p, ppm_p = f"build/{tag}.log", f"build/{tag}.sock", f"build/{tag}.ppm"
    for p in (log_p, sock_p, ppm_p):
        if os.path.exists(p):
            os.remove(p)
    proc = subprocess.Popen(["qemu-system-x86_64", *qemu_args,
                             "-debugcon", f"file:{log_p}", "-global", "isa-debugcon.iobase=0xe9",
                             "-monitor", f"unix:{sock_p},server,nowait", "-display", "none", "-no-reboot"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    rd = lambda: open(log_p, "rb").read().decode("latin1") if os.path.exists(log_p) else ""
    def wait_for(pred, tmo):
        t0 = time.time()
        while time.time() - t0 < tmo:
            if pred(rd()):
                return True
            time.sleep(0.03)
        return False
    marker = {"1": "CAP1=", "2": "CAP2=", "3": "CAP3=", "4": "ZFS=", "b": "BENCH=OK", "a": "GATE1B_OK", "q": "SHUTDOWN_OK"}
    try:
        if not wait_for(lambda l: "KBD\n" in l, boot_tmo):
            print(f"  [FAIL] {tag}: no ZEROHAND ... KBD banner:\n" + rd(), file=sys.stderr)
            sys.exit(1)
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.connect(sock_p)
        time.sleep(0.05)
        for k in keys:
            n0 = rd().count(marker[k])
            sock.sendall(f"sendkey {k}\n".encode())
            if not wait_for(lambda l: l.count(marker[k]) > n0, 6.0):
                print(f"  [FAIL] {tag}: no '{marker[k]}' response to sendkey {k}:\n" + rd(), file=sys.stderr)
                sys.exit(1)
            if dump_after == k:
                sock.sendall(f"screendump {ppm_p}\n".encode())
                wait_for(lambda l: os.path.exists(ppm_p) and os.path.getsize(ppm_p) > 0, 3.0)
                time.sleep(0.15)
        time.sleep(0.1)
        sock.sendall(b"quit\n")
        sock.close()
        proc.wait(timeout=4)
    finally:
        if proc.poll() is None:
            proc.kill()
    return rd(), ppm_p

# 1. 4-core Q35 + NVMe (`ZEXTENT1` + `ZLEDGER1`): `1` -> `2` -> `3` -> `4` (4Q PRP-List CoW TX -> EPOCH=2, KEYS=36, LVL=0!) -> `b` -> `a` -> screendump -> `q`
q35_nvme = ["-machine", "q35", *accel, *pm_flags, "-m", "512M",
            "-drive", f"if=pflash,format=raw,readonly=on,file={ovmf}",
            "-drive", "format=raw,file=fat:rw:build/esp",
            "-drive", "file=build/nvme_disk.img,if=none,id=nvm0,format=raw",
            "-device", "nvme,serial=ZEROZCAP,drive=nvm0"]
log, ppm = run_deck("gate1_debugcon", [*q35_nvme, "-smp", "4"], ["1", "2", "3", "4", "b", "a", "q"], dump_after="a")
c1, c2, c3 = EXPECT[4]
m = re.search(r"^ZEROHAND W=(\d+) H=(\d+) S=(\d+) F=0 C=4 M=(\d+) PT=(\d+) WX=0 ACPI=1 GS=1 FAB=1 SIMD=1 VEC=(\d+)/(\d+) CYC=(\d+) CAP=777 KBD\n"
              rf"CAP1={c1}\nCAP2={c2}\nCAP3={c3}\n"
              r"ZFS=OK EPOCH=2 LVL=0 Q=4 SNAP=4294967293 KEYS=36 VAL=200436\n"
              r"BENCH=OK\nSCAN=0x0000001E GATE1B_OK\nSHUTDOWN_OK\n$", log)
if not m:
    print("  [FAIL] unexpected 4-core Command Deck + ZeroFS Scale-Out debugcon log:\n" + log, file=sys.stderr)
    sys.exit(1)
gw, gh, gs, mc, pt, vec_cyc, sc_cyc, cyc = map(int, m.groups())
assert gs >= gw * 4, f"stride {gs} < width*4 ({gw*4})"
assert vec_cyc > 0 and sc_cyc > 0 and cyc > 0
print(f"  [PASS] [zero2] Q35+OVMF -smp 4 + NVMe (4 I/O queues): C=4 WX=0({pt} PT) SIMD=1 VEC={vec_cyc}/{sc_cyc} CYC={cyc} CAP=777")
print(f"  [PASS] [zero2] Command Deck + ZeroFS 4Q PRP-List CoW TX: [1..3] CAP={c1}/{c2}/{c3} [4] ZFS=OK EPOCH=2 LVL=0 Q=4 SNAP=0xFFFFFFFD KEYS=36 VAL=200436 [B] BENCH=OK [A] GATE1B_OK [Q] SHUTDOWN_OK")

with open(ppm, "rb") as f:
    assert f.readline().strip() == b"P6"
    pw, ph = map(int, f.readline().split())
    assert f.readline().strip() == b"255"
    rgb = f.read()
assert (pw, ph) == (gw, gh), f"PPM {pw}x{ph} != GOP {gw}x{gh}"

def get_px(x, y):
    o = (y * pw + x) * 3
    return rgb[o], rgb[o + 1], rgb[o + 2]

def exp_px(x, y):
    return (x ^ y) & 0xFF, (y * 3 + (x >> 2)) & 0xFF, x & 0xFF

for pt_xy in [(0, 0), (1, 0), (0, 1), (15, 0), (16, 0), (17, 5), (pw // 2, ph // 2), (pw - 1, 0), (0, ph - 1), (pw - 1, ph - 1)]:
    assert get_px(*pt_xy) == exp_px(*pt_xy), f"GOP AVX-512 scanout pixel mismatch at {pt_xy}"
assert get_px(40, 40) == ((0x1E ^ 0xA5) & 0xFF, 0xFF, 0x1E), "scancode cell mismatch at (40,40)"
for slot, want_rgb in [(0, (0x00, 0xE5, 0xFF)), (1, (0x29, 0x79, 0xFF)), (2, (0xFF, 0xD6, 0x00)),
                       (3, (0xFF, 0x6D, 0x00)), (4, (0x00, 0xE6, 0x76)), (5, (0xE0, 0x66, 0xFF))]:
    got_rgb = get_px(36 + slot * 24, 12)
    assert got_rgb == want_rgb, f"checkpoint LED {slot} mismatch: got {got_rgb}, want {want_rgb}"
for core_i in range(4):
    assert get_px(204 + core_i * 16, 12) == (0x00, 0xFF, 0xAA), f"online core pip {core_i} missing"
assert get_px(204 + 4 * 16, 12) == exp_px(204 + 4 * 16, 12), "unexpected 5th core pip on -smp 4"
assert get_px(420, 6)  == (0x00, 0xE5, 0xFF), f"HUD cyan border mismatch: {get_px(420, 6)}"
assert get_px(425, 10) == (0x0C, 0x10, 0x18), f"HUD obsidian fill mismatch: {get_px(425, 10)}"
assert get_px(434, 12) == (0xF0, 0xF4, 0xFF), f"HUD glyph 'Z' mismatch: {get_px(434, 12)}"
# Obsidian Command Deck (x=32..608, y=84..332) + Twin ZeroFS ZLEDGER1 / PCI Domains Panel (x=624..1200, y=84..332)
assert get_px(32, 84)   == (0x00, 0xE5, 0xFF), f"deck cyan border mismatch: {get_px(32, 84)}"
assert get_px(560, 100) == (0x12, 0x1A, 0x2A), f"deck header bar mismatch: {get_px(560, 100)}"
assert get_px(36, 120)  == (0x0C, 0x10, 0x18), f"deck obsidian body mismatch: {get_px(36, 120)}"
assert get_px(560, 180) == (0x0C, 0x10, 0x18) and get_px(560, 200) == (0x0C, 0x10, 0x18), "slots [1]/[2] wrongly highlighted"
assert get_px(560, 225) == (0x16, 0x26, 0x3D), f"active slot [3] highlight missing: {get_px(560, 225)}"
assert get_px(44, 218)  == (0xFF, 0xD6, 0x00), f"active slot '>' marker missing: {get_px(44, 218)}"
if pw >= 1220 and ph >= 680:
    assert get_px(624, 84)  == (0x00, 0xE5, 0xFF), f"ZeroFS panel cyan border mismatch: {get_px(624, 84)}"
    assert get_px(630, 120) == (0x0C, 0x10, 0x18), f"ZeroFS panel obsidian body mismatch: {get_px(630, 120)}"
try:
    from PIL import Image
    Image.open(ppm).save("build/gate4_live_os_hud.png")
    png_note = " -> build/gate4_live_os_hud.png"
except Exception:
    png_note = ""
print(f"  [PASS] [zero2] GOP {pw}x{ph} stride={gs}B: AVX-512 scanout, 6 LEDs, 4 core pips, HUD, Command Deck + ZeroFS ZLEDGER1 Panel{png_note}")

# 2. Cold-Reboot NVMe Persistence + Live Multi-Level (`LVL=0 -> LVL=1`) Merkle B+Tree Split (`36 -> 72` keys!)
c1, c2, c3 = EXPECT[1]
log1n, _ = run_deck("gate1_smp1_nvme", [*q35_nvme, "-smp", "1"], ["1", "2", "3", "4", "q"])
assert re.search(r"^ZEROHAND W=\d+ H=\d+ S=\d+ F=0 C=1 M=\d+ PT=\d+ WX=0 ACPI=1 GS=1 FAB=1 SIMD=1 VEC=\d+/\d+ CYC=\d+ CAP=777 KBD\n"
                 rf"CAP1={c1}\nCAP2={c2}\nCAP3={c3}\n"
                 r"ZFS=OK EPOCH=3 LVL=1 Q=1 SNAP=4294967292 KEYS=72 VAL=300172\n"
                 r"SHUTDOWN_OK\n$", log1n), f"unexpected -smp 1 cold-reboot ZeroFS NVMe split log: {log1n}"
print(f"  [PASS] [zero2] cold-reboot NVMe B+Tree split verified: mounted EPOCH=2 (36 keys) -> committed EPOCH=3 LVL=1 Q=1 SNAP=0xFFFFFFFC KEYS=72 VAL=300172")

# 2b. Independent Host Structural Audit of On-Disk `build/nvme_disk.img` (`EPOCH=3`, `LVL=1` Merkle B+Tree, `ZGENTBL1`, `ZBPFOOT1`, untouched `Bucket 0`)
ZGEN_MAGIC = 0x314C42544E45475A
ZBPF_MAGIC = 0x31544F4F4650425A
ADDR42_MASK = 0x000003FFFFFFFFFF
post_disk = open("build/nvme_disk.img", "rb").read()
assert hashlib.sha256(post_disk[:BUCKET_BYTES]).hexdigest() == bucket0_sha_before, "CRITICAL: Bucket 0 (ZEXTENT1) modified during ZeroFS CoW commits!"

sb_a_raw = post_disk[1 * BUCKET_BYTES : 1 * BUCKET_BYTES + 64]
sb_magic, sb_ver, sb_flags, sb_ep, sb_rext, sb_snap, sb_bcnt, sb_amask, sb_gpack, sb_kcnt, sb_crc_got = struct.unpack("<QIIQQIIQQII", sb_a_raw)
sb_crc_exp = crc32c_seq_u64([*struct.unpack("<7Q", sb_a_raw[:56]), sb_kcnt])
assert sb_magic == ZLEDGER_MAGIC and sb_ep == 3 and sb_flags == 3 and sb_kcnt == 72 and sb_crc_got == sb_crc_exp, \
    f"Superblock A audit failed: ep={sb_ep} flags={sb_flags:#x} keys={sb_kcnt} crc={sb_crc_got:#x}/{sb_crc_exp:#x}"

def verify_eytz_page(off):
    buf = post_disk[off : off + 4096]
    mg, ep, mn, mx, lvl, tkeys, gext, cnt, ncrc = struct.unpack_from("<7QII", buf, 0)
    assert mg == ZEYTZ_MAGIC and cnt <= 63, f"bad EytzNodeHdr at {off:#x}"
    words = [*struct.unpack_from("<7Q", buf, 0), cnt]
    for s in range(1, cnt + 1):
        words.extend(struct.unpack_from("<8Q", buf, s * 64))
    assert crc32c_seq_u64(words) == ncrc, f"EytzNodeHdr CRC32C mismatch at {off:#x}"
    return buf, ep, lvl, tkeys, gext, cnt, ncrc

root_off3 = (sb_rext & ADDR42_MASK) << 6
rbuf, rep, rlvl, rtkeys, rgext, rcnt, _ = verify_eytz_page(root_off3)
assert rep == 3 and rlvl == 1 and rtkeys == 72 and rcnt == 2 and rgext != 0, \
    f"unexpected root node: ep={rep} lvl={rlvl} total={rtkeys} cnt={rcnt} gext={rgext:#x}"

# Verify `ZfsGenHdr` (`ZGENTBL1`) 4 KiB page
gen_off3 = (rgext & ADDR42_MASK) << 6
gbuf = post_disk[gen_off3 : gen_off3 + 4096]
g_magic, g_ep, g_bcnt = struct.unpack_from("<3Q", gbuf, 0)
g_rsv, g_crc = struct.unpack_from("<II", gbuf, 56)
g_words = [*struct.unpack_from("<7Q", gbuf, 0), g_rsv, *struct.unpack_from("<504Q", gbuf, 64)]
assert g_magic == ZGEN_MAGIC and g_ep == 3 and g_bcnt == 8 and crc32c_seq_u64(g_words) == g_crc, "ZfsGenHdr CRC/header mismatch"

# Verify Merkle `EytzPivot` slots 1..2 -> child leaves (`63 + 9 == 72` keys)
leaf_key_sum = 0
for s in range(1, rcnt + 1):
    p_raw = rbuf[s * 64 : (s + 1) * 64]
    p_max_id, p_max_sn, p_ccnt, p_clvl, p_cext, p_min_id, p_min_sn, p_pad0, p_ccrc, p_fl, p_rsv, p_crc = struct.unpack("<QIHH4Q4I", p_raw)
    assert crc32c_seq_u64([*struct.unpack("<7Q", p_raw[:56]), p_rsv]) == p_crc, f"EytzPivot {s} CRC mismatch"
    _, lep, llvl, ltkeys, _, lcnt, lncrc = verify_eytz_page((p_cext & ADDR42_MASK) << 6)
    assert llvl == 0 and lncrc == p_ccrc and lcnt == p_ccnt, f"Merkle child_crc mismatch on pivot {s}"
    leaf_key_sum += lcnt
assert leaf_key_sum == 72, f"leaf key sum {leaf_key_sum} != 72"

# Verify `+0x1FF000` `ZfsBucketFooterHdr` (`ZBPFOOT1`) pages across active CoW buckets
bp_total = 0
footer_buckets = 0
for bid in range(3, 8):
    if (sb_amask >> bid) & 1:
        fbuf = post_disk[bid * BUCKET_BYTES + 0x1FF000 : bid * BUCKET_BYTES + 0x200000]
        f_mg, f_ep, f_bid, f_gen, f_used = struct.unpack_from("<QQIIQ", fbuf, 0)
        f_bpc, f_crc = struct.unpack_from("<II", fbuf, 56)
        if f_mg == ZBPF_MAGIC:
            f_words = [*struct.unpack_from("<7Q", fbuf, 0), f_bpc, *struct.unpack_from(f"<{f_bpc * 4}Q", fbuf, 64)]
            assert f_bid == bid and f_gen == 1 and crc32c_seq_u64(f_words) == f_crc, f"Bucket {bid} footer CRC mismatch"
            bp_total += f_bpc
            footer_buckets += 1
assert footer_buckets >= 1 and bp_total == 72, f"expected 72 persistent Backpointer32 records across active footers, got {bp_total}"
print(f"  [PASS] [zero2] host NVMe image audit: Bucket 0 untouched, LVL=1 Merkle B+Tree (2 leaves, 63+9=72 keys), ZGENTBL1 & {footer_buckets} ZBPFOOT1 footers ({bp_total} backpointers) CRC32C-valid")

# 3. Adversarial Foreign NVMe Drive Protection Test (`GPT` + `LUKS` + `ext4`, NO `ZLEDGER1` -> `ZFS=SAFE_RO`, SHA-256 100% unchanged!)
foreign_disk = bytearray(8 * 1024 * 1024)
foreign_disk[510:512] = b"\x55\xAA"
foreign_disk[1 * BUCKET_BYTES : 1 * BUCKET_BYTES + 6] = b"LUKS\xba\xbe"
foreign_disk[2 * BUCKET_BYTES + 0x438 : 2 * BUCKET_BYTES + 0x43A] = b"\x53\xEF" # ext4 s_magic
open("build/nvme_foreign.img", "wb").write(foreign_disk)
sha_before = hashlib.sha256(foreign_disk).hexdigest()
q35_foreign = ["-machine", "q35", *accel, *pm_flags, "-smp", "4", "-m", "512M",
               "-drive", f"if=pflash,format=raw,readonly=on,file={ovmf}",
               "-drive", "format=raw,file=fat:rw:build/esp",
               "-drive", "file=build/nvme_foreign.img,if=none,id=nvmf,format=raw",
               "-device", "nvme,serial=FOREIGN0,drive=nvmf"]
log_fg, _ = run_deck("gate1_foreign_nvme", q35_foreign, ["4", "q"])
sha_after = hashlib.sha256(open("build/nvme_foreign.img", "rb").read()).hexdigest()
assert sha_before == sha_after, f"CRITICAL: foreign NVMe image modified! {sha_before} != {sha_after}"
assert re.search(r"^ZEROHAND W=\d+ H=\d+ S=\d+ F=0 C=4 M=\d+ PT=\d+ WX=0 ACPI=1 GS=1 FAB=1 SIMD=1 VEC=\d+/\d+ CYC=\d+ CAP=0 KBD\n"
                 r"ZFS=SAFE_RO\nSHUTDOWN_OK\n$", log_fg), f"unexpected foreign-drive log: {log_fg}"
print(f"  [PASS] [zero2] foreign GPT/LUKS/ext4 NVMe drive guard: ZFS=SAFE_RO, 0 sectors written (SHA-256={sha_after[:16]}... identical)")

# 4. Single-core i440FX without NVMe: degrades cleanly (`C=1`, `CAP=0`, `ZFS=SAFE_RO`, capsule keys return `0`)
log1, _ = run_deck("gate1_smp1", [*accel, "-smp", "1", "-m", "512M",
                                   "-drive", f"if=pflash,format=raw,readonly=on,file={ovmf}",
                                   "-drive", "format=raw,file=fat:rw:build/esp"], ["2", "4", "a", "q"])
assert re.search(r"^ZEROHAND W=\d+ H=\d+ S=\d+ F=0 C=1 M=\d+ PT=\d+ WX=0 ACPI=1 GS=1 FAB=1 SIMD=1 VEC=\d+/\d+ CYC=\d+ CAP=0 KBD\n"
                 r"CAP2=0\nZFS=SAFE_RO\nSCAN=0x0000001E GATE1B_OK\nSHUTDOWN_OK\n$", log1), f"unexpected -smp 1 no-NVMe log: {log1}"
print("  [PASS] [zero2] single-core no-NVMe (-smp 1) fallback: C=1 WX=0 SIMD=1 CAP=0, CAP2=0, ZFS=SAFE_RO, GATE1B_OK, SHUTDOWN_OK")

# 4. Missing GOP (`-vga none`) returns cleanly to firmware without guessing a framebuffer (§8.5)
if os.path.exists("build/gate1_nogop.log"):
    os.remove("build/gate1_nogop.log")
proc2 = subprocess.Popen([
    "qemu-system-x86_64", *accel, "-m", "512M",
    "-drive", f"if=pflash,format=raw,readonly=on,file={ovmf}",
    "-drive", "format=raw,file=fat:rw:build/esp",
    "-vga", "none",
    "-debugcon", "file:build/gate1_nogop.log",
    "-global", "isa-debugcon.iobase=0xe9",
    "-display", "none",
    "-no-reboot",
], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(2.2)
proc2.kill()
proc2.wait()
nogop_bytes = os.path.getsize("build/gate1_nogop.log") if os.path.exists("build/gate1_nogop.log") else 0
assert nogop_bytes == 0, f"missing GOP unexpectedly wrote {nogop_bytes} bytes to 0xE9!"
print("  [PASS] [zero2] missing GOP (-vga none) returned cleanly to UEFI firmware (0 framebuffer guesses)")
PY

echo "=== [7/7] Physical-Laptop USB Image: GPT + FAT32 ESP + qemu_debug=0 BOOTX64.EFI (make usb-img) ==="
python3 scripts/build_usb_image.py --compiler "$ZERO2" --src examples/gate1_os.zr --build-dir "$BUILD" --out "$BUILD/zeroos_usb.img" > /dev/null
python3 - <<'PY'
import os, socket, struct, subprocess, sys, time, zlib

IMG = "build/zeroos_usb.img"
img = open(IMG, "rb").read()
SEC = 512
assert len(img) == 64 * 1024 * 1024, f"USB image is {len(img)} bytes, want 64 MiB"
total = len(img) // SEC

# 1. Protective MBR (type 0xEE) + primary & backup GPT headers / entry arrays with valid CRC32s
assert img[510:512] == b"\x55\xAA" and img[446 + 4] == 0xEE, "missing protective MBR (0xEE)"
def check_gpt(lba, want_alt):
    hdr = img[lba * SEC:(lba + 1) * SEC]
    assert hdr[0:8] == b"EFI PART", f"no GPT signature at LBA {lba}"
    hsz = struct.unpack_from("<I", hdr, 12)[0]
    hcrc = struct.unpack_from("<I", hdr, 16)[0]
    tmp = bytearray(hdr[:hsz]); tmp[16:20] = b"\0\0\0\0"
    assert zlib.crc32(tmp) & 0xFFFFFFFF == hcrc, f"GPT header CRC mismatch at LBA {lba}"
    my, alt, first, last = struct.unpack_from("<QQQQ", hdr, 24)
    ent_lba, n_ent, ent_sz, ent_crc = struct.unpack_from("<QIII", hdr, 72)
    assert my == lba and alt == want_alt, f"GPT LBA links wrong at {lba}: my={my} alt={alt}"
    arr = img[ent_lba * SEC: ent_lba * SEC + n_ent * ent_sz]
    assert zlib.crc32(arr) & 0xFFFFFFFF == ent_crc, f"GPT entry array CRC mismatch (LBA {lba})"
    return arr, first, last
arr, first, last = check_gpt(1, total - 1)
arr_b, _, _ = check_gpt(total - 1, 1)
assert arr == arr_b, "primary/backup GPT entry arrays differ"
esp_guid = bytes.fromhex("28732ac11ff8d211ba4b00a0c93ec93b")
assert arr[0:16] == esp_guid, "partition 1 is not an EFI System Partition"
p_first, p_last = struct.unpack_from("<QQ", arr, 32)
assert first <= p_first <= p_last <= last, "ESP outside GPT usable range"

# 2. Independent FAT32 walk: `/EFI/BOOT/BOOTX64.EFI`
vol = img[p_first * SEC:(p_last + 1) * SEC]
bps, spc, rsvd, nfats = struct.unpack_from("<HBHB", vol, 11)
fatsz, root_clus = struct.unpack_from("<I", vol, 36)[0], struct.unpack_from("<I", vol, 44)[0]
assert vol[82:90] == b"FAT32   " and vol[510:512] == b"\x55\xAA" and bps == 512, "ESP is not a FAT32 volume"
data_off = (rsvd + nfats * fatsz) * bps
clus_bytes = spc * bps
n_clusters = (len(vol) - data_off) // clus_bytes
assert n_clusters >= 65525, f"only {n_clusters} clusters: not a spec-compliant FAT32 volume"
fat = vol[rsvd * bps:(rsvd + fatsz) * bps]
assert fat == vol[(rsvd + fatsz) * bps:(rsvd + 2 * fatsz) * bps], "FAT1 != FAT2"
def chain(c):
    out = []
    while 2 <= c < 0x0FFFFFF8:
        out.append(c)
        c = struct.unpack_from("<I", fat, c * 4)[0] & 0x0FFFFFFF
        assert len(out) < 1 << 20
    return out
def read_clusters(c):
    return b"".join(vol[data_off + (k - 2) * clus_bytes: data_off + (k - 1) * clus_bytes] for k in chain(c))
def find(dir_clus, name83):
    d = read_clusters(dir_clus)
    for o in range(0, len(d), 32):
        if d[o] == 0:
            break
        if d[o:o + 11] == name83 and d[o + 11] != 0x0F:
            clus = (struct.unpack_from("<H", d, o + 20)[0] << 16) | struct.unpack_from("<H", d, o + 26)[0]
            return clus, struct.unpack_from("<I", d, o + 28)[0], d[o + 11]
    raise AssertionError(f"{name83!r} not found in FAT32 directory")
efi_c, _, a1 = find(root_clus, b"EFI        ")
boot_c, _, a2 = find(efi_c, b"BOOT       ")
f_c, f_sz, _ = find(boot_c, b"BOOTX64 EFI")
assert a1 & 0x10 and a2 & 0x10, "EFI/BOOT are not directories"
payload = read_clusters(f_c)[:f_sz]
laptop = open("build/gate1_laptop_z2.efi", "rb").read()
assert payload == laptop, "USB /EFI/BOOT/BOOTX64.EFI != independently compiled zero2 qemu_debug=0 gate1_laptop_z2.efi"
open("build/usb_extracted_BOOTX64.EFI", "wb").write(payload)
print(f"  [PASS] build/zeroos_usb.img: protective MBR + CRC-valid primary/backup GPT + FAT32 ESP ({n_clusters} clusters) "
      f"-> /EFI/BOOT/BOOTX64.EFI ({f_sz} B) byte-identical to zero2 laptop build")

# 3. Boot the raw USB image on OVMF via xHCI `usb-storage` (as a physical stick) and verify the Command Deck + i8042 input
ovmf = "/usr/share/edk2/x64/OVMF_CODE.4m.fd"
kvm = os.access("/dev/kvm", os.R_OK | os.W_OK)
accel = ["-enable-kvm", "-cpu", "host"] if kvm else ["-cpu", "max"]
sock_p, ppm_p, dbg_p = "build/usb_boot.sock", "build/usb_boot.ppm", "build/usb_boot_debugcon.log"
for p in (sock_p, ppm_p, dbg_p):
    if os.path.exists(p):
        os.remove(p)
proc = subprocess.Popen(["qemu-system-x86_64", "-machine", "q35", *accel, "-smp", "4", "-m", "512M",
                         "-drive", f"if=pflash,format=raw,readonly=on,file={ovmf}",
                         "-drive", f"if=none,id=usb0,format=raw,file={IMG},snapshot=on",
                         "-device", "qemu-xhci", "-device", "usb-storage,drive=usb0,bootindex=0",
                         "-debugcon", f"file:{dbg_p}", "-global", "isa-debugcon.iobase=0xe9",
                         "-monitor", f"unix:{sock_p},server,nowait", "-display", "none", "-no-reboot"],
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
def mon(cmd):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(sock_p)
    s.sendall((cmd + "\n").encode())
    time.sleep(0.25)
    s.close()
def dump():
    if os.path.exists(ppm_p):
        os.remove(ppm_p)
    mon(f"screendump {ppm_p}")
    for _ in range(20):
        if os.path.exists(ppm_p) and os.path.getsize(ppm_p) > 0:
            break
        time.sleep(0.05)
    with open(ppm_p, "rb") as f:
        assert f.readline().strip() == b"P6"
        w, h = map(int, f.readline().split())
        f.readline()
        data = f.read()
    return w, h, (lambda x, y: tuple(data[(y * w + x) * 3:(y * w + x) * 3 + 3]))
exp_px = lambda x, y: ((x ^ y) & 0xFF, (y * 3 + (x >> 2)) & 0xFF, x & 0xFF)
try:
    t0 = time.time()
    while not os.path.exists(sock_p) and time.time() - t0 < 5:
        time.sleep(0.05)
    booted = False
    while time.time() - t0 < 15.0:
        time.sleep(0.4)
        try:
            w, h, px = dump()
        except Exception:
            continue
        if w >= 640 and px(32, 84) == (0x00, 0xE5, 0xFF) and px(0, 0) == exp_px(0, 0) and px(w - 1, h - 1) == exp_px(w - 1, h - 1):
            booted = True
            break
    if not booted:
        print("  [FAIL] USB image did not reach the ZeroOS Command Deck under OVMF xHCI usb-storage boot", file=sys.stderr)
        sys.exit(1)
    t_boot = time.time() - t0
    for core_i in range(4):
        assert px(204 + core_i * 16, 12) == (0x00, 0xFF, 0xAA), f"USB boot: core pip {core_i} missing"
    mon("sendkey a")
    time.sleep(0.4)
    w, h, px = dump()
    assert px(40, 40) == ((0x1E ^ 0xA5) & 0xFF, 0xFF, 0x1E), f"USB boot: scancode cell mismatch {px(40, 40)}"
    assert px(36 + 4 * 24, 12) == (0x00, 0xE6, 0x76), "USB boot: keyboard LED 4 not lit"
    mon("quit")
    proc.wait(timeout=4)
finally:
    if proc.poll() is None:
        proc.kill()
dbg_bytes = os.path.getsize(dbg_p) if os.path.exists(dbg_p) else 0
assert dbg_bytes == 0, f"qemu_debug=0 USB image wrote {dbg_bytes} bytes to port 0xE9!"
print(f"  [PASS] USB image booted via OVMF xHCI usb-storage in {t_boot:.1f}s -> {w}x{h} Command Deck, 4 core pips, i8042 'a' (0x1E) cell, 0 bytes on 0xE9")
PY

echo "=== Binary Footprint Summary ==="
ls -lh --block-size=1 "$ZERO0" "$BUILD/zero1" "$ZERO2" "$BUILD/zero3" "$BUILD/z2_first" "$BUILD/z2_phase2_zero_power" "$BUILD/z2_phase3_reactor_fabric" "$BUILD/z2_phase4_avx512_simd" "$BUILD/z2_phase5_nextgen_spine" "$BUILD/z2_phase6_zerofs_ledger" "$BUILD/capsule_z2.zcap" "$BUILD/cap_avx512_wave.zcap" "$BUILD/cap_matrix_crc.zcap" "$BUILD/cap_spine_probe.zcap" "$BUILD/gate1_os_z2.efi" "$BUILD/gate1_laptop_z2.efi" "$BUILD/zeroos_usb.img"
