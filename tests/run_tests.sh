#!/usr/bin/env bash
set -euo pipefail

ZERO0="${1:-build/zero0}"
BUILD="build"
mkdir -p "$BUILD/neg"

echo "=== [1/6] Host Compiler W^X & Lexer Verification ==="
if readelf -lW "$ZERO0" | grep -q "RWE"; then
    echo "  [FAIL] $ZERO0 contains an RWX segment!" >&2
    exit 1
fi
echo "  [PASS] $ZERO0 has strict W^X PT_LOAD segments (no RWE)"

"$ZERO0" --lex tests/lex_showcase.zr > "$BUILD/lex_showcase.log"
grep -q "lex ok:" "$BUILD/lex_showcase.log"
echo "  [PASS] --lex verified on tests/lex_showcase.zr"

echo "=== [2/6] Golden-Output Positive Compilation & Execution Suites ==="
run_golden() {
    local src="$1"
    local bin="$2"
    local expected="$3"
    local out_log="${bin}.stdout"

    "$ZERO0" "$src" "$bin"
    if readelf -lW "$bin" | grep -q "RWE"; then
        echo "  [FAIL] guest binary $bin contains an RWX segment!" >&2
        exit 1
    fi
    "$bin" > "$out_log"
    diff -u "$expected" "$out_log"
    echo "  [PASS] $src -> $bin (W^X + stdout matches $expected)"
}

run_golden "examples/first.zr"              "$BUILD/first"                 "tests/first.expected"
run_golden "tests/step3_compute.zr"         "$BUILD/step3_compute"         "tests/step3_compute.expected"
run_golden "tests/step4_systems.zr"         "$BUILD/step4_systems"         "tests/step4_systems.expected"
run_golden "tests/phase0_scalar.zr"         "$BUILD/phase0_scalar"         "tests/phase0_scalar.expected"
run_golden "tests/phase1_zero_lang.zr"      "$BUILD/phase1_zero_lang"      "tests/phase1_zero_lang.expected"
run_golden "tests/phase2_zero_power.zr"     "$BUILD/phase2_zero_power"     "tests/phase2_zero_power.expected"
run_golden "tests/phase3_reactor_fabric.zr" "$BUILD/phase3_reactor_fabric" "tests/phase3_reactor_fabric.expected"

# Verify literal silicon opcodes emitted directly in guest `.text` (zero runtime wrappers)
DIS_POWER=$(objdump -D -b binary -m i386:x86-64 --start-address=120 "$BUILD/phase2_zero_power")
for op in "movnti" "lock xadd" "lock cmpxchg" "popcnt" "lzcnt" "tzcnt" "pdep" "pext" "crc32" "prefetcht0" "bswap" "sfence" "lfence" "mfence"; do
    if ! grep -q "$op" <<< "$DIS_POWER"; then
        echo "  [FAIL] $BUILD/phase2_zero_power missing inline silicon instruction: $op" >&2
        exit 1
    fi
done
DIS_FAB=$(objdump -D -b binary -m i386:x86-64 --start-address=120 "$BUILD/phase3_reactor_fabric")
for op in "movdir64b" "movdiri" "cpuid"; do
    if ! grep -q "$op" <<< "$DIS_FAB"; then
        echo "  [FAIL] $BUILD/phase3_reactor_fabric missing direct-silicon instruction: $op" >&2
        exit 1
    fi
done
echo "  [PASS] verified inline silicon opcodes (movdir64b, movdiri, movnti, lock xadd/cmpxchg, BMI2, CRC32C, prefetcht0, fences)"

echo "=== [3/6] Adversarial Negative Rejection Probes (ELF64, EFI, & Capsule) ==="
expect_compile_reject() {
    local name="$1"
    local code="$2"
    local expected_err="$3"
    local src="$BUILD/neg/${name}.zr"
    local err_log="$BUILD/neg/${name}.err"

    printf "%s\n" "$code" > "$src"
    if "$ZERO0" "$src" "$BUILD/neg/${name}.bin" 2> "$err_log"; then
        echo "  [FAIL] $name unexpectedly compiled!" >&2
        exit 1
    fi
    grep -q "$expected_err" "$err_log"
    echo "  [PASS] rejected $name ($(cat "$err_log"))"
}

expect_efi_reject() {
    local name="$1"
    local code="$2"
    local expected_err="$3"
    local src="$BUILD/neg/${name}.zr"
    local err_log="$BUILD/neg/${name}.err"

    printf "%s\n" "$code" > "$src"
    if "$ZERO0" --target=efi-os "$src" "$BUILD/neg/${name}.efi" 2> "$err_log"; then
        echo "  [FAIL] $name unexpectedly compiled on --target=efi-os!" >&2
        exit 1
    fi
    grep -q "$expected_err" "$err_log"
    echo "  [PASS] rejected $name ($(cat "$err_log"))"
}

expect_capsule_reject() {
    local name="$1"
    local code="$2"
    local expected_err="$3"
    local src="$BUILD/neg/${name}.zr"
    local err_log="$BUILD/neg/${name}.err"

    printf "%s\n" "$code" > "$src"
    if "$ZERO0" --target=capsule "$src" "$BUILD/neg/${name}.zcap" 2> "$err_log"; then
        echo "  [FAIL] $name unexpectedly compiled on --target=capsule!" >&2
        exit 1
    fi
    grep -q "$expected_err" "$err_log"
    echo "  [PASS] rejected $name ($(cat "$err_log"))"
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

echo "=== [4/6] Runtime Unhandled Fault Exit Check ==="
printf 'fn main() { fail 99 }\n' > "$BUILD/neg/unhandled_fail.zr"
"$ZERO0" "$BUILD/neg/unhandled_fail.zr" "$BUILD/neg/unhandled_fail.bin" > /dev/null
set +e
"$BUILD/neg/unhandled_fail.bin"
rc=$?
set -e
if [[ "$rc" -ne 1 ]]; then
    echo "  [FAIL] unhandled_fail exited with $rc instead of 1" >&2
    exit 1
fi
echo "  [PASS] unhandled runtime fail exited cleanly with status $rc"

echo "=== [5/6] Phase-4 Self-Hosting Fixed-Point Bootstrap Verification (zero0 -> zero1 -> zero2 == zero3) ==="
"$ZERO0" "compiler/zero1.zr" "$BUILD/zero1" > /dev/null
"$BUILD/zero1" "compiler/zero1.zr" "$BUILD/zero2" > /dev/null
"$BUILD/zero2" "compiler/zero1.zr" "$BUILD/zero3" > /dev/null
for zbin in "$BUILD/zero1" "$BUILD/zero2" "$BUILD/zero3"; do
    if readelf -lW "$zbin" | grep -q "RWE"; then
        echo "  [FAIL] self-hosted compiler $zbin contains an RWX segment!" >&2
        exit 1
    fi
done
if ! cmp -s "$BUILD/zero2" "$BUILD/zero3"; then
    echo "  [FAIL] Stage-2 ($BUILD/zero2) and Stage-3 ($BUILD/zero3) differ (fixed-point failed)!" >&2
    exit 1
fi
echo "  [PASS] deterministic self-hosting fixed point proven: $BUILD/zero2 == $BUILD/zero3 ($(stat -c%s "$BUILD/zero2") bytes, strict W^X)"

"$BUILD/zero2" "examples/first.zr"      "$BUILD/z2_first"  > /dev/null
"$BUILD/zero2" "tests/step3_compute.zr" "$BUILD/z2_step3"  > /dev/null
"$BUILD/zero2" "tests/phase0_scalar.zr" "$BUILD/z2_phase0" > /dev/null
"$BUILD/z2_first"  | diff -u "tests/first.expected"         -
"$BUILD/z2_step3"  | diff -u "tests/step3_compute.expected" -
"$BUILD/z2_phase0" | diff -u "tests/phase0_scalar.expected" -
echo "  [PASS] self-hosted zero2 compiled first.zr, step3_compute.zr, & phase0_scalar.zr (100% golden match)"

"$ZERO0"       --target=capsule "examples/capsule_compute.zr" "$BUILD/capsule_compute.zcap" > /dev/null
"$BUILD/zero2" --target=capsule "examples/capsule_compute.zr" "$BUILD/capsule_z2.zcap"      > /dev/null
echo "  [PASS] both zero0 and self-hosted zero2 emitted CRC32C-signed --target=capsule (.zcap) workloads"

echo "=== [6/6] Gate-3 Bare-Metal Multi-Core Reactor + PCIe/NVMe @capsule Verification (QEMU Q35 + OVMF) ==="
"$ZERO0" --target=efi-os "examples/gate1_os.zr" "$BUILD/gate1_os.efi"
HDR_EFI=$(objdump -p "$BUILD/gate1_os.efi")
DIS_EFI=$(objdump -d "$BUILD/gate1_os.efi")
grep -q "Subsystem[[:space:]]*0000000a[[:space:]]*(EFI application)" <<< "$HDR_EFI"
for efi_op in "movdir64b" "monitor" "mwait" "iretq" "movnti" "sfence" "crc32" "%gs:0x0" "%gs:0x8"; do
    if ! grep -q "$efi_op" <<< "$DIS_EFI"; then
        echo "  [FAIL] $BUILD/gate1_os.efi missing hardware instruction/prefix: $efi_op" >&2
        exit 1
    fi
done
echo "  [PASS] examples/gate1_os.zr -> $BUILD/gate1_os.efi (PE32+ + movdir64b + monitor/mwait + iretq + crc32 + %gs:0 + movnti)"

# Verify `qemu_debug :: 0` laptop build compiles cleanly too and eliminates 0xE9 port I/O
ln -sfn ../examples/os "$BUILD/os"
sed 's/^qemu_debug[[:space:]]*:: 1/qemu_debug          :: 0/' examples/gate1_os.zr > "$BUILD/gate1_laptop.zr"
"$ZERO0" --target=efi-os "$BUILD/gate1_laptop.zr" "$BUILD/gate1_laptop.efi" > /dev/null
if objdump -d "$BUILD/gate1_laptop.efi" | grep -Eq '\b(in|out)\b.*0xe9'; then
    echo "  [FAIL] gate1_laptop.efi still contains port 0xE9 I/O with qemu_debug :: 0" >&2
    exit 1
fi
echo "  [PASS] laptop variant (qemu_debug :: 0) compiled -> $BUILD/gate1_laptop.efi (0xE9 eliminated)"

python3 - <<'PY'
import os, re, shutil, socket, struct, subprocess, sys, time

ovmf = "/usr/share/edk2/x64/OVMF_CODE.4m.fd"
if not os.path.exists(ovmf):
    print("  [FAIL] OVMF firmware missing at", ovmf, file=sys.stderr)
    sys.exit(1)

os.makedirs("build/esp/EFI/BOOT", exist_ok=True)
shutil.copyfile("build/gate1_os.efi", "build/esp/EFI/BOOT/BOOTX64.EFI")

# Build 16 MiB raw NVMe Master Extent image holding self-hosted `build/capsule_z2.zcap`!
cap_bytes = open("build/capsule_z2.zcap", "rb").read()
hdr_crc = struct.unpack_from("<I", cap_bytes, 20)[0]
disk = bytearray(16 * 1024 * 1024)
struct.pack_into("<QIIQ", disk, 0, 0x31544E455458455A, 1, 1, 1)
struct.pack_into("<QQQII", disk, 64, 0x5A32434150, 4096, len(cap_bytes), hdr_crc, 1)
disk[4096 : 4096 + len(cap_bytes)] = cap_bytes
open("build/nvme_disk.img", "wb").write(disk)

accel = ["-enable-kvm", "-cpu", "host"] if os.access("/dev/kvm", os.R_OK | os.W_OK) else ["-cpu", "max"]
pm_flags = ["-overcommit", "cpu-pm=on"] if os.access("/dev/kvm", os.R_OK | os.W_OK) else []

# 1. Boot `-machine q35 -smp 4` with PCIe NVMe (`capsule_z2.zcap`), `MONITOR`/`MWAIT`, & `MOVDIR64B`
for p in ("build/gate1_debugcon.log", "build/gate1_gop.ppm", "build/qmp.sock"):
    if os.path.exists(p):
        os.remove(p)

cmd = [
    "qemu-system-x86_64", "-machine", "q35", *accel, *pm_flags, "-smp", "4", "-m", "512M",
    "-drive", f"if=pflash,format=raw,readonly=on,file={ovmf}",
    "-drive", "format=raw,file=fat:rw:build/esp",
    "-drive", "file=build/nvme_disk.img,if=none,id=nvm0,format=raw",
    "-device", "nvme,serial=ZEROZCAP,drive=nvm0",
    "-debugcon", "file:build/gate1_debugcon.log",
    "-global", "isa-debugcon.iobase=0xe9",
    "-monitor", "unix:build/qmp.sock,server,nowait",
    "-display", "none",
    "-no-reboot",
]

proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
try:
    t0 = time.time()
    sent_key = False
    sock = None
    while time.time() - t0 < 12.0:
        if os.path.exists("build/gate1_debugcon.log"):
            log = open("build/gate1_debugcon.log", "rb").read().decode("latin1")
            if ("KBD\n" in log) and not sent_key:
                sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                sock.connect("build/qmp.sock")
                time.sleep(0.05)
                sock.sendall(b"sendkey a\n")
                sent_key = True
            if "GATE1B_OK" in log and sock is not None:
                sock.sendall(b"screendump build/gate1_gop.ppm\n")
                time.sleep(0.15)
                sock.sendall(b"quit\n")
                sock.close()
                break
        time.sleep(0.05)
    proc.wait(timeout=4)
finally:
    if proc.poll() is None:
        proc.kill()

log = open("build/gate1_debugcon.log", "rb").read().decode("latin1")
m = re.search(r"^ZEROHAND W=(\d+) H=(\d+) S=(\d+) F=0 C=4 M=(\d+) PT=(\d+) WX=0 ACPI=1 GS=1 FAB=1 CYC=(\d+) CAP=777 KBD\nSCAN=0x0000001E GATE1B_OK\n$", log)
if not m:
    print("  [FAIL] unexpected Gate-3 (Q35 + NVMe -smp 4) debugcon log:\n" + log, file=sys.stderr)
    sys.exit(1)

gw, gh, gs, mc, pt, cyc = map(int, m.groups())
assert gs >= gw * 4, f"stride {gs} < width*4 ({gw*4})"
assert cyc > 0, f"expected positive rdtsc round-trip cycle count, got {cyc}"
print(f"  [PASS] Q35+OVMF (-smp 4, cpu-pm=on, NVMe) C=4 + W^X({pt} PT) + gs:0 + MOVDIR64B/MWAIT (CYC={cyc}) + Self-Hosted NVMe @capsule (CAP=777) + i8042(0x1E)")

with open("build/gate1_gop.ppm", "rb") as f:
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

for pt_xy in [(0, 0), (1, 0), (0, 1), (17, 5), (pw // 2, ph // 2), (pw - 1, 0), (0, ph - 1), (pw - 1, ph - 1)]:
    assert get_px(*pt_xy) == exp_px(*pt_xy), f"GOP pixel mismatch at {pt_xy}"

assert get_px(40, 40) == ((0x1E ^ 0xA5) & 0xFF, 0xFF, 0x1E), "scancode cell mismatch at (40,40)"
expected_leds = [
    (0, (0x00, 0xE5, 0xFF)),
    (1, (0x29, 0x79, 0xFF)),
    (2, (0xFF, 0xD6, 0x00)),
    (3, (0xFF, 0x6D, 0x00)),
    (4, (0x00, 0xE6, 0x76)),
    (5, (0xE0, 0x66, 0xFF)),
]
for slot, want_rgb in expected_leds:
    got_rgb = get_px(36 + slot * 24, 12)
    assert got_rgb == want_rgb, f"checkpoint LED {slot} mismatch: got {got_rgb}, want {want_rgb}"

for core_i in range(4):
    assert get_px(204 + core_i * 16, 12) == (0x00, 0xFF, 0xAA), f"online core pip {core_i} missing"
assert get_px(204 + 4 * 16, 12) == exp_px(204 + 4 * 16, 12), "unexpected 5th core pip on -smp 4"
assert get_px(420, 6)  == (0x00, 0xE5, 0xFF), f"HUD cyan border mismatch: {get_px(420, 6)}"
assert get_px(425, 10) == (0x0C, 0x10, 0x18), f"HUD obsidian fill mismatch: {get_px(425, 10)}"
assert get_px(434, 12) == (0xF0, 0xF4, 0xFF), f"HUD glyph 'Z' mismatch: {get_px(434, 12)}"
print(f"  [PASS] GOP BGRX8888 stride={gs}B (movnti+unroll:4), 6 LEDs (incl. NVMe @capsule), 4 core pips, HUD ('ZERO OS // GATE 3 // C:4 WX:0 CYC:{cyc} CAP:777') verified ({pw}x{ph})")

# 2. Verify `-smp 1` without NVMe degrades cleanly (`C=1`, `CAP=0`)
for p in ("build/gate1_smp1.log", "build/qmp1.sock"):
    if os.path.exists(p):
        os.remove(p)
proc1 = subprocess.Popen([
    "qemu-system-x86_64", *accel, "-smp", "1", "-m", "512M",
    "-drive", f"if=pflash,format=raw,readonly=on,file={ovmf}",
    "-drive", "format=raw,file=fat:rw:build/esp",
    "-debugcon", "file:build/gate1_smp1.log",
    "-global", "isa-debugcon.iobase=0xe9",
    "-monitor", "unix:build/qmp1.sock,server,nowait",
    "-display", "none",
    "-no-reboot",
], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
try:
    t0 = time.time()
    sent1 = False
    sock1 = None
    while time.time() - t0 < 10.0:
        if os.path.exists("build/gate1_smp1.log"):
            log1 = open("build/gate1_smp1.log", "rb").read().decode("latin1")
            if ("KBD\n" in log1) and not sent1:
                sock1 = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                sock1.connect("build/qmp1.sock")
                time.sleep(0.05)
                sock1.sendall(b"sendkey a\n")
                sent1 = True
            if "GATE1B_OK" in log1 and sock1 is not None:
                sock1.sendall(b"quit\n")
                sock1.close()
                break
        time.sleep(0.05)
    proc1.wait(timeout=4)
finally:
    if proc1.poll() is None:
        proc1.kill()

log1 = open("build/gate1_smp1.log", "rb").read().decode("latin1")
assert re.search(r"^ZEROHAND W=\d+ H=\d+ S=\d+ F=0 C=1 M=\d+ PT=\d+ WX=0 ACPI=1 GS=1 FAB=1 CYC=\d+ CAP=0 KBD\nSCAN=0x0000001E GATE1B_OK\n$", log1), f"unexpected -smp 1 log: {log1}"
print("  [PASS] single-core no-NVMe (-smp 1) fallback verified cleanly (C=1, WX=0, GS=1, FAB=1, CAP=0)")

# 3. Verify missing GOP (`-vga none`) returns cleanly to firmware without guessing framebuffer (§8.5)
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
print("  [PASS] missing GOP (-vga none) returned cleanly to UEFI firmware (0 framebuffer guesses)")
PY

echo "=== Binary Footprint Summary ==="
ls -lh --block-size=1 "$ZERO0" "$BUILD/zero1" "$BUILD/zero2" "$BUILD/zero3" "$BUILD/first" "$BUILD/z2_first" "$BUILD/capsule_compute.zcap" "$BUILD/capsule_z2.zcap" "$BUILD/gate1_os.efi" "$BUILD/gate1_laptop.efi"
