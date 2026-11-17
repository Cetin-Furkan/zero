; =============================================================================
; Zero Stage-0 Bootstrap — Gate-1a Ring-0 Intrinsics (`bootstrap/intrinsics.inc`)
; =============================================================================
; Strictly gated behind `--target=efi-os` (`[mode_efi] == 1`).
; Implements `PEER_MASTER_PLAN.md` §8.4 hardware intrinsics:
;   @cli, @sti, @hlt, @lgdt(base, limit), @lidt(base, limit), @ltr(sel),
;   @cr_read(n), @cr_write(n, v) [n in {0,3,4}], @msr_read(id), @msr_write(id, v),
;   @xsetbv(xcr, v), @port_in8(p), @port_out8(p, v),
;   plus physical memory accessors @mm_read32/64, @mm_write32/64 & @isr_stub_base.
; =============================================================================

; Called from `parse_at_intrinsic` when `rax` = identifier token after `@`
parse_efi_intrinsic:
    push    rbx
    push    r12
    push    r13

    mov     ebx, [rax + 8]          ; intrinsic sym_id
    mov     r12d, [rax + 4]         ; source line

    ; `@rsp_align()`, `@sizeof(T)`, `@alignof(T)`, `@view<T>(...)`, `@fn_addr(f)` work on BOTH targets!
    cmp     ebx, [sym_rsp_align_id]
    je      .do_rsp_align
    cmp     ebx, [sym_sizeof_id]
    je      .do_sizeof
    cmp     ebx, [sym_alignof_id]
    je      .do_alignof
    cmp     ebx, [sym_view_id]
    je      .do_view
    cmp     ebx, [sym_fn_addr_id]
    je      .do_fn_addr

    call    try_parse_ext_intrinsic
    test    eax, eax
    jnz     .done

    ; All Ring-0 hardware intrinsics require `--target=efi-os`!
    cmp     byte [mode_efi], 1
    jne     .err_need_efi

    cmp     ebx, [sym_cli_id]
    je      .do_cli
    cmp     ebx, [sym_sti_id]
    je      .do_sti
    cmp     ebx, [sym_hlt_id]
    je      .do_hlt
    cmp     ebx, [sym_halt_id]
    je      .do_halt
    cmp     ebx, [sym_pause_id]
    je      .do_pause
    cmp     ebx, [sym_rdtsc_id]
    je      .do_rdtsc
    cmp     ebx, [sym_lgdt_id]
    je      .do_lgdt
    cmp     ebx, [sym_lidt_id]
    je      .do_lidt
    cmp     ebx, [sym_ltr_id]
    je      .do_ltr
    cmp     ebx, [sym_cr_read_id]
    je      .do_cr_read
    cmp     ebx, [sym_cr_write_id]
    je      .do_cr_write
    cmp     ebx, [sym_msr_read_id]
    je      .do_msr_read
    cmp     ebx, [sym_msr_write_id]
    je      .do_msr_write
    cmp     ebx, [sym_xsetbv_id]
    je      .do_xsetbv
    cmp     ebx, [sym_wrpkru_id]
    je      .do_wrpkru
    cmp     ebx, [sym_port_in8_id]
    je      .do_port_in8
    cmp     ebx, [sym_port_out8_id]
    je      .do_port_out8
    cmp     ebx, [sym_mm_read32_id]
    je      .do_mm_read32
    cmp     ebx, [sym_mm_read64_id]
    je      .do_mm_read64
    cmp     ebx, [sym_mm_write32_id]
    je      .do_mm_write32
    cmp     ebx, [sym_mm_write64_id]
    je      .do_mm_write64
    cmp     ebx, [sym_isr_base_id]
    je      .do_isr_base

    mov     esi, r12d
    lea     rdi, [msg_syn_line]
    mov     edx, msg_syn_line_len
    jmp     err_msg_at_line

.err_need_efi:
    mov     esi, r12d
    lea     rdi, [msg_need_efi]
    mov     edx, msg_need_efi_len
    jmp     err_msg_at_line

.err_bad_cr:
    mov     esi, r12d
    lea     rdi, [msg_bad_intrinsic_arg]
    mov     edx, msg_bad_intrinsic_arg_len
    jmp     err_msg_at_line

.do_rsp_align:
    mov     al, TOK_LPAREN
    call    expect_tok_kind
    mov     al, TOK_RPAREN
    call    expect_tok_kind
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xE089              ; mov rax, rsp
    call    emit_u16
    mov     al, 0x83
    call    emit_u8
    mov     ax, 0x0FE0              ; and eax, 15
    call    emit_u16
    mov     dword [last_expr_ty], TY_U64
    jmp     .done

.do_sizeof:
    xor     edi, edi
    call    parse_runtime_sizeof_alignof
    jmp     .done

.do_alignof:
    mov     edi, 1
    call    parse_runtime_sizeof_alignof
    jmp     .done

.do_view:
    xor     edi, edi
    call    parse_view_or_mmio_call
    jmp     .done

.do_fn_addr:
    mov     al, TOK_LPAREN
    call    expect_tok_kind
    call    next_tok_ptr
    cmp     byte [rax], TOK_IDENT
    jne     parse_err_at_tok
    mov     edi, [rax + 8]          ; fn sym_id
    call    find_or_add_fn          ; eax = fn_idx
    mov     ebx, eax
    mov     al, TOK_RPAREN
    call    expect_tok_kind
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0x058D              ; 48 8D 05 <rel32> = lea rax, [rip + rel32]
    call    emit_u16
    mov     edx, [code_off]
    mov     rcx, [space_ast]
    add     rcx, AST_CALLFIX_OFF
    mov     eax, [call_fix_cnt]
    mov     [rcx + rax * 8],     edx
    mov     [rcx + rax * 8 + 4], ebx
    inc     dword [call_fix_cnt]
    xor     eax, eax
    call    emit_u32
    jmp     .done_u64

.do_pause:
    call    .expect_empty_parens
    mov     ax, 0x90F3              ; F3 90 = pause
    call    emit_u16
    jmp     .done_u64

.do_rdtsc:
    call    .expect_empty_parens
    mov     ax, 0x310F              ; 0F 31 = rdtsc
    call    emit_u16
    mov     eax, 0x20E2C148         ; 48 C1 E2 20 = shl rdx, 32
    call    emit_u32
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xD009              ; 48 09 D0 = or rax, rdx
    call    emit_u16
    jmp     .done_u64

.do_cli:
    call    .expect_empty_parens
    mov     al, 0xFA                ; cli
    call    emit_u8
    jmp     .done_u64

.do_sti:
    call    .expect_empty_parens
    mov     al, 0xFB                ; sti
    call    emit_u8
    jmp     .done_u64

.do_hlt:
    call    .expect_empty_parens
    mov     al, 0xF4                ; hlt
    call    emit_u8
    jmp     .done_u64

.do_halt:
    call    .expect_empty_parens
    mov     eax, 0xFDEBF4FA         ; cli; .h: hlt; jmp .h (non-returning!)
    call    emit_u32
    jmp     .done_u64

.do_wrpkru:
    call    .eval_one_arg_rax
    mov     eax, 0xD231C931         ; xor ecx, ecx; xor edx, edx
    call    emit_u32
    mov     al, 0x0F
    call    emit_u8
    mov     ax, 0xEF01              ; 0F 01 EF = wrpkru
    call    emit_u16
    mov     ax, 0xC031              ; xor eax, eax
    call    emit_u16
    jmp     .done_u64

.do_lgdt:
    mov     al, TOK_LPAREN
    call    expect_tok_kind
    ; Static check (§8.4): if `base` is a constant expression, reject `(base & 7) != 0`!
    call    .check_const_align8_before_comma
    call    .eval_two_args_rcx_rax  ; rcx = base, rax = limit
    lea     rsi, [seq_lgdt_reload]
    mov     ecx, seq_lgdt_reload_end - seq_lgdt_reload
    call    emit_bytes
    jmp     .done_u64

.do_lidt:
    mov     al, TOK_LPAREN
    call    expect_tok_kind
    call    .check_const_align8_before_comma
    call    .eval_two_args_rcx_rax  ; rcx = base, rax = limit
    lea     rsi, [seq_lidt_load]
    mov     ecx, seq_lidt_load_end - seq_lidt_load
    call    emit_bytes
    jmp     .done_u64

.do_ltr:
    call    .eval_one_arg_rax
    mov     al, 0x0F
    call    emit_u8
    mov     ax, 0xD800              ; 0F 00 D8 = ltr ax
    call    emit_u16
    jmp     .done_u64

.do_cr_read:
    mov     al, TOK_LPAREN
    call    expect_tok_kind
    mov     al, TOK_RPAREN
    call    .eval_const_arg_fenced  ; `rax` = constant `n`
    mov     rcx, rax                ; n must be 0, 3, or 4!
    mov     al, TOK_RPAREN
    call    expect_tok_kind
    cmp     rcx, 0
    mov     dl, 0xC0                ; cr0
    je      .emit_cr_rd
    cmp     rcx, 3
    mov     dl, 0xD8                ; cr3
    je      .emit_cr_rd
    cmp     rcx, 4
    mov     dl, 0xE0                ; cr4
    jne     .err_bad_cr
.emit_cr_rd:
    push    rdx
    mov     ax, 0x200F              ; 0F 20 /r = mov r64, crN
    call    emit_u16
    pop     rax
    call    emit_u8
    jmp     .done_u64

.do_cr_write:
    mov     al, TOK_LPAREN
    call    expect_tok_kind
    mov     al, TOK_COMMA
    call    .eval_const_arg_fenced  ; `rax` = constant `n`
    mov     rcx, rax
    cmp     rcx, 0
    mov     r13b, 0xC0
    je      .crw_val
    cmp     rcx, 3
    mov     r13b, 0xD8
    je      .crw_val
    cmp     rcx, 4
    mov     r13b, 0xE0
    jne     .err_bad_cr
.crw_val:
    mov     al, TOK_COMMA
    call    expect_tok_kind
    mov     edi, 1
    call    parse_expr              ; `rax` = value
    mov     al, TOK_RPAREN
    call    expect_tok_kind
    mov     ax, 0x220F              ; 0F 22 /r = mov crN, r64
    call    emit_u16
    mov     al, r13b
    call    emit_u8
    jmp     .done_u64

.do_msr_read:
    call    .eval_one_arg_rax       ; `rax` = msr_id
    lea     rsi, [seq_rdmsr]
    mov     ecx, seq_rdmsr_end - seq_rdmsr
    call    emit_bytes
    jmp     .done_u64

.do_msr_write:
    mov     al, TOK_LPAREN
    call    expect_tok_kind
    call    .eval_two_args_rcx_rax  ; `rcx` = msr_id, `rax` = 64-bit val
    lea     rsi, [seq_wrmsr]
    mov     ecx, seq_wrmsr_end - seq_wrmsr
    call    emit_bytes
    jmp     .done_u64

.do_xsetbv:
    mov     al, TOK_LPAREN
    call    expect_tok_kind
    call    .eval_two_args_rcx_rax  ; `rcx` = xcr, `rax` = 64-bit mask
    lea     rsi, [seq_xsetbv]
    mov     ecx, seq_xsetbv_end - seq_xsetbv
    call    emit_bytes
    jmp     .done_u64

.do_port_in8:
    call    .eval_one_arg_rax       ; `rax` = port
    mov     ax, 0xC289              ; 89 C2 = mov edx, eax
    call    emit_u16
    mov     ax, 0xC031              ; 31 C0 = xor eax, eax
    call    emit_u16
    mov     al, 0xEC                ; EC = in al, dx
    call    emit_u8
    mov     dword [last_expr_ty], TY_U8
    jmp     .done

.do_port_out8:
    mov     al, TOK_LPAREN
    call    expect_tok_kind
    call    .eval_two_args_rcx_rax  ; `rcx` = port, `rax` = byte val
    mov     ax, 0xCA89              ; 89 CA = mov edx, ecx
    call    emit_u16
    mov     al, 0xEE                ; EE = out dx, al
    call    emit_u8
    jmp     .done_u64

.do_mm_read32:
    call    .eval_one_arg_rax
    mov     ax, 0x008B              ; mov eax, [rax]
    call    emit_u16
    mov     dword [last_expr_ty], TY_U32
    jmp     .done

.do_mm_read64:
    call    .eval_one_arg_rax
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0x008B              ; mov rax, [rax]
    call    emit_u16
    jmp     .done_u64

.do_mm_write32:
    mov     al, TOK_LPAREN
    call    expect_tok_kind
    call    .eval_two_args_rcx_rax  ; rcx = addr, rax = u32 val
    mov     ax, 0x0189              ; mov [rcx], eax
    call    emit_u16
    jmp     .done_u64

.do_mm_write64:
    mov     al, TOK_LPAREN
    call    expect_tok_kind
    call    .eval_two_args_rcx_rax  ; rcx = addr, rax = u64 val
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0x0189              ; mov [rcx], rax
    call    emit_u16
    jmp     .done_u64

.do_isr_base:
    call    .expect_empty_parens
    ; Emit `lea rax, [rip + disp32]` targeting `[efi_isr_stubs_off]` in `.text`!
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0x058D              ; 48 8D 05 <disp32>
    call    emit_u16
    mov     edi, [efi_isr_stubs_off]
    mov     eax, [code_off]
    add     eax, 4
    sub     edi, eax
    mov     eax, edi
    call    emit_u32
    jmp     .done_u64

.done_u64:
    mov     dword [last_expr_ty], TY_U64
.done:
    pop     r13
    pop     r12
    pop     rbx
    ret

; --- Internal Argument Parsing Helpers ---
.expect_empty_parens:
    mov     al, TOK_LPAREN
    call    expect_tok_kind
    mov     al, TOK_RPAREN
    jmp     expect_tok_kind

.eval_one_arg_rax:
    mov     al, TOK_LPAREN
    call    expect_tok_kind
    mov     edi, 1
    call    parse_expr
    mov     al, TOK_RPAREN
    jmp     expect_tok_kind

; Expects opening `(` already consumed; evaluates `arg0 -> rcx, arg1 -> rax`, consumes `)`
.eval_two_args_rcx_rax:
    push    rbx
    mov     edi, 1
    call    parse_expr
    call    alloc_temp_qword
    mov     ebx, eax
    mov     edi, ebx
    call    emit_store_local

    mov     al, TOK_COMMA
    call    expect_tok_kind
    mov     edi, 1
    call    parse_expr              ; arg1 in `rax`

    mov     al, TOK_RPAREN
    call    expect_tok_kind

    ; Load arg0 from `[rbp + ebx]` into `rcx` (`48 8B 8D <disp32>`)
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0x8D8B
    call    emit_u16
    mov     eax, ebx
    call    emit_u32

    mov     edi, 1
    call    free_temp_qwords
    pop     rbx
    ret

; Evaluates a required constant argument up to delimiter `al` (`TOK_COMMA` or `TOK_RPAREN`)
; Leaves `[tok_idx]` pointing at delimiter `al`; faults via `.err_bad_cr` if non-constant.
.eval_const_arg_fenced:
    push    rbx
    push    r14
    movzx   r14d, al                ; delimiter token kind
    mov     ebx, [tok_idx]          ; start tok_idx
    xor     edx, edx                ; paren depth
.ec_scan:
    call    peek_tok_ptr
    movzx   ecx, byte [rax]
    test    cl, cl
    jz      parse_err_at_tok
    cmp     cl, TOK_LPAREN
    jne     .ec_rp
    inc     edx
    inc     dword [tok_idx]
    jmp     .ec_scan
.ec_rp:
    cmp     cl, TOK_RPAREN
    jne     .ec_delim
    test    edx, edx
    jz      .ec_chk_end
    dec     edx
    inc     dword [tok_idx]
    jmp     .ec_scan
.ec_delim:
    test    edx, edx
    jnz     .ec_adv
.ec_chk_end:
    cmp     ecx, r14d
    je      .ec_fenced
.ec_adv:
    ; Reject non-constant tokens cleanly with `msg_bad_intrinsic_arg`
    cmp     cl, TOK_DOT
    je      .ec_fail
    cmp     cl, TOK_LBRACK
    je      .ec_fail
    cmp     cl, TOK_AT
    je      .ec_fail
    inc     dword [tok_idx]
    jmp     .ec_scan
.ec_fenced:
    cmp     [tok_idx], ebx
    je      .ec_fail
    mov     r14, rax                ; delimiter token ptr
    movzx   edx, byte [r14]
    push    rdx
    mov     byte [r14], TOK_EOF
    mov     byte [const_trial_mode], 1
    mov     edi, ebx
    call    try_eval_const_at
    mov     byte [const_trial_mode], 0
    pop     rcx
    mov     [r14], cl
    test    eax, eax
    jz      .ec_fail
    cmp     byte [r14], cl          ; ensure restored
    mov     rax, rdx
    pop     r14
    pop     rbx
    ret
.ec_fail:
    pop     r14
    pop     rbx
    jmp     .err_bad_cr

; Non-destructively checks if arg0 before `,` is a compile-time constant with `(val & 7) != 0`
.check_const_align8_before_comma:
    push    rbx
    push    r14
    push    r15
    mov     ebx, [tok_idx]          ; save start tok_idx
    xor     edx, edx                ; paren depth
.ca_scan:
    call    peek_tok_ptr
    movzx   ecx, byte [rax]
    test    cl, cl
    jz      .ca_skip
    cmp     cl, TOK_LPAREN
    jne     .ca_rp
    inc     edx
    inc     dword [tok_idx]
    jmp     .ca_scan
.ca_rp:
    cmp     cl, TOK_RPAREN
    jne     .ca_chk_comma
    test    edx, edx
    jz      .ca_skip
    dec     edx
    inc     dword [tok_idx]
    jmp     .ca_scan
.ca_chk_comma:
    test    edx, edx
    jnz     .ca_tok_ok
    cmp     cl, TOK_COMMA
    je      .ca_try
.ca_tok_ok:
    ; Runtime-only tokens (`.`, `[`, `@`) mean arg0 is not a constant expression
    cmp     cl, TOK_DOT
    je      .ca_skip
    cmp     cl, TOK_LBRACK
    je      .ca_skip
    cmp     cl, TOK_AT
    je      .ca_skip
    inc     dword [tok_idx]
    jmp     .ca_scan
.ca_try:
    mov     r15d, [tok_idx]         ; comma tok_idx
    cmp     r15d, ebx
    je      .ca_skip
    mov     r14, rax
    movzx   ecx, byte [r14]
    push    rcx
    mov     byte [r14], TOK_EOF
    mov     byte [const_trial_mode], 1
    mov     edi, ebx
    call    try_eval_const_at
    mov     byte [const_trial_mode], 0
    pop     rcx
    mov     [r14], cl
    cmp     [tok_idx], r15d         ; must have consumed entire arg0 up to `,`
    jne     .ca_skip
    mov     [tok_idx], ebx          ; rewind cursor for normal codegen!
    test    eax, eax
    jz      .ca_done                ; runtime variable -> ok
    test    rdx, 7                  ; constant -> must be 8-byte aligned!
    jnz     .ca_unaligned
    jmp     .ca_done
.ca_skip:
    mov     [tok_idx], ebx
.ca_done:
    pop     r15
    pop     r14
    pop     rbx
    ret
.ca_unaligned:
    pop     r15
    pop     r14
    pop     rbx
    jmp     .err_bad_cr

; --- Position-Independent Ring-0 Instruction Sequences ---
seq_lgdt_reload:
    sub     rsp, 16
    mov     [rsp], ax
    mov     [rsp + 2], rcx
    lgdt    [rsp]
    add     rsp, 16
    mov     eax, 0x10
    mov     ds, ax
    mov     es, ax
    mov     ss, ax
    push    0x08
    lea     rax, [rel .cs_ok]
    push    rax
    retfq
.cs_ok:
    xor     eax, eax
seq_lgdt_reload_end:

seq_lidt_load:
    sub     rsp, 16
    mov     [rsp], ax
    mov     [rsp + 2], rcx
    lidt    [rsp]
    add     rsp, 16
    xor     eax, eax
seq_lidt_load_end:

seq_rdmsr:
    mov     ecx, eax
    rdmsr
    shl     rdx, 32
    or      rax, rdx
seq_rdmsr_end:

seq_wrmsr:
    mov     rdx, rax
    shr     rdx, 32
    wrmsr
    xor     eax, eax
seq_wrmsr_end:

seq_xsetbv:
    mov     rdx, rax
    shr     rdx, 32
    xsetbv
    xor     eax, eax
seq_xsetbv_end:
