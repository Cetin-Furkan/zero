; =============================================================================
; Zero Stage-0 Bootstrap — Phase-3 Reactor ABI & Silicon Fabric (`bootstrap/reactor.inc`)
; =============================================================================
; Implements `PEER_MASTER_PLAN.md` §6 & §9 + Modern Direct-Silicon Primitives:
;   1. Pinned Reactor ABI registers (`r12`=id, `r13`=inbox, `r14`=scratch, `r15`/`gs:0`=local)
;   2. `publish(dst, cell)` -> hardware `movdir64b` (64B atomic direct store, zero RFO)
;   3. `@movdiri(dst, val64)` -> hardware `movdiri` (8B direct-store doorbell, zero RFO)
;   4. `wait(addr, deadline_tsc)` -> `IA32_TSC_DEADLINE` + Ring-0 `monitor`/`mwait`
;   5. Cooperative `<tile>` quanta (`gs:8` depth + `gs:16` RFLAGS.IF save/restore)
;   6. `@isr` / `@isr_ok` hardware interrupt gates (`iretq` + static register/fault ban)
; =============================================================================

; -----------------------------------------------------------------------------
; Pinpoint Error Helpers for Reactor / ISR / `publish` Violations
; -----------------------------------------------------------------------------
err_need_efi_cur:
    call    peek_tok_ptr
    mov     esi, [rax + 4]
err_need_efi_line:
    lea     rdi, [msg_need_efi]
    mov     edx, msg_need_efi_len
    jmp     err_msg_at_line

err_isr_ban_cur:
    call    peek_tok_ptr
    mov     esi, [rax + 4]
err_isr_ban_line:
    lea     rdi, [msg_isr_violation]
    mov     edx, msg_isr_violation_len
    jmp     err_msg_at_line

err_bad_publish_cur:
    call    peek_tok_ptr
    mov     esi, [rax + 4]
err_bad_publish_line:
    lea     rdi, [msg_bad_publish]
    mov     edx, msg_bad_publish_len
    jmp     err_msg_at_line

; -----------------------------------------------------------------------------
; `<tile>` Hardware Quantum Entry, Exit & Unwind (`gs:8` = depth, `gs:16` = IF)
; -----------------------------------------------------------------------------
emit_tile_enter:                    ; esi = source line number
    cmp     byte [mode_efi], 1
    jne     err_need_efi_line
    cmp     byte [in_isr_mode], 0
    jne     err_isr_ban_line
    lea     rsi, [seq_tile_enter]
    mov     ecx, seq_tile_enter_end - seq_tile_enter
    call    emit_bytes
    inc     dword [tile_lex_depth]
    ret

emit_tile_exit:
    lea     rsi, [seq_tile_exit]
    mov     ecx, seq_tile_exit_end - seq_tile_exit
    call    emit_bytes
    dec     dword [tile_lex_depth]
    ret

; Unwinds `ecx` active `<tile>` levels without clobbering `rax` or `rdx`!
emit_tile_unwind_count:             ; ecx = count (> 0)
    test    ecx, ecx
    jz      .done
    push    rcx
    lea     rsi, [seq_tile_sub_prefix]
    mov     ecx, seq_tile_sub_prefix_end - seq_tile_sub_prefix
    call    emit_bytes
    pop     rax                     ; al = imm8 count to subtract from `[gs:8]`
    call    emit_u8
    lea     rsi, [seq_tile_sub_tail]
    mov     ecx, seq_tile_sub_tail_end - seq_tile_sub_tail
    call    emit_bytes
.done:
    ret

; Standalone `tile { ... }` statement block
parse_stmt_tile:
    call    peek_tok_ptr
    mov     esi, [rax + 4]
    inc     dword [tok_idx]         ; consume `tile`
    call    emit_tile_enter
    call    parse_block
    jmp     emit_tile_exit

; -----------------------------------------------------------------------------
; `publish(dst_addr, cell_expr)` -> Validated `MOVDIR64B` Direct Cacheline Store
; -----------------------------------------------------------------------------
parse_stmt_or_expr_publish:
    push    rbx
    push    r12
    push    r13

    call    peek_tok_ptr
    mov     r12d, [rax + 4]         ; line number
    cmp     byte [in_isr_mode], 0
    jne     err_isr_ban_line

    inc     dword [tok_idx]         ; consume `publish`
    mov     al, TOK_LPAREN
    call    expect_tok_kind

    ; 1. Evaluate `dst_addr` -> `rax`; verify `rax != 0` and `(rax & 63) == 0` -> else `fail 3`
    mov     edi, 1
    call    parse_expr
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC085              ; 48 85 C0 = test rax, rax
    call    emit_u16
    mov     ax, 0x840F              ; 0F 84 <rel32> = jz .bad_dst
    call    emit_u16
    mov     ebx, [code_off]
    xor     eax, eax
    call    emit_u32

    mov     ax, 0x3FA8              ; A8 3F = test al, 63
    call    emit_u16
    mov     ax, 0x840F              ; 0F 84 <rel32> = jz .dst_ok
    call    emit_u16
    mov     r13d, [code_off]
    xor     eax, eax
    call    emit_u32

    ; .bad_dst:
    mov     edi, ebx
    call    patch_rel32_here
    mov     edi, 3
    call    emit_raise_fault_imm

    ; .dst_ok:
    mov     edi, r13d
    call    patch_rel32_here

    call    alloc_temp_qword
    mov     ebx, eax                ; temp slot holding 64B-aligned `dst_addr`
    mov     edi, ebx
    call    emit_store_local

    ; 2. Parse `,` + `cell_expr` -> `rax` = `&cell`
    mov     al, TOK_COMMA
    call    expect_tok_kind
    mov     edi, 1
    call    parse_expr

    ; Static verification (`PEER_MASTER_PLAN.md` §6.3): `sizeof == 64 && alignof == 64`!
    mov     r13d, [last_expr_ty]
    test    r13d, TY_FLAG_SPAN | TY_FLAG_ATOMIC
    jnz     .bad_pub
    mov     edi, r13d
    and     edi, TY_BASE_MASK
    cmp     edi, TY_STRUCT_BASE
    jb      .bad_pub
    cmp     edi, TY_CUSTOM_PTR_BASE
    jae     .bad_pub
    call    type_size_align         ; eax = size, edx = align
    cmp     eax, 64
    jne     .bad_pub
    cmp     edx, 64
    jne     .bad_pub

    test    r13d, TY_FLAG_VIEW_ONE
    jz      .close_pub
    test    r13d, TY_FLAG_MMIO
    jnz     .close_pub
    call    emit_null_view_check_rax

.close_pub:
    mov     al, TOK_RPAREN
    call    expect_tok_kind

    ; `rcx` = `&cell` (`rax`), `rax` = `dst_addr` (`[rbp + ebx]`), emit `movdir64b rax, [rcx]`!
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC189              ; 48 89 C1 = mov rcx, rax
    call    emit_u16
    mov     edi, ebx
    call    emit_load_local         ; rax = dst_addr
    mov     edi, 1
    call    free_temp_qwords

    lea     rsi, [seq_movdir64b]
    mov     ecx, seq_movdir64b_end - seq_movdir64b
    call    emit_bytes

    mov     dword [last_expr_ty], TY_U64
    pop     r13
    pop     r12
    pop     rbx
    ret

.bad_pub:
    mov     esi, r12d
    jmp     err_bad_publish_line

; -----------------------------------------------------------------------------
; `wait(monitor_addr, deadline_tsc [, expected_old_u64])`
;   - 2 args: `IA32_TSC_DEADLINE` (if != 0) + `monitor` / `mwait` (§6.3)
;   - 3 args: Intel SDM §9.10.4 race-free `monitor -> cmp [rax], r8 -> jne -> mwait`!
; -----------------------------------------------------------------------------
parse_stmt_or_expr_wait:
    push    rbx
    push    r12
    push    r13

    call    peek_tok_ptr
    mov     esi, [rax + 4]
    cmp     byte [mode_efi], 1
    jne     err_need_efi_line
    cmp     byte [in_isr_mode], 0
    jne     err_isr_ban_line

    inc     dword [tok_idx]         ; consume `wait`
    mov     al, TOK_LPAREN
    call    expect_tok_kind

    mov     edi, 1
    call    parse_expr              ; `monitor_addr` -> temp slot `ebx`
    call    alloc_temp_qword
    mov     ebx, eax
    mov     edi, ebx
    call    emit_store_local

    mov     al, TOK_COMMA
    call    expect_tok_kind
    mov     edi, 1
    call    parse_expr              ; `deadline_tsc` -> `rax`

    call    peek_tok_ptr
    cmp     byte [rax], TOK_COMMA
    je      .wait_3_args

    mov     al, TOK_RPAREN
    call    expect_tok_kind

    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC189              ; mov rcx, rax (`deadline_tsc`)
    call    emit_u16
    mov     edi, ebx
    call    emit_load_local         ; rax = `monitor_addr`
    mov     edi, 1
    call    free_temp_qwords

    lea     rsi, [seq_wait_mwait]
    mov     ecx, seq_wait_mwait_end - seq_wait_mwait
    call    emit_bytes
    jmp     .wait_ret

.wait_3_args:
    call    alloc_temp_qword
    mov     r13d, eax               ; temp slot holding `deadline_tsc`
    mov     edi, r13d
    call    emit_store_local

    inc     dword [tok_idx]         ; consume `,`
    mov     edi, 1
    call    parse_expr              ; `expected_old_u64` -> `rax`
    mov     al, TOK_RPAREN
    call    expect_tok_kind

    mov     al, 0x49
    call    emit_u8
    mov     ax, 0xC089              ; mov r8, rax (`expected_old_u64`)
    call    emit_u16
    mov     edi, r13d
    call    emit_load_local_rcx     ; rcx = `deadline_tsc`
    mov     edi, ebx
    call    emit_load_local         ; rax = `monitor_addr`
    mov     edi, 2
    call    free_temp_qwords

    lea     rsi, [seq_wait_mwait_guarded]
    mov     ecx, seq_wait_mwait_guarded_end - seq_wait_mwait_guarded
    call    emit_bytes

.wait_ret:
    mov     dword [last_expr_ty], TY_U64
    pop     r13
    pop     r12
    pop     rbx
    ret

; -----------------------------------------------------------------------------
; Reactor ABI & Direct-Silicon `@` Intrinsics
; Input: `ebx = sym_id`, `r12d = line`
; Returns: `eax = 1` if handled, `0` otherwise.
; -----------------------------------------------------------------------------
try_parse_reactor_intrinsic:
    cmp     ebx, [sym_reactor_id]
    je      .do_rid
    cmp     ebx, [sym_inbox_base_id]
    je      .do_inbox
    cmp     ebx, [sym_scratch_base_id]
    je      .do_scratch
    cmp     ebx, [sym_local_base_id]
    je      .do_local
    cmp     ebx, [sym_reactor_bind_id]
    je      .do_bind
    cmp     ebx, [sym_wrgsbase_id]
    je      .do_wrgs
    cmp     ebx, [sym_rdgsbase_id]
    je      .do_rdgs
    cmp     ebx, [sym_eoi_id]
    je      .do_eoi
    cmp     ebx, [sym_movdiri_id]
    je      .do_movdiri
    cmp     ebx, [sym_cpuid_ecx_id]
    je      .do_cpuid_ecx
    cmp     ebx, [sym_cpuid_ebx_id]
    je      .do_cpuid_ebx
    xor     eax, eax
    ret

.chk_efi_non_isr:
    mov     esi, r12d
    cmp     byte [mode_efi], 1
    jne     err_need_efi_line
    cmp     byte [in_isr_mode], 0
    jne     err_isr_ban_line
    ret

.expect_0_args:
    mov     al, TOK_LPAREN
    call    expect_tok_kind
    mov     al, TOK_RPAREN
    jmp     expect_tok_kind

.do_rid:
    call    .chk_efi_non_isr
    call    .expect_0_args
    mov     al, 0x4C
    call    emit_u8
    mov     ax, 0xE089              ; 4C 89 E0 = mov rax, r12
    call    emit_u16
    mov     dword [last_expr_ty], TY_U64
    mov     eax, 1
    ret

.do_inbox:
    call    .chk_efi_non_isr
    call    .expect_0_args
    mov     al, 0x4C
    call    emit_u8
    mov     ax, 0xE889              ; 4C 89 E8 = mov rax, r13
    call    emit_u16
    mov     dword [last_expr_ty], TY_U64
    mov     eax, 1
    ret

.do_scratch:
    call    .chk_efi_non_isr
    call    .expect_0_args
    mov     al, 0x4C
    call    emit_u8
    mov     ax, 0xF089              ; 4C 89 F0 = mov rax, r14
    call    emit_u16
    mov     dword [last_expr_ty], TY_U64
    mov     eax, 1
    ret

.do_local:
    call    .chk_efi_non_isr
    call    .expect_0_args
    mov     al, 0x4C
    call    emit_u8
    mov     ax, 0xF889              ; 4C 89 F8 = mov rax, r15
    call    emit_u16
    mov     dword [last_expr_ty], TY_U64
    mov     eax, 1
    ret

.do_bind:
    call    .chk_efi_non_isr
    push    r13
    push    r14
    mov     al, TOK_LPAREN
    call    expect_tok_kind
    mov     edi, 1
    call    parse_expr              ; id
    call    alloc_temp_qword
    mov     r13d, eax
    mov     edi, r13d
    call    emit_store_local

    mov     al, TOK_COMMA
    call    expect_tok_kind
    mov     edi, 1
    call    parse_expr              ; inbox_base
    call    alloc_temp_qword
    mov     r14d, eax
    mov     edi, r14d
    call    emit_store_local

    mov     al, TOK_COMMA
    call    expect_tok_kind
    mov     edi, 1
    call    parse_expr              ; scratch_base -> `rax`
    mov     al, TOK_RPAREN
    call    expect_tok_kind

    mov     al, 0x49
    call    emit_u8
    mov     ax, 0xC689              ; 49 89 C6 = mov r14, rax (scratch_base)
    call    emit_u16
    mov     edi, r14d
    call    emit_load_local
    mov     al, 0x49
    call    emit_u8
    mov     ax, 0xC589              ; 49 89 C5 = mov r13, rax (inbox_base)
    call    emit_u16
    mov     edi, r13d
    call    emit_load_local
    mov     al, 0x49
    call    emit_u8
    mov     ax, 0xC489              ; 49 89 C4 = mov r12, rax (reactor_id)
    call    emit_u16

    mov     edi, 2
    call    free_temp_qwords
    mov     dword [last_expr_ty], TY_U64
    pop     r14
    pop     r13
    mov     eax, 1
    ret

.do_wrgs:
    call    .chk_efi_non_isr
    mov     al, TOK_LPAREN
    call    expect_tok_kind
    mov     edi, 1
    call    parse_expr
    mov     al, TOK_RPAREN
    call    expect_tok_kind
    lea     rsi, [seq_wrgsbase]
    mov     ecx, seq_wrgsbase_end - seq_wrgsbase
    call    emit_bytes
    mov     dword [last_expr_ty], TY_U64
    mov     eax, 1
    ret

.do_rdgs:
    call    .chk_efi_non_isr
    call    .expect_0_args
    lea     rsi, [seq_rdgsbase]
    mov     ecx, seq_rdgsbase_end - seq_rdgsbase
    call    emit_bytes
    mov     dword [last_expr_ty], TY_U64
    mov     eax, 1
    ret

.do_eoi:
    mov     esi, r12d
    cmp     byte [mode_efi], 1
    jne     err_need_efi_line       ; ALLOWED inside `@isr` & `@isr_ok`!
    call    .expect_0_args
    lea     rsi, [seq_x2apic_eoi]
    mov     ecx, seq_x2apic_eoi_end - seq_x2apic_eoi
    call    emit_bytes
    mov     dword [last_expr_ty], TY_U64
    mov     eax, 1
    ret

.do_movdiri:
    push    r13
    mov     al, TOK_LPAREN
    call    expect_tok_kind
    mov     edi, 1
    call    parse_expr              ; dst_addr
    call    alloc_temp_qword
    mov     r13d, eax
    mov     edi, r13d
    call    emit_store_local
    mov     al, TOK_COMMA
    call    expect_tok_kind
    mov     edi, 1
    call    parse_expr              ; val64 -> `rax`
    mov     al, TOK_RPAREN
    call    expect_tok_kind
    mov     edi, r13d
    call    emit_load_local_rcx     ; rcx = dst_addr
    mov     edi, 1
    call    free_temp_qwords
    lea     rsi, [seq_movdiri]
    mov     ecx, seq_movdiri_end - seq_movdiri
    call    emit_bytes
    mov     dword [last_expr_ty], TY_U64
    pop     r13
    mov     eax, 1
    ret

.do_cpuid_ecx:
    push    r13
    call    .eval_cpuid_args
    lea     rsi, [seq_cpuid_ecx]
    mov     ecx, seq_cpuid_ecx_end - seq_cpuid_ecx
    call    emit_bytes
    mov     dword [last_expr_ty], TY_U32
    pop     r13
    mov     eax, 1
    ret

.do_cpuid_ebx:
    push    r13
    call    .eval_cpuid_args
    lea     rsi, [seq_cpuid_ebx]
    mov     ecx, seq_cpuid_ebx_end - seq_cpuid_ebx
    call    emit_bytes
    mov     dword [last_expr_ty], TY_U32
    pop     r13
    mov     eax, 1
    ret

.eval_cpuid_args:
    mov     al, TOK_LPAREN
    call    expect_tok_kind
    mov     edi, 1
    call    parse_expr              ; leaf -> temp `r13d`
    call    alloc_temp_qword
    mov     r13d, eax
    mov     edi, r13d
    call    emit_store_local
    mov     al, TOK_COMMA
    call    expect_tok_kind
    mov     edi, 1
    call    parse_expr              ; subleaf -> `rcx`
    mov     al, TOK_RPAREN
    call    expect_tok_kind
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC189              ; mov rcx, rax (subleaf)
    call    emit_u16
    mov     edi, r13d
    call    emit_load_local         ; rax = leaf
    mov     edi, 1
    jmp     free_temp_qwords

; -----------------------------------------------------------------------------
; `@isr` Hardware Interrupt Gate Prologue & `iretq` Epilogue
; -----------------------------------------------------------------------------
emit_isr_prologue:                  ; returns `eax` = `sub rsp, imm32` patch offset!
    lea     rsi, [seq_isr_prolog_pre]
    mov     ecx, seq_isr_prolog_pre_end - seq_isr_prolog_pre
    call    emit_bytes
    mov     eax, [code_off]
    push    rax
    xor     eax, eax
    call    emit_u32
    lea     rsi, [seq_isr_prolog_post]
    mov     ecx, seq_isr_prolog_post_end - seq_isr_prolog_post
    call    emit_bytes
    pop     rax
    ret

emit_isr_epilogue:
    lea     rsi, [seq_isr_epilog]
    mov     ecx, seq_isr_epilog_end - seq_isr_epilog
    jmp     emit_bytes

; =============================================================================
; NASM-Assembled Exact Machine-Instruction Templates
; =============================================================================
seq_movdir64b:
    movdir64b rax, [rcx]
seq_movdir64b_end:

seq_movdiri:
    movdiri [rcx], rax
seq_movdiri_end:

seq_wrgsbase:
    mov     r15, rax
    wrgsbase rax
seq_wrgsbase_end:

seq_rdgsbase:
    rdgsbase rax
seq_rdgsbase_end:

seq_x2apic_eoi:
    xor     eax, eax
    xor     edx, edx
    mov     ecx, 0x80B
    wrmsr
seq_x2apic_eoi_end:

seq_cpuid_ecx:
    push    rbx
    cpuid
    mov     eax, ecx
    pop     rbx
seq_cpuid_ecx_end:

seq_cpuid_ebx:
    push    rbx
    cpuid
    mov     eax, ebx
    pop     rbx
seq_cpuid_ebx_end:

seq_tile_enter:
    cmp     qword [gs:8], 0
    jne     .nested
    pushfq
    pop     rax
    shr     eax, 9
    and     eax, 1
    mov     [gs:16], rax
    cli
.nested:
    inc     qword [gs:8]
seq_tile_enter_end:

seq_tile_exit:
    dec     qword [gs:8]
    jnz     .still
    cmp     qword [gs:16], 0
    je      .still
    sti
.still:
seq_tile_exit_end:

seq_tile_sub_prefix:
    db 0x65, 0x48, 0x83, 0x2C, 0x25, 0x08, 0x00, 0x00, 0x00 ; sub qword [gs:8], <imm8>
seq_tile_sub_prefix_end:

seq_tile_sub_tail:
    jnz     .still2
    cmp     qword [gs:16], 0
    je      .still2
    sti
.still2:
seq_tile_sub_tail_end:

seq_wait_mwait:
    test    rcx, rcx
    jz      .chk_mwait
    push    rax
    mov     rax, rcx
    mov     rdx, rcx
    shr     rdx, 32
    mov     ecx, 0x6E0
    wrmsr
    pop     rax
.chk_mwait:
    cmp     qword [gs:32], 0
    je      .fallback_pause
    xor     ecx, ecx
    xor     edx, edx
    monitor
    xor     eax, eax
    xor     ecx, ecx
    mwait
    jmp     .done
.fallback_pause:
    pause
.done:
    xor     eax, eax
seq_wait_mwait_end:

seq_wait_mwait_guarded:
    test    rcx, rcx
    jz      .g_chk_mwait
    push    rax
    mov     rax, rcx
    mov     rdx, rcx
    shr     rdx, 32
    mov     ecx, 0x6E0
    wrmsr
    pop     rax
.g_chk_mwait:
    cmp     qword [gs:32], 0
    je      .g_fallback_pause
    xor     ecx, ecx
    xor     edx, edx
    monitor
    cmp     [rax], r8               ; Intel SDM §9.10.4: recheck AFTER arming `monitor`!
    jne     .g_done
    xor     eax, eax
    xor     ecx, ecx
    mwait
    jmp     .g_done
.g_fallback_pause:
    pause
.g_done:
    xor     eax, eax
seq_wait_mwait_guarded_end:

seq_isr_prolog_pre:
    push    rbp
    push    rax
    push    rcx
    push    rdx
    push    rsi
    push    rdi
    push    r8
    push    r9
    push    r10
    push    r11
    cld
    mov     rbp, rsp
    db 0x48, 0x81, 0xEC             ; sub rsp, <imm32>
seq_isr_prolog_pre_end:

seq_isr_prolog_post:
    and     rsp, -16
seq_isr_prolog_post_end:

seq_isr_epilog:
    mov     rsp, rbp
    pop     r11
    pop     r10
    pop     r9
    pop     r8
    pop     rdi
    pop     rsi
    pop     rdx
    pop     rcx
    pop     rax
    pop     rbp
    iretq
seq_isr_epilog_end:
