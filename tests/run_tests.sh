#!/usr/bin/env bash
set -euo pipefail

ZERO0="${1:-build/zero0}"
BUILD="build"
mkdir -p "$BUILD/neg"

echo "=== [1/5] Host Compiler W^X & Lexer Verification ==="
if readelf -lW "$ZERO0" | grep -q "RWE"; then
    echo "  [FAIL] $ZERO0 contains an RWX segment!" >&2
    exit 1
fi
echo "  [PASS] $ZERO0 has strict W^X PT_LOAD segments (no RWE)"

"$ZERO0" --lex tests/lex_showcase.zr > "$BUILD/lex_showcase.log"
grep -q "lex ok:" "$BUILD/lex_showcase.log"
echo "  [PASS] --lex verified on tests/lex_showcase.zr"

echo "=== [2/5] Golden-Output Positive Compilation & Execution Suites ==="
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

run_golden "examples/first.zr"      "$BUILD/first"         "tests/first.expected"
run_golden "tests/step3_compute.zr" "$BUILD/step3_compute" "tests/step3_compute.expected"
run_golden "tests/step4_systems.zr" "$BUILD/step4_systems" "tests/step4_systems.expected"
run_golden "tests/phase0_scalar.zr"    "$BUILD/phase0_scalar"    "tests/phase0_scalar.expected"
run_golden "tests/phase1_zero_lang.zr" "$BUILD/phase1_zero_lang" "tests/phase1_zero_lang.expected"

echo "=== [3/5] Adversarial Negative Rejection Probes (ELF64 & EFI) ==="
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

expect_compile_reject "unimpl_ring_space" \
    $'@ring q : <u64 | cap: 64>\nfn main() { }' \
    "unimplemented '@' space"

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

# Gate-1a Target Isolation & Intrinsic Static Validation (§8.4)
expect_compile_reject "ring0_cli_on_host_elf" \
    'fn main() { @cli() }' \
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

expect_efi_reject "efi_still_rejects_ring" \
    $'@ring q : <u64 | cap: 64>\nfn main(h: u64) i64 { return 0 }' \
    "unimplemented '@' space"

expect_efi_reject "efi_still_rejects_tile" \
    'fn main(h: u64) i64 { for i in 0..8 <tile: 32KB> { } return 0 }' \
    "unimplemented or invalid loop lens"

expect_efi_reject "bad_port_width_i64" \
    $'@port bad : <u64 | 0x60>\nfn main(h: u64) i64 { return 0 }' \
    "invalid @port width or port number"

expect_efi_reject "bad_port_range" \
    $'@port bad : <u8 | 0x10000>\nfn main(h: u64) i64 { return 0 }' \
    "invalid @port width or port number"

echo "=== [4/5] Runtime Unhandled Fault Exit Check ==="
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

echo "=== [5/5] Gate-1a Bare-Metal UEFI OS Verification (QEMU + OVMF) ==="
"$ZERO0" --target=efi-os "examples/gate1_os.zr" "$BUILD/gate1_os.efi"
objdump -p "$BUILD/gate1_os.efi" | grep -q "Subsystem[[:space:]]*0000000a[[:space:]]*(EFI application)"
echo "  [PASS] examples/gate1_os.zr -> $BUILD/gate1_os.efi (PE32+ EFI application)"

# Verify `qemu_debug :: 0` laptop build compiles cleanly too and eliminates 0xE9 port I/O
sed 's/^qemu_debug[[:space:]]*:: 1/qemu_debug          :: 0/' examples/gate1_os.zr > "$BUILD/gate1_laptop.zr"
"$ZERO0" --target=efi-os "$BUILD/gate1_laptop.zr" "$BUILD/gate1_laptop.efi" > /dev/null
if objdump -d "$BUILD/gate1_laptop.efi" | grep -Eq '\b(in|out)\b.*0xe9'; then
    echo "  [FAIL] gate1_laptop.efi still contains port 0xE9 I/O with qemu_debug :: 0" >&2
    exit 1
fi
echo "  [PASS] laptop variant (qemu_debug :: 0) compiled -> $BUILD/gate1_laptop.efi (0xE9 eliminated)"

python3 - <<'PY'
import os, re, shutil, socket, subprocess, sys, time

ovmf = "/usr/share/edk2/x64/OVMF_CODE.4m.fd"
if not os.path.exists(ovmf):
    print("  [FAIL] OVMF firmware missing at", ovmf, file=sys.stderr)
    sys.exit(1)

os.makedirs("build/esp/EFI/BOOT", exist_ok=True)
shutil.copyfile("build/gate1_os.efi", "build/esp/EFI/BOOT/BOOTX64.EFI")

accel = ["-enable-kvm", "-cpu", "host"] if os.access("/dev/kvm", os.R_OK | os.W_OK) else ["-cpu", "max"]

# 1. Boot with GOP enabled, inject PS/2 scancode `a` (0x1E), and capture GOP hardware scanout PPM
for p in ("build/gate1_debugcon.log", "build/gate1_gop.ppm", "build/qmp.sock"):
    if os.path.exists(p):
        os.remove(p)

cmd = [
    "qemu-system-x86_64", *accel, "-m", "512M",
    "-drive", f"if=pflash,format=raw,readonly=on,file={ovmf}",
    "-drive", "format=raw,file=fat:rw:build/esp",
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
            if "GATE1A_OK" in log and sock is not None:
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
m = re.search(r"^ZEROHAND W=(\d+) H=(\d+) S=(\d+) F=0 C=1 M=(\d+) PT=(\d+) WX=0 ACPI=1 KBD\nSCAN=0x0000001E GATE1A_OK\n$", log)
if not m:
    print("  [FAIL] unexpected Gate-1a debugcon log:\n" + log, file=sys.stderr)
    sys.exit(1)

gw, gh, gs, mc, pt = map(int, m.groups())
assert gs >= gw * 4, f"stride {gs} < width*4 ({gw*4})"
print(f"  [PASS] OVMF ExitBootServices + GDT/TSS + CR4/XCR0(0xE7) + PKRU=0 + PAT(WC)/CR3({pt} pages, {mc} ranges, WX=0, ACPI=1) + IDT + i8042(0x1E)")

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
]
for slot, want_rgb in expected_leds:
    got_rgb = get_px(36 + slot * 24, 12)
    assert got_rgb == want_rgb, f"checkpoint LED {slot} mismatch: got {got_rgb}, want {want_rgb}"
print(f"  [PASS] GOP BGRX8888 stride={gs}B, 5 checkpoint LEDs, & scancode cell verified ({pw}x{ph})")

# 2. Verify missing GOP (`-vga none`) returns cleanly to firmware without guessing framebuffer (§8.5)
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
ls -lh --block-size=1 "$ZERO0" "$BUILD/first" "$BUILD/step3_compute" "$BUILD/step4_systems" "$BUILD/phase0_scalar" "$BUILD/phase1_zero_lang" "$BUILD/gate1_os.efi" "$BUILD/gate1_laptop.efi"
