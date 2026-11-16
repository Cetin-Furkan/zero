; =============================================================================
; Zero Language — Stage-0 Pure Assembly Bootstrap (`bootstrap/seed0.asm`)
; =============================================================================
; Phase 0 Scalar-Frozen Compiler:
;   - Both `zero0` itself and all guest binaries are 2-segment strict W^X ELF64
;     executables (zero RWX segments anywhere).
;   - 512 MiB Disjoint Virtual Compiler Arena (8 x 64 MiB spaces, 0 malloc).
;   - Implements full Phase-0 scalar rules from `PEER_MASTER_PLAN.md` §5.
; =============================================================================

BITS 64
DEFAULT REL
ORG 0x400000

%define SYS_READ    0
%define SYS_WRITE   1
%define SYS_CLOSE   3
%define SYS_LSEEK   8
%define SYS_MMAP    9
%define SYS_EXIT    60
%define SYS_FCHMOD  91
%define SYS_OPENAT  257

%define AT_FDCWD    -100
%define O_RDONLY    0
%define O_WR_CREAT  0x241
%define SEEK_SET    0
%define SEEK_END    2
%define PROT_RW     3
%define MAP_ANON_PV 0x22

%define SPACE_SIZE      0x04000000  ; 64 MiB per compiler space
%define ARENA_TOTAL     0x20000000  ; 512 MiB total virtual reservation
%define HOST_BSS_BASE   0x00800000  ; Disjoint RW `.bss` for `zero0` itself

%include "bootstrap/tokens.inc"

; =============================================================================
; 1. Host Strict W^X Two-Segment ELF64 Header (176 bytes)
; =============================================================================
ehdr:
    db 0x7F, "ELF"
    db 2, 1, 1, 0
    dq 0
    dw 2                            ; ET_EXEC
    dw 62                           ; EM_X86_64
    dd 1
    dq _start
    dq phdr_rx - $$                 ; e_phoff = 64
    dq 0
    dd 0
    dw 64                           ; e_ehsize
    dw 56                           ; e_phentsize
    dw 2                            ; e_phnum = 2 (strict W^X on zero0 too!)
    dw 0, 0, 0

phdr_rx:
    dd 1                            ; PT_LOAD
    dd 5                            ; PF_R | PF_X (no W!)
    dq 0
    dq $$                           ; 0x00400000
    dq $$
    dq file_end - $$
    dq file_end - $$
    dq 0x1000

phdr_rw:
    dd 1                            ; PT_LOAD
    dd 6                            ; PF_R | PF_W (no X!)
    dq 0
    dq HOST_BSS_BASE                ; 0x00800000
    dq HOST_BSS_BASE
    dq 0                            ; 0 bytes on disk
    dq bss_end - bss_start
    dq 0x1000

; =============================================================================
; 2. Entry Point (`_start`)
; =============================================================================
_start:
    mov     rax, [rsp]
    cmp     rax, 4
    je      .check_efi_flag
    cmp     rax, 3
    jne     err_usage

    mov     rdi, [rsp + 16]
    mov     rsi, [rsp + 24]
    mov     byte [mode_efi], 0

    cmp     dword [rdi], 0x656C2D2D ; "--le"
    jne     .normal_mode
    cmp     word [rdi + 4], 0x0078  ; "x\0"
    jne     .normal_mode

    mov     byte [mode_lex], 1
    mov     [in_path], rsi
    jmp     .init_arena

.normal_mode:
    cmp     word [rdi], 0x2D2D      ; reject unknown "--*" flags!
    je      err_usage
    mov     byte [mode_lex], 0
    mov     [in_path], rdi
    mov     [out_path], rsi
    jmp     .init_arena

.check_efi_flag:
    mov     rdi, [rsp + 16]         ; argv[1]
    mov     rax, [str_flag_efi]
    cmp     [rdi], rax
    jne     err_usage
    mov     rax, [str_flag_efi + 8]
    cmp     [rdi + 8], rax
    jne     err_usage
    mov     byte [mode_lex], 0
    mov     byte [mode_efi], 1
    mov     rax, [rsp + 24]
    mov     [in_path], rax
    mov     rax, [rsp + 32]
    mov     [out_path], rax

.init_arena:
    mov     eax, SYS_MMAP
    xor     edi, edi
    mov     esi, ARENA_TOTAL
    mov     edx, PROT_RW
    mov     r10d, MAP_ANON_PV
    mov     r8, -1
    xor     r9d, r9d
    syscall
    test    rax, rax
    js      err_mmap

    mov     [space_src],     rax
    lea     rcx, [rax + SPACE_SIZE * 1]
    mov     [space_tok],     rcx
    lea     rcx, [rax + SPACE_SIZE * 2]
    mov     [space_sym],     rcx
    lea     rcx, [rax + SPACE_SIZE * 3]
    mov     [space_rodata],  rcx
    lea     rcx, [rax + SPACE_SIZE * 4]
    mov     [space_tgt_ro],  rcx
    lea     rcx, [rax + SPACE_SIZE * 5]
    mov     [space_fixup],   rcx
    lea     rcx, [rax + SPACE_SIZE * 6]
    mov     [space_ast],     rcx
    lea     rcx, [rax + SPACE_SIZE * 7]
    mov     [space_out],     rcx

    ; --- Read Input `.zr` File ---
    mov     eax, SYS_OPENAT
    mov     edi, AT_FDCWD
    mov     rsi, [in_path]
    xor     edx, edx
    xor     r10d, r10d
    syscall
    test    rax, rax
    js      err_open_in
    mov     [in_fd], rax

    mov     rdi, rax
    xor     esi, esi
    mov     edx, SEEK_END
    mov     eax, SYS_LSEEK
    syscall
    test    rax, rax
    js      err_open_in
    mov     [src_len], rax

    mov     rdi, [in_fd]
    xor     esi, esi
    xor     edx, edx
    mov     eax, SYS_LSEEK
    syscall

    mov     rdi, [in_fd]
    mov     rsi, [space_src]
    mov     rdx, [src_len]
    mov     eax, SYS_READ
    syscall
    cmp     rax, [src_len]
    jne     err_open_in

    mov     rdi, [in_fd]
    mov     eax, SYS_CLOSE
    syscall

    ; =========================================================================
    ; 3. Initialize Lexer & Pre-Intern Contextual Identifiers
    ; =========================================================================
    call    lexer_init

    lea     rdi, [str_main]
    mov     esi, 4
    call    sym_intern
    mov     [sym_main_id], eax

    lea     rdi, [str_max]
    mov     esi, 3
    call    sym_intern
    mov     [sym_max_id], eax

    lea     rdi, [str_until]
    mov     esi, 5
    call    sym_intern
    mov     [sym_until_id], eax

    lea     rdi, [str_bits]
    mov     esi, 4
    call    sym_intern
    mov     [sym_bits_id], eax

    lea     rdi, [str_align]
    mov     esi, 5
    call    sym_intern
    mov     [sym_align_id], eax

    lea     rdi, [str_cap]
    mov     esi, 3
    call    sym_intern
    mov     [sym_cap_id], eax

    lea     rdi, [str_base]
    mov     esi, 4
    call    sym_intern
    mov     [sym_base_id], eax

    lea     rdi, [str_rsp_align]
    mov     esi, 9
    call    sym_intern
    mov     [sym_rsp_align_id], eax

    ; --- Pre-Intern Gate-1a Ring-0 Intrinsic Identifiers (§8.4) ---
    lea     rdi, [str_cli]
    mov     esi, 3
    call    sym_intern
    mov     [sym_cli_id], eax

    lea     rdi, [str_sti]
    mov     esi, 3
    call    sym_intern
    mov     [sym_sti_id], eax

    lea     rdi, [str_hlt]
    mov     esi, 3
    call    sym_intern
    mov     [sym_hlt_id], eax

    lea     rdi, [str_lgdt]
    mov     esi, 4
    call    sym_intern
    mov     [sym_lgdt_id], eax

    lea     rdi, [str_lidt]
    mov     esi, 4
    call    sym_intern
    mov     [sym_lidt_id], eax

    lea     rdi, [str_ltr]
    mov     esi, 3
    call    sym_intern
    mov     [sym_ltr_id], eax

    lea     rdi, [str_cr_read]
    mov     esi, 7
    call    sym_intern
    mov     [sym_cr_read_id], eax

    lea     rdi, [str_cr_write]
    mov     esi, 8
    call    sym_intern
    mov     [sym_cr_write_id], eax

    lea     rdi, [str_msr_read]
    mov     esi, 8
    call    sym_intern
    mov     [sym_msr_read_id], eax

    lea     rdi, [str_msr_write]
    mov     esi, 9
    call    sym_intern
    mov     [sym_msr_write_id], eax

    lea     rdi, [str_xsetbv]
    mov     esi, 6
    call    sym_intern
    mov     [sym_xsetbv_id], eax

    lea     rdi, [str_port_in8]
    mov     esi, 8
    call    sym_intern
    mov     [sym_port_in8_id], eax

    lea     rdi, [str_port_out8]
    mov     esi, 9
    call    sym_intern
    mov     [sym_port_out8_id], eax

    lea     rdi, [str_mm_read32]
    mov     esi, 9
    call    sym_intern
    mov     [sym_mm_read32_id], eax

    lea     rdi, [str_mm_read64]
    mov     esi, 9
    call    sym_intern
    mov     [sym_mm_read64_id], eax

    lea     rdi, [str_mm_write32]
    mov     esi, 10
    call    sym_intern
    mov     [sym_mm_write32_id], eax

    lea     rdi, [str_mm_write64]
    mov     esi, 10
    call    sym_intern
    mov     [sym_mm_write64_id], eax

    lea     rdi, [str_isr_base]
    mov     esi, 13
    call    sym_intern
    mov     [sym_isr_base_id], eax

    ; --- Pre-Intern Phase-1 Typed View, Port, Packed & Ring-0 Symbols ---
    lea     rdi, [str_port]
    mov     esi, 4
    call    sym_intern
    mov     [sym_port_id], eax

    lea     rdi, [str_view]
    mov     esi, 4
    call    sym_intern
    mov     [sym_view_id], eax

    lea     rdi, [str_packed]
    mov     esi, 6
    call    sym_intern
    mov     [sym_packed_id], eax

    lea     rdi, [str_sizeof]
    mov     esi, 6
    call    sym_intern
    mov     [sym_sizeof_id], eax

    lea     rdi, [str_alignof]
    mov     esi, 7
    call    sym_intern
    mov     [sym_alignof_id], eax

    lea     rdi, [str_read]
    mov     esi, 4
    call    sym_intern
    mov     [sym_read_id], eax

    lea     rdi, [str_write]
    mov     esi, 5
    call    sym_intern
    mov     [sym_write_id], eax

    lea     rdi, [str_len]
    mov     esi, 3
    call    sym_intern
    mov     [sym_len_id], eax

    lea     rdi, [str_halt]
    mov     esi, 4
    call    sym_intern
    mov     [sym_halt_id], eax

    lea     rdi, [str_wrpkru]
    mov     esi, 6
    call    sym_intern
    mov     [sym_wrpkru_id], eax

    call    lexer_tokenize

    cmp     byte [mode_lex], 1
    jne     .compile_all

    call    lexer_dump
    xor     edi, edi
    mov     eax, SYS_EXIT
    syscall

    ; =========================================================================
    ; 4. Compile Module & Emit Two-Segment W^X ELF64 Guest Binary
    ; =========================================================================
.compile_all:
    call    compile_module

    cmp     byte [mode_efi], 1
    jne     .finalize_elf
    call    finalize_efi_image
    mov     r15, [space_out]
    jmp     .write_output_file

.finalize_elf:
    mov     r15, [space_out]
    mov     r8d, [code_off]
    mov     rsi, [space_tgt_ro]
    lea     rdi, [r15 + r8]
    mov     ecx, [tgt_ro_off]
    rep     movsb
    mov     eax, r8d
    add     eax, [tgt_ro_off]
    mov     [total_out], eax

    mov     r13, [space_fixup]
    xor     ecx, ecx
    mov     edx, [fixup_cnt]
.patch_ro_loop:
    cmp     ecx, edx
    jae     .write_elf
    mov     eax, [r13 + rcx * 8]
    mov     esi, [r13 + rcx * 8 + 4]
    lea     esi, [rsi + r8 + TARGET_BASE]
    mov     [r15 + rax], esi
    inc     ecx
    jmp     .patch_ro_loop

.write_elf:
    lea     rsi, [ehdr]
    mov     rdi, r15
    mov     ecx, 64
    rep     movsb

    mov     qword [r15 + 24], TARGET_BASE + TARGET_HDR_LEN  ; e_entry = 0x4000B0
    mov     qword [r15 + 32], 64
    mov     word  [r15 + 56], 2

    ; --- Guest Phdr[0]: RX at 0x00400000 ---
    mov     dword [r15 + 64 + 0],  1
    mov     dword [r15 + 64 + 4],  5
    mov     qword [r15 + 64 + 8],  0
    mov     qword [r15 + 64 + 16], TARGET_BASE
    mov     qword [r15 + 64 + 24], TARGET_BASE
    mov     eax, [total_out]
    mov     [r15 + 64 + 32], rax
    mov     [r15 + 64 + 40], rax
    mov     qword [r15 + 64 + 48], 0x1000

    ; --- Guest Phdr[1]: RW at 0x10000000 ---
    mov     dword [r15 + 120 + 0],  1
    mov     dword [r15 + 120 + 4],  6
    mov     qword [r15 + 120 + 8],  0
    mov     qword [r15 + 120 + 16], DATA_VADDR_BASE
    mov     qword [r15 + 120 + 24], DATA_VADDR_BASE
    mov     qword [r15 + 120 + 32], 0
    mov     rax, [data_bss_off]
    mov     [r15 + 120 + 40], rax
    mov     qword [r15 + 120 + 48], 0x1000

.write_output_file:
    mov     eax, SYS_OPENAT
    mov     edi, AT_FDCWD
    mov     rsi, [out_path]
    mov     edx, O_WR_CREAT
    mov     r10d, 0o755
    syscall
    test    rax, rax
    js      err_open_out
    mov     [out_fd], rax

    mov     rdi, rax
    mov     rsi, r15
    mov     edx, [total_out]
    mov     eax, SYS_WRITE
    syscall

    mov     rdi, [out_fd]
    mov     esi, 0o755
    mov     eax, SYS_FCHMOD
    syscall

    mov     rdi, [out_fd]
    mov     eax, SYS_CLOSE
    syscall

    cmp     byte [mode_efi], 1
    je      .print_efi_ok

    mov     eax, SYS_WRITE
    mov     edi, 1
    lea     rsi, [msg_ok]
    mov     edx, msg_ok_len
    syscall
    jmp     .exit_zero

.print_efi_ok:
    mov     eax, SYS_WRITE
    mov     edi, 1
    lea     rsi, [msg_ok_efi]
    mov     edx, msg_ok_efi_len
    syscall

.exit_zero:
    xor     edi, edi
    mov     eax, SYS_EXIT
    syscall

; =============================================================================
; Token Cursor & Pinpoint Error Diagnostics
; =============================================================================
peek_tok_ptr:
    mov     eax, [tok_idx]
    shl     rax, 4
    add     rax, [space_tok]
    ret

next_tok_ptr:
    mov     eax, [tok_idx]
    inc     dword [tok_idx]
    shl     rax, 4
    add     rax, [space_tok]
    ret

expect_tok_kind:
    movzx   edx, al
    call    next_tok_ptr
    cmp     byte [rax], dl
    jne     parse_err_at_tok
    ret

expect_tok_kw:
    movzx   edx, ax
    call    next_tok_ptr
    cmp     word [rax], dx
    jne     parse_err_at_tok
    ret

parse_err_at_tok:
    mov     esi, [rax + 4]
    lea     rdi, [msg_syn_line]
    mov     edx, msg_syn_line_len

err_msg_at_line:
    push    rsi
    mov     rsi, rdi
    mov     edi, 2
    mov     eax, SYS_WRITE
    syscall
    pop     rsi
    mov     edi, 2
    call    write_u64_fd
    mov     edi, 2
    lea     rsi, [nl_byte]
    mov     edx, 1
    mov     eax, SYS_WRITE
    syscall
    mov     edi, 1
    mov     eax, SYS_EXIT
    syscall

; =============================================================================
; Include Subsystems
; =============================================================================
%include "bootstrap/lexer.inc"
%include "bootstrap/codegen.inc"
%include "bootstrap/efi_boot.inc"
%include "bootstrap/efi_writer.inc"
%include "bootstrap/types.inc"
%include "bootstrap/intrinsics.inc"
%include "bootstrap/views.inc"
%include "bootstrap/parser.inc"

; =============================================================================
; Fatal Error Handlers & Messages
; =============================================================================
err_usage:
    lea     rsi, [msg_usage]
    mov     edx, msg_usage_len
    jmp     die_stderr
err_mmap:
    lea     rsi, [msg_mmap]
    mov     edx, msg_mmap_len
    jmp     die_stderr
err_open_in:
    lea     rsi, [msg_in]
    mov     edx, msg_in_len
    jmp     die_stderr
err_open_out:
    lea     rsi, [msg_out]
    mov     edx, msg_out_len
    jmp     die_stderr
err_undef_fn:
    lea     rsi, [msg_undef_fn]
    mov     edx, msg_undef_fn_len

die_stderr:
    mov     eax, SYS_WRITE
    mov     edi, 2
    syscall
    mov     edi, 1
    mov     eax, SYS_EXIT
    syscall

str_main:               db "main"
str_max:                db "max"
str_until:              db "until"
str_bits:               db "bits"
str_align:              db "align"
str_cap:                db "cap"
str_base:               db "base"
str_rsp_align:          db "rsp_align"
str_flag_efi:           db "--target=efi-os", 0
str_cli:                db "cli"
str_sti:                db "sti"
str_hlt:                db "hlt"
str_lgdt:               db "lgdt"
str_lidt:               db "lidt"
str_ltr:                db "ltr"
str_cr_read:            db "cr_read"
str_cr_write:           db "cr_write"
str_msr_read:           db "msr_read"
str_msr_write:          db "msr_write"
str_xsetbv:             db "xsetbv"
str_port_in8:           db "port_in8"
str_port_out8:          db "port_out8"
str_mm_read32:          db "mm_read32"
str_mm_read64:          db "mm_read64"
str_mm_write32:         db "mm_write32"
str_mm_write64:         db "mm_write64"
str_isr_base:           db "isr_stub_base"
str_port:               db "port"
str_view:               db "view"
str_packed:             db "packed"
str_sizeof:             db "sizeof"
str_alignof:            db "alignof"
str_read:               db "read"
str_write:              db "write"
str_len:                db "len"
str_halt:               db "halt"
str_wrpkru:             db "wrpkru"

msg_ok:                 db "[zero0] phase-0 compile ok: emitted 2-segment W^X ELF64 binary", 10
msg_ok_len              equ $ - msg_ok

msg_ok_efi:             db "[zero0] gate-1a compile ok: emitted standalone PE32+ UEFI OS image", 10
msg_ok_efi_len          equ $ - msg_ok_efi

msg_usage:              db "usage: zero0 <in.zr> <out_elf> | zero0 --target=efi-os <in.zr> <out.efi> | zero0 --lex <in.zr>", 10
msg_usage_len           equ $ - msg_usage

msg_mmap:               db "[zero0] fatal: virtual arena mmap failed", 10
msg_mmap_len            equ $ - msg_mmap

msg_in:                 db "[zero0] fatal: cannot read input .zr file", 10
msg_in_len              equ $ - msg_in

msg_out:                db "[zero0] fatal: cannot write output ELF64 binary", 10
msg_out_len             equ $ - msg_out

msg_undef_fn:           db "[zero0] link fault: called undefined function", 10
msg_undef_fn_len        equ $ - msg_undef_fn

msg_syn_line:           db "[zero0] parse fault at line "
msg_syn_line_len        equ $ - msg_syn_line

msg_dup_sym:            db "[zero0] duplicate top-level symbol at line "
msg_dup_sym_len         equ $ - msg_dup_sym

msg_dup_local:          db "[zero0] duplicate variable in same block at line "
msg_dup_local_len       equ $ - msg_dup_local

msg_bad_const:          db "[zero0] unresolved or cyclic constant at line "
msg_bad_const_len       equ $ - msg_bad_const

msg_bad_bp_width:       db "[zero0] bit-layout width must sum to 8, 16, 32, or 64 at line "
msg_bad_bp_width_len    equ $ - msg_bad_bp_width

msg_unimpl_space:       db "[zero0] unimplemented '@' space in stage-0 at line "
msg_unimpl_space_len    equ $ - msg_unimpl_space

msg_unimpl_cold:        db "[zero0] unimplemented '@cold' split in stage-0 at line "
msg_unimpl_cold_len     equ $ - msg_unimpl_cold

msg_unimpl_lens:        db "[zero0] unimplemented or invalid loop lens at line "
msg_unimpl_lens_len     equ $ - msg_unimpl_lens

msg_need_efi:           db "[zero0] ring-0 intrinsic requires --target=efi-os at line "
msg_need_efi_len        equ $ - msg_need_efi

msg_bad_intrinsic_arg:  db "[zero0] invalid or unaligned ring-0 intrinsic constant at line "
msg_bad_intrinsic_arg_len equ $ - msg_bad_intrinsic_arg

msg_no_host_in_efi:     db "[zero0] host print/@syscall unavailable in --target=efi-os at line "
msg_no_host_in_efi_len  equ $ - msg_no_host_in_efi

msg_bad_port:           db "[zero0] invalid @port width or port number at line "
msg_bad_port_len        equ $ - msg_bad_port

align 8, db 0
file_end:

; =============================================================================
; Host Zero-Initialized `.bss` (Mapped RW at 0x00800000 via Phdr[1]!)
; =============================================================================
SECTION .bss nobits start=HOST_BSS_BASE align=8
bss_start:
space_src:              resq 1
space_tok:              resq 1
space_sym:              resq 1
space_rodata:           resq 1
space_tgt_ro:           resq 1
space_fixup:            resq 1
space_ast:              resq 1
space_out:              resq 1
data_bss_off:           resq 1

in_path:                resq 1
out_path:               resq 1
in_fd:                  resq 1
out_fd:                 resq 1
src_len:                resq 1

sym_count:              resd 1
sym_main_id:            resd 1
sym_max_id:             resd 1
sym_until_id:           resd 1
sym_bits_id:            resd 1
sym_align_id:           resd 1
sym_cap_id:             resd 1
sym_base_id:            resd 1
sym_rsp_align_id:       resd 1
sym_cli_id:             resd 1
sym_sti_id:             resd 1
sym_hlt_id:             resd 1
sym_lgdt_id:            resd 1
sym_lidt_id:            resd 1
sym_ltr_id:             resd 1
sym_cr_read_id:         resd 1
sym_cr_write_id:        resd 1
sym_msr_read_id:        resd 1
sym_msr_write_id:       resd 1
sym_xsetbv_id:          resd 1
sym_port_in8_id:        resd 1
sym_port_out8_id:       resd 1
sym_mm_read32_id:       resd 1
sym_mm_read64_id:       resd 1
sym_mm_write32_id:      resd 1
sym_mm_write64_id:      resd 1
sym_isr_base_id:        resd 1
sym_port_id:            resd 1
sym_view_id:            resd 1
sym_packed_id:          resd 1
sym_sizeof_id:          resd 1
sym_alignof_id:         resd 1
sym_read_id:            resd 1
sym_write_id:           resd 1
sym_len_id:             resd 1
sym_halt_id:            resd 1
sym_wrpkru_id:          resd 1
efi_isr_stubs_off:      resd 1
efi_code_span_patch_off: resd 1

cur_line:               resd 1
tok_count:              resd 1
tok_idx:                resd 1
rodata_off:             resd 1
tgt_ro_off:             resd 1
code_off:               resd 1
fixup_cnt:              resd 1
total_out:              resd 1
rt_write_i64_off:       resd 1

const_cnt:              resd 1
type_cnt:               resd 1
field_cnt:              resd 1
space_cnt:              resd 1
fn_cnt:                 resd 1
call_fix_cnt:           resd 1
local_cnt:              resd 1
scope_depth:            resd 1
frame_qwords:           resd 1
temp_depth:             resd 1
max_frame_qwords:       resd 1
loop_depth:             resd 1
rec_depth:              resd 1
last_expr_ty:           resd 1

mode_lex:               resb 1
mode_efi:               resb 1
const_trial_mode:       resb 1
const_unresolved_hit:   resb 1
in_fn_body:             resb 1
bss_end:
