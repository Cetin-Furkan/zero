; =============================================================================
; Zero Stage-0 Bootstrap — Phase-0 Scalar-Frozen Parser (`bootstrap/parser.inc`)
; =============================================================================
; Implements all Phase-0 Scalar Guarantees (`PEER_MASTER_PLAN.md` §5):
;   1. Signed vs Unsigned lowering (`shr` vs `sar`, `div` vs `idiv`,
;      `setb/be/a/ae` vs `setl/le/g/ge`) + div/mod by zero -> `fail 1`.
;   2. Lexical `{}` brace scoping, shadowing & same-depth duplicate rejection.
;   3. Unsigned `@slab[i]` / `@arena[i]` bounds check `i >= cap` -> `fail 2`.
;   4. Context-exact `|` (`ident :` only) & `>` disambiguation in `<...>`.
; =============================================================================

; -----------------------------------------------------------------------------
; Signedness Classification (`is_ty_unsigned`)
; Returns `eax = 1` for `u8/u16/u32/u64/bit/bool/mask64` & all bitpacks, else `0`
; -----------------------------------------------------------------------------
is_ty_unsigned:                     ; edi = ty_tag
    test    edi, TY_FLAG_VIEW_ONE | TY_FLAG_SPAN
    jnz     .yes
    and     edi, TY_BASE_MASK
    cmp     edi, TY_U8
    je      .yes
    cmp     edi, TY_U16
    je      .yes
    cmp     edi, TY_U32
    je      .yes
    cmp     edi, TY_U64
    je      .yes
    cmp     edi, TY_BITPACK_BASE
    jb      .no
    cmp     edi, TY_STRUCT_BASE
    jb      .yes
.no:
    xor     eax, eax
    ret
.yes:
    mov     eax, 1
    ret

; Emits immediate scalar fault (`edi = fault_code`, e.g. `1` div0, `2` bounds)
emit_raise_fault_imm:
    call    emit_fault_store_imm32

    cmp     dword [rec_depth], 0
    jg      .jmp_rec
    mov     ecx, [tile_lex_depth]
    call    emit_tile_unwind_count
    mov     eax, 0xC3C9C031         ; xor eax, eax; leave; ret
    jmp     emit_u32
.jmp_rec:
    jmp     emit_unwind_to_recover

; Emits unsigned bounds check on target `rax` (`idx`) against host `rax` (`cap`)
; Raises `fail 2` if `idx >= cap` (catches negative i64 indices via unsigned `jb`!)
emit_bounds_check_rax:              ; host `rax` = u64 capacity
    call    emit_mov_rcx_imm64      ; movabs rcx, cap (guest rax untouched)
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC839              ; 48 39 C8 = cmp rax, rcx
    call    emit_u16
    mov     ax, 0x820F              ; 0F 82 <rel32> = jb .in_bounds
    call    emit_u16
    mov     eax, [code_off]
    push    rax
    xor     eax, eax
    call    emit_u32
    mov     edi, 2
    call    emit_raise_fault_imm    ; `fail 2`
    pop     rdi
    jmp     patch_rel32_here

; Emits division-by-zero check on `rcx` -> raises `fail 1` if `rcx == 0`
emit_div_zero_check_rcx:
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC985              ; 48 85 C9 = test rcx, rcx
    call    emit_u16
    mov     ax, 0x850F              ; 0F 85 <rel32> = jnz .divisor_ok
    call    emit_u16
    mov     eax, [code_off]
    push    rax
    xor     eax, eax
    call    emit_u32
    mov     edi, 1
    call    emit_raise_fault_imm    ; `fail 1`
    pop     rdi
    jmp     patch_rel32_here

; -----------------------------------------------------------------------------
; Function & Lexically Scoped Local Tables
; -----------------------------------------------------------------------------
find_or_add_fn:
    mov     rcx, [space_ast]
    add     rcx, AST_FN_OFF
    xor     eax, eax
    mov     r8d, [fn_cnt]
.loop:
    cmp     eax, r8d
    jae     .add_new
    mov     r9d, eax
    shl     r9d, 4
    cmp     [rcx + r9], edi
    je      .done
    inc     eax
    jmp     .loop
.add_new:
    mov     r9d, eax
    inc     dword [fn_cnt]
    shl     r9d, 4
    mov     [rcx + r9], edi
    mov     dword [rcx + r9 + 4], 0
    mov     dword [rcx + r9 + 8], 0
    mov     dword [rcx + r9 + 12], TY_I64
.done:
    ret

emit_call_fn_idx:
    push    rbx
    mov     ebx, edi

    ; Enforce Phase-3 `@isr` / `@isr_ok` call-graph safety (`PEER_MASTER_PLAN.md` §6.2)
    cmp     byte [in_isr_mode], 0
    je      .isr_call_ok
    cmp     byte [in_isr_mode], 2   ; `@isr_ok` must be a pure leaf!
    je      err_isr_ban_cur
    mov     rcx, [space_ast]
    cmp     byte [rcx + AST_FN_ATTR_OFF + rbx], 2 ; `@isr` may ONLY call `@isr_ok`!
    jne     err_isr_ban_cur
.isr_call_ok:

    mov     al, 0xE8
    call    emit_u8
    mov     edx, [code_off]
    mov     rcx, [space_ast]
    add     rcx, AST_CALLFIX_OFF
    mov     eax, [call_fix_cnt]
    mov     [rcx + rax * 8],     edx
    mov     [rcx + rax * 8 + 4], ebx
    inc     dword [call_fix_cnt]
    xor     eax, eax
    call    emit_u32

    mov     rcx, [space_ast]
    add     rcx, AST_FN_OFF
    mov     edx, ebx
    shl     edx, 4
    mov     eax, [rcx + rdx + 12]
    mov     [last_expr_ty], eax

    cmp     byte [in_fn_body], 0
    je      .done
    cmp     byte [in_isr_mode], 0   ; `@isr_ok` callees never fault!
    jne     .chk_span_ret

    call    emit_fault_cmp_zero

    cmp     dword [rec_depth], 0
    jg      .unwind_to_recover

    cmp     dword [tile_lex_depth], 0
    jg      .unwind_with_tile

    mov     ax, 0x0474
    call    emit_u16
    mov     eax, 0xC3C9C031
    call    emit_u32
    jmp     .chk_span_ret

.unwind_with_tile:
    mov     ax, 0x840F              ; jz .chk_span_ret
    call    emit_u16
    mov     eax, [code_off]
    push    rax
    xor     eax, eax
    call    emit_u32
    mov     ecx, [tile_lex_depth]
    call    emit_tile_unwind_count
    mov     eax, 0xC3C9C031
    call    emit_u32
    pop     rdi
    call    patch_rel32_here
    jmp     .chk_span_ret

.unwind_to_recover:
    mov     ax, 0x840F              ; jz .chk_span_ret
    call    emit_u16
    mov     eax, [code_off]
    push    rax
    xor     eax, eax
    call    emit_u32
    call    emit_unwind_to_recover
    pop     rdi
    call    patch_rel32_here

.chk_span_ret:
    test    dword [last_expr_ty], TY_FLAG_SPAN
    jz      .done
    ; Unpack 2-register `RAX:RDX` (`{ base, len }`) slice return into caller span slot!
    push    r12
    mov     al, 0x49
    call    emit_u8
    mov     ax, 0xD089              ; 49 89 D0 = mov r8, rdx (`len`)
    call    emit_u16
    call    alloc_anon_span_slot
    mov     r12d, eax
    mov     edi, r12d
    call    emit_store_local        ; `[rbp + slot] = base`
    mov     al, 0x4C
    call    emit_u8
    mov     ax, 0xC089              ; 4C 89 C0 = mov rax, r8
    call    emit_u16
    lea     edi, [r12d + 8]
    call    emit_store_local        ; `[rbp + slot + 8] = len`
    mov     edi, r12d
    call    emit_lea_local
    pop     r12

.done:
    pop     rbx
    ret

; find_local_entry(edi = sym_id) -> eax = negative rbp_disp (0 if none), edx = ty_tag
find_local_entry:
    mov     rcx, [space_ast]
    add     rcx, AST_LOCAL_OFF
    mov     eax, [local_cnt]
.loop:
    test    eax, eax
    jz      .not_found
    dec     eax
    mov     r8d, eax
    shl     r8d, 4
    cmp     [rcx + r8], edi
    je      .found
    jmp     .loop
.found:
    mov     edx, [rcx + r8 + 8]
    mov     eax, [rcx + r8 + 4]
    ret
.not_found:
    xor     eax, eax
    xor     edx, edx
    ret

; alloc_local_scoped(edi = sym_id, esi = ty_tag, edx = line_num) -> eax = rbp_disp32
; Rejects duplicate `sym_id != 0` at identical `[scope_depth]`; allows inner shadowing.
alloc_local_scoped:
    push    rbx
    push    r12
    push    r13
    mov     ebx,  edi               ; sym_id (0 = anonymous)
    mov     r12d, esi               ; ty_tag
    mov     r13d, edx               ; line_num

    test    ebx, ebx
    jz      .alloc_slot

    ; Check for duplicate declaration in the SAME `[scope_depth]`
    mov     rcx, [space_ast]
    add     rcx, AST_LOCAL_OFF
    mov     eax, [local_cnt]
    mov     r9d, [scope_depth]
.dup_chk:
    test    eax, eax
    jz      .alloc_slot
    dec     eax
    mov     r8d, eax
    shl     r8d, 4
    cmp     [rcx + r8 + 12], r9d    ; stop once below current brace depth
    jb      .alloc_slot
    cmp     [rcx + r8], ebx
    je      .dup_err
    jmp     .dup_chk

.dup_err:
    mov     esi, r13d
    lea     rdi, [msg_dup_local]
    mov     edx, msg_dup_local_len
    jmp     err_msg_at_line

.alloc_slot:
    mov     edi, r12d
    call    type_size_align
    add     eax, 7
    shr     eax, 3
    test    eax, eax
    jnz     .grow
    mov     eax, 1
.grow:
    add     eax, [frame_qwords]
    mov     [frame_qwords], eax
    cmp     eax, [max_frame_qwords]
    jbe     .calc_disp
    mov     [max_frame_qwords], eax
.calc_disp:
    mov     edx, eax
    shl     edx, 3
    neg     edx

    mov     ecx, [local_cnt]
    inc     dword [local_cnt]
    shl     ecx, 4
    mov     r8, [space_ast]
    add     r8, AST_LOCAL_OFF
    mov     [r8 + rcx],      ebx
    mov     [r8 + rcx + 4],  edx
    mov     [r8 + rcx + 8],  r12d
    mov     eax, [scope_depth]
    mov     [r8 + rcx + 12], eax
    mov     eax, edx

    pop     r13
    pop     r12
    pop     rbx
    ret

; Anonymous hidden slot helper (`edi = 0`)
alloc_anon_local:
    xor     edi, edi
    mov     esi, TY_I64
    xor     edx, edx
    jmp     alloc_local_scoped

; -----------------------------------------------------------------------------
; Recover Scope Stack Helpers
; -----------------------------------------------------------------------------
rec_push_scope:
    mov     eax, [rec_depth]
    mov     rcx, [space_ast]
    mov     dword [rcx + AST_REC_CNT_OFF + rax * 4], 0
    mov     edx, [tile_lex_depth]
    mov     [rcx + AST_REC_TILE_OFF + rax * 4], edx
    inc     dword [rec_depth]
    ret

; Unwinds any `<tile>` levels entered inside the active `{ ... } recover` scope, then jumps to `recover`!
emit_unwind_to_recover:
    mov     edx, [rec_depth]
    dec     edx
    mov     rcx, [space_ast]
    mov     eax, [tile_lex_depth]
    sub     eax, [rcx + AST_REC_TILE_OFF + rdx * 4]
    jle     .do_jmp
    mov     ecx, eax
    call    emit_tile_unwind_count
.do_jmp:
    call    emit_jmp_rel32
    mov     edi, eax
    jmp     rec_add_patch

rec_add_patch:
    mov     edx, [rec_depth]
    dec     edx
    mov     rcx, [space_ast]
    mov     esi, [rcx + AST_REC_CNT_OFF + rdx * 4]
    inc     dword [rcx + AST_REC_CNT_OFF + rdx * 4]
    shl     edx, 8
    lea     rcx, [rcx + AST_REC_PATCH_OFF + rdx]
    mov     [rcx + rsi * 4], edi
    ret

rec_pop_and_patch_here:
    push    rbx
    dec     dword [rec_depth]
    mov     ebx, [rec_depth]
    mov     rcx, [space_ast]
    mov     r8d, [rcx + AST_REC_CNT_OFF + rbx * 4]
    shl     ebx, 8
.r_loop:
    test    r8d, r8d
    jz      .done
    dec     r8d
    mov     rcx, [space_ast]
    lea     rcx, [rcx + AST_REC_PATCH_OFF + rbx]
    mov     edi, [rcx + r8 * 4]
    push    r8
    call    patch_rel32_here
    pop     r8
    jmp     .r_loop
.done:
    pop     rbx
    ret

block_has_trailing_recover:
    mov     ecx, [tok_idx]
    push    rcx
    call    skip_brace_block
    call    peek_tok_ptr
    xor     edx, edx
    cmp     byte [rax], TOK_KW
    jne     .restore
    cmp     byte [rax + 1], KW_RECOVER
    jne     .restore
    mov     edx, 1
.restore:
    pop     rcx
    mov     [tok_idx], ecx
    mov     eax, edx
    ret

; -----------------------------------------------------------------------------
; Descriptor `< ... >` Disambiguator & Skippers
; -----------------------------------------------------------------------------
is_descriptor_lt:
    call    peek_tok_ptr
    cmp     byte [rax], TOK_LT
    jne     .no
    movzx   ecx, byte [rax + 16]
    cmp     cl, TOK_IDENT
    je      .chk_t2
    cmp     cl, TOK_KW
    je      .chk_t2
    jmp     .no
.chk_t2:
    movzx   edx, byte [rax + 32]
    cmp     dl, TOK_COLON
    je      .yes
    cmp     dl, TOK_PIPE
    je      .yes
    cmp     dl, TOK_GT
    jne     .no
    cmp     byte [rax + 48], TOK_LBRACE
    je      .yes
.no:
    xor     eax, eax
    ret
.yes:
    mov     eax, 1
    ret

skip_angle_block:
    mov     al, TOK_LT
    call    expect_tok_kind
    mov     ecx, 1
.loop:
    call    next_tok_ptr
    movzx   edx, byte [rax]
    test    dl, dl
    jz      parse_err_at_tok
    cmp     dl, TOK_LT
    jne     .chk_gt
    inc     ecx
    jmp     .loop
.chk_gt:
    cmp     dl, TOK_GT
    jne     .loop
    dec     ecx
    jnz     .loop
    ret

parse_fn_type_annot:
    call    peek_tok_ptr
    cmp     byte [rax], TOK_BANG
    jne     .chk_at
    inc     dword [tok_idx]
    call    next_tok_ptr
    mov     eax, TY_I64
    ret
.chk_at:
    cmp     byte [rax], TOK_AT
    jne     .normal_ty
    inc     dword [tok_idx]
    call    peek_tok_ptr
    cmp     byte [rax], TOK_IDENT
    jne     .at_overlay
    mov     ecx, [rax + 8]
    cmp     ecx, [sym_span_id]
    je      .at_span_ty
    cmp     ecx, [sym_atomic_id]
    je      .at_atomic_ty
.at_overlay:
    call    parse_resolved_type_tag
    or      eax, TY_FLAG_VIEW_ONE
    ret
.at_span_ty:
    inc     dword [tok_idx]
    mov     al, TOK_LT
    call    expect_tok_kind
    call    parse_resolved_type_tag
    push    rax
    mov     al, TOK_GT
    call    expect_tok_kind
    pop     rax
    or      eax, TY_FLAG_SPAN
    ret
.at_atomic_ty:
    inc     dword [tok_idx]
    mov     al, TOK_LT
    call    expect_tok_kind
    call    parse_resolved_type_tag
    push    rax
    mov     al, TOK_GT
    call    expect_tok_kind
    pop     rax
    or      eax, TY_FLAG_ATOMIC
    ret
.normal_ty:
    jmp     parse_resolved_type_tag

; -----------------------------------------------------------------------------
; Compile-Time Constant Expression Evaluator
; -----------------------------------------------------------------------------
eval_const_expr:
    mov     edi, 1
    jmp     eval_const_prec

eval_const_prec:
    push    rbx
    push    r12
    push    r13
    mov     ebx, edi
    call    eval_const_unary
    mov     r13, rax

.loop:
    call    peek_tok_ptr
    movzx   eax, byte [rax]
    call    binop_precedence
    cmp     ecx, ebx
    jb      .done
    mov     r12d, eax
    inc     dword [tok_idx]
    lea     edi, [rcx + 1]
    call    eval_const_prec
    mov     rcx, rax
    mov     rax, r13

    cmp     byte [const_unresolved_hit], 0
    jne     .next

    cmp     r12d, TOK_PLUS
    jne     .c_sub
    add     rax, rcx
    jmp     .next
.c_sub:
    cmp     r12d, TOK_MINUS
    jne     .c_mul
    sub     rax, rcx
    jmp     .next
.c_mul:
    cmp     r12d, TOK_STAR
    jne     .c_div
    imul    rax, rcx
    jmp     .next
.c_div:
    cmp     r12d, TOK_SLASH
    jne     .c_mod
    test    rcx, rcx
    jz      parse_err_cur_tok
    cqo
    idiv    rcx
    jmp     .next
.c_mod:
    cmp     r12d, TOK_PERCENT
    jne     .c_shl
    test    rcx, rcx
    jz      parse_err_cur_tok
    cqo
    idiv    rcx
    mov     rax, rdx
    jmp     .next
.c_shl:
    cmp     r12d, TOK_SHL
    jne     .c_shr
    shl     rax, cl
    jmp     .next
.c_shr:
    cmp     r12d, TOK_SHR
    jne     .c_rol
    sar     rax, cl
    jmp     .next
.c_rol:
    cmp     r12d, TOK_ROL
    jne     .c_ror
    rol     rax, cl
    jmp     .next
.c_ror:
    cmp     r12d, TOK_ROR
    jne     .c_and
    ror     rax, cl
    jmp     .next
.c_and:
    cmp     r12d, TOK_AMP
    jne     .c_or
    and     rax, rcx
    jmp     .next
.c_or:
    cmp     r12d, TOK_PIPE
    jne     .c_xor
    or      rax, rcx
    jmp     .next
.c_xor:
    cmp     r12d, TOK_CARET
    jne     .c_eq
    xor     rax, rcx
    jmp     .next
.c_eq:
    cmp     r12d, TOK_EQ
    jne     .c_ne
    cmp     rax, rcx
    sete    al
    jmp     .c_cmp_z
.c_ne:
    cmp     r12d, TOK_NE
    jne     .c_lt
    cmp     rax, rcx
    setne   al
    jmp     .c_cmp_z
.c_lt:
    cmp     r12d, TOK_LT
    jne     .c_le
    cmp     rax, rcx
    setl    al
    jmp     .c_cmp_z
.c_le:
    cmp     r12d, TOK_LE
    jne     .c_gt
    cmp     rax, rcx
    setle   al
    jmp     .c_cmp_z
.c_gt:
    cmp     r12d, TOK_GT
    jne     .c_ge
    cmp     rax, rcx
    setg    al
    jmp     .c_cmp_z
.c_ge:
    cmp     r12d, TOK_GE
    jne     .chk_cand
    cmp     rax, rcx
    setge   al
.c_cmp_z:
    movzx   eax, al
    jmp     .next
.chk_cand:
    cmp     r12d, TOK_AND2
    je      .c_and2
    cmp     r12d, TOK_OR2
    je      .c_or2
    jmp     parse_err_cur_tok
.c_and2:
    xor     edx, edx
    test    rax, rax
    jz      .c_log_done
    test    rcx, rcx
    setne   dl
.c_log_done:
    mov     eax, edx
    jmp     .next
.c_or2:
    mov     edx, 1
    test    rax, rax
    jnz     .c_log_done
    test    rcx, rcx
    jnz     .c_log_done
    xor     edx, edx
    jmp     .c_log_done
.next:
    mov     r13, rax
    jmp     .loop
.done:
    mov     rax, r13
    pop     r13
    pop     r12
    pop     rbx
    ret

eval_const_unary:
    call    .primary
.bs_lp:
    push    rax
    call    peek_tok_ptr
    cmp     byte [rax], TOK_LBRACK
    jne     .bs_dn
    call    parse_bitslice_bounds   ; eax = lo, edx = width
    mov     ecx, eax
    pop     rax
    shr     rax, cl
    mov     ecx, edx
    cmp     ecx, 64
    jae     .bs_lp
    mov     rdx, 1
    shl     rdx, cl
    dec     rdx
    and     rax, rdx
    jmp     .bs_lp
.bs_dn:
    pop     rax
    ret
.primary:
    call    peek_tok_ptr
    movzx   ecx, byte [rax]
    cmp     cl, TOK_MINUS
    je      .neg
    cmp     cl, TOK_TILDE
    je      .not
    cmp     cl, TOK_BANG
    je      .lnot
    cmp     cl, TOK_AT
    je      .at_query
    cmp     cl, TOK_LPAREN
    je      .paren
    cmp     cl, TOK_INT
    je      .int
    cmp     cl, TOK_CHAR
    je      .int
    cmp     cl, TOK_KW
    je      .kw
    cmp     cl, TOK_IDENT
    je      .ident
    jmp     parse_err_at_tok
.neg:
    inc     dword [tok_idx]
    call    eval_const_unary
    neg     rax
    ret
.not:
    inc     dword [tok_idx]
    call    eval_const_unary
    not     rax
    ret
.lnot:
    inc     dword [tok_idx]
    call    eval_const_unary
    test    rax, rax
    sete    al
    movzx   eax, al
    ret
.at_query:
    inc     dword [tok_idx]
    call    next_tok_ptr
    cmp     byte [rax], TOK_IDENT
    jne     parse_err_at_tok
    mov     ecx, [rax + 8]
    cmp     ecx, [sym_sizeof_id]
    je      .at_sz
    cmp     ecx, [sym_alignof_id]
    jne     parse_err_at_tok
    mov     edi, 1
    jmp     eval_type_query_intrinsic
.at_sz:
    xor     edi, edi
    jmp     eval_type_query_intrinsic
.paren:
    inc     dword [tok_idx]
    call    eval_const_expr
    push    rax
    mov     al, TOK_RPAREN
    call    expect_tok_kind
    pop     rax
    ret
.int:
    inc     dword [tok_idx]
    mov     rax, [rax + 8]
    ret
.kw:
    movzx   edx, byte [rax + 1]
    inc     dword [tok_idx]
    cmp     dl, KW_TRUE
    je      .ret1
    cmp     dl, KW_FALSE
    jne     parse_err_at_tok
    xor     eax, eax
    ret
.ret1:
    mov     eax, 1
    ret
.ident:
    mov     edi, [rax + 8]
    inc     dword [tok_idx]
    call    find_const
    test    eax, eax
    jz      .unresolved
    mov     rax, rdx
    ret
.unresolved:
    cmp     byte [const_trial_mode], 1
    jne     parse_err_cur_tok
    mov     byte [const_unresolved_hit], 1
    xor     eax, eax
    ret

; -----------------------------------------------------------------------------
; Bit-Pack Field Lookup & Extraction
; -----------------------------------------------------------------------------
find_type_field:
    mov     rcx, [space_ast]
    mov     edx, edi
    shl     edx, 5
    lea     r8, [rcx + AST_TYPE_OFF + rdx]
    movzx   r9d, byte [r8 + 5]
    mov     r10d, [r8 + 12]
    add     rcx, AST_FIELD_OFF
    xor     edx, edx
.loop:
    cmp     edx, r9d
    jae     .none
    mov     eax, r10d
    add     eax, edx
    shl     eax, 4
    cmp     [rcx + rax], esi
    je      .hit
    inc     edx
    jmp     .loop
.hit:
    lea     rax, [rcx + rax]
    ret
.none:
    xor     eax, eax
    ret

width_to_mask64:
    cmp     ecx, 64
    jae     .all_ones
    mov     rax, 1
    shl     rax, cl
    dec     rax
    ret
.all_ones:
    mov     rax, -1
    ret

emit_extract_bitfield:
    push    rbx
    mov     ebx, esi
    test    edi, edi
    jz      .mask_part
    push    rdi
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xE8C1
    call    emit_u16
    pop     rax
    call    emit_u8
.mask_part:
    cmp     ebx, 64
    jae     .done
    mov     ecx, ebx
    call    width_to_mask64
    call    emit_mov_rcx_imm64
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC821
    call    emit_u16
.done:
    mov     dword [last_expr_ty], TY_U64 ; bitpack fields are unsigned (§5.1)
    pop     rbx
    ret

; -----------------------------------------------------------------------------
; Runtime Expression Engine (Signedness-Exact + Div-Zero Guard + Short-Circuit)
; -----------------------------------------------------------------------------
binop_precedence:
    movzx   eax, al
    cmp     al, TOK_PIPE_GT
    mov     ecx, 1
    je      .ok
    cmp     al, TOK_OR2
    mov     ecx, 2
    je      .ok
    cmp     al, TOK_AND2
    mov     ecx, 3
    je      .ok
    cmp     al, TOK_PIPE
    mov     ecx, 4
    je      .ok
    cmp     al, TOK_CARET
    mov     ecx, 5
    je      .ok
    cmp     al, TOK_AMP
    mov     ecx, 6
    je      .ok
    cmp     al, TOK_EQ
    mov     ecx, 7
    je      .ok
    cmp     al, TOK_NE
    je      .ok
    cmp     al, TOK_LT
    je      .chk_lt_desc
    cmp     al, TOK_LE
    mov     ecx, 8
    je      .ok
    cmp     al, TOK_GT
    je      .ok
    cmp     al, TOK_GE
    je      .ok
    cmp     al, TOK_SHL
    mov     ecx, 9
    je      .ok
    cmp     al, TOK_SHR
    je      .ok
    cmp     al, TOK_ROL
    je      .ok
    cmp     al, TOK_ROR
    je      .ok
    cmp     al, TOK_PLUS
    mov     ecx, 10
    je      .ok
    cmp     al, TOK_MINUS
    je      .ok
    cmp     al, TOK_STAR
    mov     ecx, 11
    je      .ok
    cmp     al, TOK_SLASH
    je      .ok
    cmp     al, TOK_PERCENT
    je      .ok
    xor     ecx, ecx
.ok:
    ret
.chk_lt_desc:
    call    is_descriptor_lt
    test    eax, eax
    jnz     .not_binop
    mov     eax, TOK_LT
    mov     ecx, 8
    ret
.not_binop:
    xor     ecx, ecx
    ret

parse_expr:
    push    rbx
    push    r12
    push    r13
    push    r14
    push    r15
    mov     ebx, edi                ; min_prec

    call    parse_unary

.bin_loop:
    call    peek_tok_ptr
    movzx   eax, byte [rax]
    call    binop_precedence
    cmp     ecx, ebx
    jb      .done

    mov     r12d, eax               ; r12d = op token kind
    mov     r14d, [last_expr_ty]    ; r14d = LHS type tag!
    inc     dword [tok_idx]

    cmp     r12d, TOK_PIPE_GT
    je      .op_pipe_gt
    cmp     r12d, TOK_AND2
    je      .sc_and
    cmp     r12d, TOK_OR2
    je      .sc_or

    ; Precedence-safe immediate-RHS instruction selection (`+ - << >> & *pow2`)
    push    rcx
    call    try_emit_imm_rhs_binop
    pop     rcx
    test    eax, eax
    jnz     .bin_loop

    ; Save `lhs` into 16B-aligned frame temp slot
    push    rcx
    call    alloc_temp_qword
    mov     r13d, eax
    mov     edi, r13d
    call    emit_store_local
    pop     rcx

    lea     edi, [rcx + 1]
    call    parse_expr
    mov     r15d, [last_expr_ty]    ; r15d = RHS type tag!

    ; `mov rcx, rax`, then load `lhs` into `rax`
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC189
    call    emit_u16
    mov     edi, r13d
    call    emit_load_local
    mov     edi, 1
    call    free_temp_qwords

    ; Determine signedness of LHS (`edi`) and RHS (`esi`):
    ;   r14d = 1 if LHS unsigned else 0
    ;   r15d = 1 if RHS unsigned else 0
    mov     edi, r14d
    call    is_ty_unsigned
    mov     r14d, eax
    mov     edi, r15d
    call    is_ty_unsigned
    mov     r15d, eax

    ; Default binary result type: `TY_U64` if both unsigned, else `TY_I64`
    mov     eax, r14d
    and     eax, r15d
    mov     edx, TY_I64
    test    eax, eax
    jz      .set_def_res
    mov     edx, TY_U64
.set_def_res:
    mov     [last_expr_ty], edx

    cmp     r12d, TOK_PLUS
    je      .op_add
    cmp     r12d, TOK_MINUS
    je      .op_sub
    cmp     r12d, TOK_STAR
    je      .op_mul
    cmp     r12d, TOK_SLASH
    je      .op_div
    cmp     r12d, TOK_PERCENT
    je      .op_mod
    cmp     r12d, TOK_AMP
    je      .op_and
    cmp     r12d, TOK_PIPE
    je      .op_or
    cmp     r12d, TOK_CARET
    je      .op_xor
    cmp     r12d, TOK_SHL
    je      .op_shl
    cmp     r12d, TOK_SHR
    je      .op_shr
    cmp     r12d, TOK_ROL
    je      .op_rol
    cmp     r12d, TOK_ROR
    je      .op_ror
    cmp     r12d, TOK_EQ
    mov     dl, 0x94                ; sete
    je      .op_cmp_common
    cmp     r12d, TOK_NE
    mov     dl, 0x95                ; setne
    je      .op_cmp_common

    ; Relational `< <= > >=`: unsigned iff BOTH `r14d == 1 && r15d == 1` (§5.1)
    mov     eax, r14d
    and     eax, r15d
    test    eax, eax
    jnz     .cmp_unsigned

    cmp     r12d, TOK_LT
    mov     dl, 0x9C                ; setl
    je      .op_cmp_common
    cmp     r12d, TOK_LE
    mov     dl, 0x9E                ; setle
    je      .op_cmp_common
    cmp     r12d, TOK_GT
    mov     dl, 0x9F                ; setg
    je      .op_cmp_common
    mov     dl, 0x9D                ; setge
    jmp     .op_cmp_common

.cmp_unsigned:
    cmp     r12d, TOK_LT
    mov     dl, 0x92                ; setb
    je      .op_cmp_common
    cmp     r12d, TOK_LE
    mov     dl, 0x96                ; setbe
    je      .op_cmp_common
    cmp     r12d, TOK_GT
    mov     dl, 0x97                ; seta
    je      .op_cmp_common
    mov     dl, 0x93                ; setae
    jmp     .op_cmp_common

.sc_and:
    push    rcx
    call    emit_jz_rel32
    mov     r13d, eax
    pop     rcx
    lea     edi, [rcx + 1]
    call    parse_expr
    lea     rsi, [seq_bool_norm]
    mov     ecx, seq_bool_norm_end - seq_bool_norm
    call    emit_bytes
    call    emit_jmp_rel32
    push    rax
    mov     edi, r13d
    call    patch_rel32_here
    mov     ax, 0xC031
    call    emit_u16
    pop     rdi
    call    patch_rel32_here
    mov     dword [last_expr_ty], TY_I64
    jmp     .bin_loop

.sc_or:
    push    rcx
    call    emit_jnz_rel32
    mov     r13d, eax
    pop     rcx
    lea     edi, [rcx + 1]
    call    parse_expr
    lea     rsi, [seq_bool_norm]
    mov     ecx, seq_bool_norm_end - seq_bool_norm
    call    emit_bytes
    call    emit_jmp_rel32
    push    rax
    mov     edi, r13d
    call    patch_rel32_here
    mov     al, 0xB8
    call    emit_u8
    mov     eax, 1
    call    emit_u32
    pop     rdi
    call    patch_rel32_here
    mov     dword [last_expr_ty], TY_I64
    jmp     .bin_loop

.op_add:
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC801
    call    emit_u16
    jmp     .bin_loop
.op_sub:
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC829
    call    emit_u16
    jmp     .bin_loop
.op_mul:
    mov     eax, 0xC1AF0F48
    call    emit_u32
    jmp     .bin_loop

.op_div:
    ; Result inherits LHS signedness; `rcx == 0` -> `fail 1`!
    mov     edx, TY_I64
    test    r14d, r14d
    jz      .div_ty_set
    mov     edx, TY_U64
.div_ty_set:
    mov     [last_expr_ty], edx
    call    emit_div_zero_check_rcx
    test    r14d, r14d
    jnz     .udiv
    mov     ax, 0x9948              ; cqo
    call    emit_u16
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xF9F7              ; idiv rcx
    call    emit_u16
    jmp     .bin_loop
.udiv:
    mov     ax, 0xD231              ; 31 D2 = xor edx, edx
    call    emit_u16
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xF1F7              ; 48 F7 F1 = div rcx
    call    emit_u16
    jmp     .bin_loop

.op_mod:
    mov     edx, TY_I64
    test    r14d, r14d
    jz      .mod_ty_set
    mov     edx, TY_U64
.mod_ty_set:
    mov     [last_expr_ty], edx
    call    emit_div_zero_check_rcx
    test    r14d, r14d
    jnz     .umod
    mov     ax, 0x9948              ; cqo
    call    emit_u16
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xF9F7              ; idiv rcx
    call    emit_u16
    jmp     .mod_finish
.umod:
    mov     ax, 0xD231              ; xor edx, edx
    call    emit_u16
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xF1F7              ; div rcx
    call    emit_u16
.mod_finish:
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xD089              ; mov rax, rdx
    call    emit_u16
    jmp     .bin_loop

.op_and:
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC821
    call    emit_u16
    jmp     .bin_loop
.op_or:
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC809
    call    emit_u16
    jmp     .bin_loop
.op_xor:
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC831
    call    emit_u16
    jmp     .bin_loop

.op_shl:
    ; Shifts inherit LHS signedness (§5.1)
    mov     edx, TY_I64
    test    r14d, r14d
    jz      .shl_ty
    mov     edx, TY_U64
.shl_ty:
    mov     [last_expr_ty], edx
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xE0D3              ; 48 D3 E0 = shl rax, cl
    call    emit_u16
    jmp     .bin_loop

.op_shr:
    mov     edx, TY_I64
    test    r14d, r14d
    jz      .shr_signed
    mov     dword [last_expr_ty], TY_U64
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xE8D3              ; 48 D3 E8 = shr rax, cl (UNSIGNED!)
    call    emit_u16
    jmp     .bin_loop
.shr_signed:
    mov     dword [last_expr_ty], TY_I64
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xF8D3              ; 48 D3 F8 = sar rax, cl (SIGNED!)
    call    emit_u16
    jmp     .bin_loop

.op_rol:
    mov     dword [last_expr_ty], TY_U64
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC0D3              ; 48 D3 C0 = rol rax, cl
    call    emit_u16
    jmp     .bin_loop

.op_ror:
    mov     dword [last_expr_ty], TY_U64
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC8D3              ; 48 D3 C8 = ror rax, cl
    call    emit_u16
    jmp     .bin_loop

.op_pipe_gt:
    ; Pipeline `lhs |> fn_name[(arg1, ...)]`: passes `lhs` as `arg0` (`rdi`)!
    sub     rsp, 32
    call    alloc_temp_qword
    mov     [rsp + 0], eax          ; arg0 = lhs
    mov     edi, eax
    call    emit_store_local
    mov     r15d, 1                 ; arg_cnt = 1

    call    next_tok_ptr
    cmp     byte [rax], TOK_IDENT
    jne     parse_err_at_tok
    mov     r13d, [rax + 8]         ; target fn sym_id

    call    peek_tok_ptr
    cmp     byte [rax], TOK_LPAREN
    jne     .pg_load_args
    inc     dword [tok_idx]         ; consume `(`
    call    peek_tok_ptr
    cmp     byte [rax], TOK_RPAREN
    je      .pg_close_p

.pg_arg_lp:
    cmp     r15d, 6
    jae     parse_err_cur_tok
    mov     edi, 2                  ; prec > `|>`
    call    parse_expr
    call    alloc_temp_qword
    mov     [rsp + r15 * 4], eax
    mov     edi, eax
    call    emit_store_local
    inc     r15d
    call    peek_tok_ptr
    cmp     byte [rax], TOK_COMMA
    jne     .pg_close_p
    inc     dword [tok_idx]
    jmp     .pg_arg_lp

.pg_close_p:
    mov     al, TOK_RPAREN
    call    expect_tok_kind

.pg_load_args:
    mov     edi, [rsp + 0]
    call    emit_load_local
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC789              ; mov rdi, rax
    call    emit_u16
    cmp     r15d, 2
    jb      .pg_call
    mov     edi, [rsp + 4]
    call    emit_load_local
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC689              ; mov rsi, rax
    call    emit_u16
    cmp     r15d, 3
    jb      .pg_call
    mov     edi, [rsp + 8]
    call    emit_load_local
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC289              ; mov rdx, rax
    call    emit_u16
    cmp     r15d, 4
    jb      .pg_call
    mov     edi, [rsp + 12]
    call    emit_load_local
    call    emit_move_rax_to_arg3
    cmp     r15d, 5
    jb      .pg_call
    mov     edi, [rsp + 16]
    call    emit_load_local
    mov     al, 0x49
    call    emit_u8
    mov     ax, 0xC089              ; mov r8, rax
    call    emit_u16
    cmp     r15d, 6
    jb      .pg_call
    mov     edi, [rsp + 20]
    call    emit_load_local
    mov     al, 0x49
    call    emit_u8
    mov     ax, 0xC189              ; mov r9, rax
    call    emit_u16

.pg_call:
    mov     edi, r15d
    call    free_temp_qwords
    add     rsp, 32
    mov     edi, r13d
    call    find_or_add_fn
    mov     edi, eax
    call    emit_call_fn_idx
    jmp     .bin_loop

.op_cmp_common:
    mov     dword [last_expr_ty], TY_I64 ; comparisons always yield i64 0 or 1
    push    rdx
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC839
    call    emit_u16
    mov     al, 0x0F
    call    emit_u8
    pop     rax
    call    emit_u8
    mov     al, 0xC0
    call    emit_u8
    mov     al, 0x0F
    call    emit_u8
    mov     ax, 0xC0B6
    call    emit_u16
    jmp     .bin_loop

.done:
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbx
    ret

seq_bool_norm:
    test    rax, rax
    setne   al
    movzx   eax, al
seq_bool_norm_end:

; -----------------------------------------------------------------------------
; Unary, Primary & Postfix
; -----------------------------------------------------------------------------
parse_unary:
    call    parse_primary_or_prefix
.postfix_loop:
    call    try_parse_postfix_bitslice
    mov     eax, [last_expr_ty]
    test    eax, TY_FLAG_SPAN
    jnz     .chk_span_post
    test    eax, TY_FLAG_ATOMIC
    jnz     .chk_atomic_post
    test    eax, TY_FLAG_VIEW_ONE
    jnz     .chk_view1_dot
    and     eax, TY_BASE_MASK
    cmp     eax, TY_CUSTOM_PTR_BASE
    jae     .chk_cptr_post
    cmp     eax, TY_STRUCT_BASE
    jae     .chk_struct_dot
    cmp     eax, TY_BITPACK_BASE
    jb      .done
    call    peek_tok_ptr
    cmp     byte [rax], TOK_DOT
    jne     .done
    inc     dword [tok_idx]
    call    next_tok_ptr
    cmp     byte [rax], TOK_IDENT
    jne     parse_err_at_tok
    mov     esi, [rax + 8]
    mov     edi, [last_expr_ty]
    and     edi, TY_BASE_MASK
    sub     edi, TY_BITPACK_BASE
    call    find_type_field
    test    rax, rax
    jz      parse_err_cur_tok
    mov     edi, [rax + 4]
    mov     esi, [rax + 8]
    call    emit_extract_bitfield
    jmp     .postfix_loop

.chk_span_post:
    call    peek_tok_ptr
    cmp     byte [rax], TOK_DOT
    je      .do_span_post
    cmp     byte [rax], TOK_LBRACK
    jne     .done
.do_span_post:
    push    r12
    push    r13
    push    r14
    mov     r13d, [last_expr_ty]
    mov     al, 0x49
    call    emit_u8
    mov     ax, 0xC089              ; mov r8, rax
    call    emit_u16
    call    alloc_anon_span_slot
    mov     r12d, eax
    mov     al, 0x49
    call    emit_u8
    mov     ax, 0x008B              ; mov rax, [r8]
    call    emit_u16
    mov     edi, r12d
    call    emit_store_local
    mov     eax, 0x08408B49         ; mov rax, [r8 + 8]
    call    emit_u32
    lea     edi, [r12d + 8]
    call    emit_store_local
    call    parse_span_local_rval
    pop     r14
    pop     r13
    pop     r12
    jmp     .postfix_loop

.chk_atomic_post:
    call    peek_tok_ptr
    cmp     byte [rax], TOK_DOT
    jne     .done
    push    r12
    push    r13
    mov     r13d, [last_expr_ty]
    mov     edi, 1
    call    alloc_anon_frame_qwords
    mov     r12d, eax
    mov     edi, r12d
    call    emit_store_local
    call    parse_atomic_rval_or_stmt
    pop     r13
    pop     r12
    jmp     .postfix_loop

.chk_cptr_post:
    call    peek_tok_ptr
    cmp     byte [rax], TOK_DOT
    je      .do_cptr_post
    cmp     byte [rax], TOK_LBRACK
    jne     .done
.do_cptr_post:
    push    r12
    push    r13
    mov     r13d, [last_expr_ty]
    mov     edi, 1
    call    alloc_anon_frame_qwords
    mov     r12d, eax
    mov     edi, r12d
    call    emit_store_local
    call    parse_custom_ptr_rval
    pop     r13
    pop     r12
    jmp     .postfix_loop
.chk_view1_dot:
    call    peek_tok_ptr
    cmp     byte [rax], TOK_DOT
    jne     .done
    mov     edi, [last_expr_ty]
    and     edi, TY_BASE_MASK
    cmp     edi, TY_STRUCT_BASE
    jb      .done
    push    rdi
    test    dword [last_expr_ty], TY_FLAG_MMIO
    jnz     .v1_walk
    call    emit_null_view_check_rax
.v1_walk:
    pop     rdi
    call    walk_struct_fields_rval
    jmp     .postfix_loop
.chk_struct_dot:
    call    peek_tok_ptr
    cmp     byte [rax], TOK_DOT
    jne     .done
    mov     edi, [last_expr_ty]
    and     edi, TY_BASE_MASK
    call    walk_struct_fields_rval
    jmp     .postfix_loop
.done:
    ret

parse_primary_or_prefix:
    mov     dword [last_expr_ty], TY_I64
    call    peek_tok_ptr
    movzx   ecx, byte [rax]
    cmp     cl, TOK_MINUS
    je      .u_neg
    cmp     cl, TOK_BANG
    je      .u_not
    cmp     cl, TOK_TILDE
    je      .u_bitnot
    cmp     cl, TOK_LPAREN
    je      .u_paren
    cmp     cl, TOK_INT
    je      .u_int
    cmp     cl, TOK_CHAR
    je      .u_int
    cmp     cl, TOK_STR
    je      emit_str_literal_span
    cmp     cl, TOK_KW
    je      .u_kw
    cmp     cl, TOK_AT
    je      parse_at_intrinsic
    cmp     cl, TOK_IDENT
    je      parse_ident_rvalue
    jmp     parse_err_at_tok

.u_neg:
    inc     dword [tok_idx]
    call    parse_unary
    mov     dword [last_expr_ty], TY_I64
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xD8F7
    jmp     emit_u16
.u_bitnot:
    inc     dword [tok_idx]
    call    parse_unary
    ; Preserve signedness across `~`
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xD0F7
    jmp     emit_u16
.u_not:
    inc     dword [tok_idx]
    call    parse_unary
    mov     dword [last_expr_ty], TY_I64
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC085
    call    emit_u16
    mov     al, 0x0F
    call    emit_u8
    mov     ax, 0xC094
    call    emit_u16
    mov     al, 0x0F
    call    emit_u8
    mov     ax, 0xC0B6
    jmp     emit_u16
.u_paren:
    inc     dword [tok_idx]
    mov     edi, 1
    call    parse_expr
    mov     al, TOK_RPAREN
    jmp     expect_tok_kind
.u_int:
    mov     rax, [rax + 8]
    inc     dword [tok_idx]
    mov     dword [last_expr_ty], TY_I64 ; integer literals are signed i64 (§5.1)
    jmp     emit_mov_rax_imm64
.u_kw:
    movzx   edx, byte [rax + 1]
    inc     dword [tok_idx]
    mov     dword [last_expr_ty], TY_U8  ; `bool` (`true`/`false`) is unsigned (§5.1)
    cmp     dl, KW_TRUE
    je      .kw_t
    cmp     dl, KW_FALSE
    jne     parse_err_cur_tok
    xor     eax, eax
    jmp     emit_mov_rax_imm64
.kw_t:
    mov     eax, 1
    jmp     emit_mov_rax_imm64

; -----------------------------------------------------------------------------
; `@syscall(nr, a0..a5)` & `@rsp_align()`
; -----------------------------------------------------------------------------
parse_at_intrinsic:
    push    rbx
    push    r12
    push    r13
    sub     rsp, 64
    inc     dword [tok_idx]

    call    next_tok_ptr
    cmp     byte [rax], TOK_KW
    je      .chk_sys_kw
    cmp     byte [rax], TOK_IDENT
    jne     parse_err_at_tok
    add     rsp, 64
    pop     r13
    pop     r12
    pop     rbx
    jmp     parse_efi_intrinsic

.chk_sys_kw:
    cmp     byte [rax + 1], KW_MMIO
    jne     .chk_real_sys
    cmp     byte [mode_efi], 1
    jne     .err_need_efi_kw
    add     rsp, 64
    pop     r13
    pop     r12
    pop     rbx
    mov     edi, 1
    jmp     parse_view_or_mmio_call
.err_need_efi_kw:
    mov     esi, [rax + 4]
    lea     rdi, [msg_need_efi]
    mov     edx, msg_need_efi_len
    jmp     err_msg_at_line
.chk_real_sys:
    cmp     byte [rax + 1], KW_SYSCALL
    jne     parse_err_at_tok
    cmp     byte [mode_efi], 1
    je      .err_no_sys_efi
    mov     al, TOK_LPAREN
    call    expect_tok_kind
    jmp     .sys_args_start
.err_no_sys_efi:
    mov     esi, [rax + 4]
    lea     rdi, [msg_no_host_in_efi]
    mov     edx, msg_no_host_in_efi_len
    jmp     err_msg_at_line
.sys_args_start:

    xor     r12d, r12d
.sys_arg_loop:
    cmp     r12d, 7
    jae     parse_err_cur_tok
    mov     edi, 1
    call    parse_expr
    call    alloc_temp_qword
    mov     [rsp + r12 * 4], eax
    mov     edi, eax
    call    emit_store_local
    inc     r12d

    call    peek_tok_ptr
    cmp     byte [rax], TOK_COMMA
    jne     .sys_args_done
    inc     dword [tok_idx]
    jmp     .sys_arg_loop

.sys_args_done:
    mov     al, TOK_RPAREN
    call    expect_tok_kind

    cmp     r12d, 2
    jb      .load_nr
    mov     edi, [rsp + 4]
    call    emit_load_local
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC789
    call    emit_u16
    cmp     r12d, 3
    jb      .load_nr
    mov     edi, [rsp + 8]
    call    emit_load_local
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC689
    call    emit_u16
    cmp     r12d, 4
    jb      .load_nr
    mov     edi, [rsp + 12]
    call    emit_load_local
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC289
    call    emit_u16
    cmp     r12d, 5
    jb      .load_nr
    mov     edi, [rsp + 16]
    call    emit_load_local
    mov     al, 0x49
    call    emit_u8
    mov     ax, 0xC289
    call    emit_u16
    cmp     r12d, 6
    jb      .load_nr
    mov     edi, [rsp + 20]
    call    emit_load_local
    mov     al, 0x49
    call    emit_u8
    mov     ax, 0xC089
    call    emit_u16
    cmp     r12d, 7
    jb      .load_nr
    mov     edi, [rsp + 24]
    call    emit_load_local
    mov     al, 0x49
    call    emit_u8
    mov     ax, 0xC189
    call    emit_u16

.load_nr:
    mov     edi, [rsp + 0]
    call    emit_load_local
    mov     edi, r12d
    call    free_temp_qwords
    mov     ax, 0x050F
    call    emit_u16
    mov     dword [last_expr_ty], TY_I64

.done:
    add     rsp, 64
    pop     r13
    pop     r12
    pop     rbx
    ret

; -----------------------------------------------------------------------------
; Rvalue Identifier Resolution (With `@slab`/`@arena` Bounds Check `fail 2`)
; -----------------------------------------------------------------------------
parse_ident_rvalue:
    push    rbx
    push    r12
    push    r13
    push    r14

    call    next_tok_ptr
    mov     ebx, [rax + 8]

    call    peek_tok_ptr
    cmp     byte [rax], TOK_LPAREN
    je      .do_fn_call

    mov     edi, ebx
    call    find_user_type
    cmp     eax, -1
    je      .chk_space
    mov     r12d, eax
    shl     eax, 5
    mov     rcx, [space_ast]
    cmp     byte [rcx + AST_TYPE_OFF + rax + 4], UKIND_STRUCT
    je      .do_struct_lit
    call    peek_tok_ptr
    cmp     byte [rax], TOK_LT
    jne     parse_err_at_tok
    call    parse_bitpack_literal
    jmp     .done
.do_struct_lit:
    call    peek_tok_ptr
    cmp     byte [rax], TOK_LBRACE
    jne     parse_err_at_tok
    call    parse_struct_literal
    jmp     .done

.chk_space:
    mov     edi, ebx
    call    find_space
    test    rax, rax
    jnz     .do_space_rval

    mov     edi, ebx
    call    find_local_entry
    test    eax, eax
    jnz     .do_local_rval

    mov     edi, ebx
    call    find_const
    test    eax, eax
    jz      parse_err_cur_tok
    mov     rax, rdx
    call    emit_mov_rax_imm64
    mov     dword [last_expr_ty], TY_I64
    jmp     .done

.do_local_rval:
    mov     r12d, eax               ; rbp_disp
    mov     r13d, edx               ; ty_tag
    test    r13d, TY_FLAG_SPAN
    jz      .chk_local_atomic
    call    parse_span_local_rval
    jmp     .done
.chk_local_atomic:
    test    r13d, TY_FLAG_ATOMIC
    jz      .chk_local_view1
    call    parse_atomic_rval_or_stmt
    jmp     .done
.chk_local_view1:
    test    r13d, TY_FLAG_VIEW_ONE
    jz      .chk_local_cptr
    call    parse_view1_local_rval
    jmp     .done
.chk_local_cptr:
    mov     eax, r13d
    and     eax, TY_BASE_MASK
    cmp     eax, TY_CUSTOM_PTR_BASE
    jb      .chk_local_struct
    call    parse_custom_ptr_rval
    jmp     .done
.chk_local_struct:
    cmp     r13d, TY_STRUCT_BASE
    jae     .local_struct_addr
    ; Width-exact load from `[rbp + disp]` (`movzx`/`movsx`/`mov`)!
    mov     edi, r12d
    call    emit_lea_local
    mov     edi, r13d
    call    emit_load_mem_typed
    mov     [last_expr_ty], r13d
    jmp     .done

.local_struct_addr:
    mov     edi, r12d
    call    emit_lea_local
    mov     edi, r13d
    call    walk_struct_fields_rval
    jmp     .done

.do_space_rval:
    mov     r14, rax                ; &SpaceEntry
    cmp     dword [r14 + 4], SKIND_PORT
    jne     .sp_rval_mem
    call    parse_port_rval
    jmp     .done
.sp_rval_mem:
    call    peek_tok_ptr
    cmp     byte [rax], TOK_DOT
    je      .space_prop

    mov     al, TOK_LBRACK
    call    expect_tok_kind
    mov     edi, 1
    call    parse_expr              ; `rax` = index or `lo`
    call    peek_tok_ptr
    cmp     byte [rax], TOK_DOT2
    je      .sp_subslice
    mov     al, TOK_RBRACK
    call    expect_tok_kind

    ; `@ring` / `@dma_queue` wrap power-of-2 (`idx & (cap - 1)`); `@slab` / `@arena` check `idx < cap` (`fail 2`)
    mov     rax, [r14 + 24]
    cmp     dword [r14 + 4], SKIND_RING
    je      .sp_wrap_pow2
    cmp     dword [r14 + 4], SKIND_DMA_QUEUE
    je      .sp_wrap_pow2
    call    emit_bounds_check_rax
    jmp     .sp_mul_esize
.sp_wrap_pow2:
    dec     rax
    call    emit_mov_rcx_imm64
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC821              ; 48 21 C8 = and rax, rcx
    call    emit_u16
.sp_mul_esize:
    mov     eax, [r14 + 16]
    cmp     eax, 1
    jbe     .sp_add_base
    call    emit_mov_rcx_imm64
    mov     eax, 0xC1AF0F48
    call    emit_u32
.sp_add_base:
    mov     rax, [r14 + 8]
    call    emit_load_space_base_rcx
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC801
    call    emit_u16

    mov     edi, [r14 + 20]
    cmp     edi, TY_STRUCT_BASE
    jae     .sp_struct
    mov     [last_expr_ty], edi
    call    emit_load_mem_typed
    jmp     .done
.sp_struct:
    call    walk_struct_fields_rval
    jmp     .done

.space_prop:
    inc     dword [tok_idx]
    call    next_tok_ptr
    cmp     byte [rax], TOK_IDENT
    jne     parse_err_at_tok
    mov     ecx, [rax + 8]
    cmp     ecx, [sym_base_id]
    je      .ret_sp_base
    cmp     ecx, [sym_max_id]
    je      .ret_sp_cap
    cmp     ecx, [sym_cap_id]
    je      .ret_sp_cap
    cmp     ecx, [sym_len_id]
    je      .ret_sp_cap
    cmp     ecx, [sym_span_id]
    je      .ret_sp_span
    cmp     ecx, [sym_slot_addr_id]
    je      .ret_sp_slot_addr
    jmp     parse_err_cur_tok
.ret_sp_slot_addr:
    ; `ring.slot_addr(seq)` -> `ring.base + ((seq & (cap - 1)) * esize)`!
    cmp     dword [r14 + 4], SKIND_RING
    je      .sa_go
    cmp     dword [r14 + 4], SKIND_DMA_QUEUE
    jne     parse_err_cur_tok
.sa_go:
    mov     al, TOK_LPAREN
    call    expect_tok_kind
    mov     edi, 1
    call    parse_expr              ; seq -> `rax`
    mov     al, TOK_RPAREN
    call    expect_tok_kind
    mov     rax, [r14 + 24]
    dec     rax
    call    emit_mov_rcx_imm64
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC821              ; and rax, rcx (`seq & (cap - 1)`)
    call    emit_u16
    mov     eax, [r14 + 16]
    call    emit_mov_rcx_imm64
    mov     eax, 0xC1AF0F48         ; imul rax, rcx
    call    emit_u32
    mov     rax, [r14 + 8]
    call    emit_load_space_base_rcx
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC801              ; add rax, rcx
    call    emit_u16
    mov     dword [last_expr_ty], TY_U64
    jmp     .done
.ret_sp_span:
    call    alloc_anon_span_slot
    mov     r12d, eax
    mov     rax, [r14 + 8]
    call    emit_load_space_base_rax
    mov     edi, r12d
    call    emit_store_local
    mov     rax, [r14 + 24]
    call    emit_mov_rax_imm64
    lea     edi, [r12d + 8]
    call    emit_store_local
    mov     edi, r12d
    call    emit_lea_local
    mov     eax, [r14 + 20]
    or      eax, TY_FLAG_SPAN
    mov     [last_expr_ty], eax
    jmp     .done
.sp_subslice:
    ; `slab[lo..hi]` -> construct full span & subslice!
    inc     dword [tok_idx]         ; consume `..`
    call    alloc_temp_qword
    mov     r12d, eax               ; temp `lo`
    mov     edi, r12d
    call    emit_store_local
    mov     edi, 1
    call    parse_expr              ; `hi` -> `rax`
    mov     al, TOK_RBRACK
    call    expect_tok_kind
    call    alloc_temp_qword
    mov     r13d, eax               ; temp `hi`
    mov     edi, r13d
    call    emit_store_local

    ; Bounds check `(u64)lo <= (u64)hi && (u64)hi <= slab.cap` -> `fail 2`
    mov     edi, r12d
    call    emit_load_local
    mov     edi, r13d
    call    emit_load_local_rcx
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC839              ; cmp rax, rcx (lo vs hi)
    call    emit_u16
    mov     ax, 0x870F              ; ja .sp_ss_oob
    call    emit_u16
    mov     eax, [code_off]
    push    rax
    xor     eax, eax
    call    emit_u32

    mov     rax, [r14 + 24]
    call    emit_mov_rax_imm64      ; rax = cap
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC139              ; cmp rcx, rax (hi vs cap)
    call    emit_u16
    mov     ax, 0x860F              ; jbe .sp_ss_ok
    call    emit_u16
    mov     eax, [code_off]
    push    rax
    xor     eax, eax
    call    emit_u32

    mov     rdx, [rsp + 8]
    mov     edi, edx
    call    patch_rel32_here
    mov     edi, 2
    call    emit_raise_fault_imm

    pop     rdi
    call    patch_rel32_here
    add     rsp, 8

    call    alloc_anon_span_slot
    mov     ebx, eax                ; new span slot
    mov     edi, r13d
    call    emit_load_local
    mov     edi, r12d
    call    emit_load_local_rcx
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC829              ; hi - lo
    call    emit_u16
    lea     edi, [ebx + 8]
    call    emit_store_local

    mov     edi, r12d
    call    emit_load_local
    mov     ecx, [r14 + 16]         ; elem_size
    cmp     ecx, 1
    jbe     .sp_ss_b
    push    rax
    mov     eax, ecx
    call    emit_mov_rcx_imm64
    pop     rax
    mov     eax, 0xC1AF0F48
    call    emit_u32
.sp_ss_b:
    mov     rax, [r14 + 8]
    call    emit_load_space_base_rcx
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC801
    call    emit_u16
    mov     edi, ebx
    call    emit_store_local

    mov     edi, 2
    call    free_temp_qwords
    mov     edi, ebx
    call    emit_lea_local
    mov     eax, [r14 + 20]
    or      eax, TY_FLAG_SPAN
    mov     [last_expr_ty], eax
    jmp     .done
.ret_sp_base:
    mov     rax, [r14 + 8]
    call    emit_load_space_base_rax
    mov     dword [last_expr_ty], TY_U64
    jmp     .done
.ret_sp_cap:
    mov     rax, [r14 + 24]
    call    emit_mov_rax_imm64
    mov     dword [last_expr_ty], TY_U64
    jmp     .done

.do_fn_call:
    cmp     ebx, [sym_publish_id]
    je      .rval_publish
    cmp     ebx, [sym_wait_id]
    je      .rval_wait
    sub     rsp, 32
    inc     dword [tok_idx]
    xor     r12d, r12d
    jmp     .fc_chk_empty
.rval_publish:
    dec     dword [tok_idx]
    call    parse_stmt_or_expr_publish
    jmp     .done
.rval_wait:
    dec     dword [tok_idx]
    call    parse_stmt_or_expr_wait
    jmp     .done
.fc_chk_empty:
    call    peek_tok_ptr
    cmp     byte [rax], TOK_RPAREN
    je      .fc_args_done

.fc_arg_loop:
    cmp     r12d, 6
    jae     parse_err_cur_tok
    mov     edi, 1
    call    parse_expr
    call    alloc_temp_qword
    mov     [rsp + r12 * 4], eax
    mov     edi, eax
    call    emit_store_local
    inc     r12d

    call    peek_tok_ptr
    cmp     byte [rax], TOK_COMMA
    jne     .fc_args_done
    inc     dword [tok_idx]
    jmp     .fc_arg_loop

.fc_args_done:
    mov     al, TOK_RPAREN
    call    expect_tok_kind

    cmp     r12d, 1
    jb      .fc_emit
    mov     edi, [rsp + 0]
    call    emit_load_local
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC789
    call    emit_u16
    cmp     r12d, 2
    jb      .fc_emit
    mov     edi, [rsp + 4]
    call    emit_load_local
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC689
    call    emit_u16
    cmp     r12d, 3
    jb      .fc_emit
    mov     edi, [rsp + 8]
    call    emit_load_local
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC289
    call    emit_u16
    cmp     r12d, 4
    jb      .fc_emit
    mov     edi, [rsp + 12]
    call    emit_load_local
    call    emit_move_rax_to_arg3
    cmp     r12d, 5
    jb      .fc_emit
    mov     edi, [rsp + 16]
    call    emit_load_local
    mov     al, 0x49
    call    emit_u8
    mov     ax, 0xC089
    call    emit_u16
    cmp     r12d, 6
    jb      .fc_emit
    mov     edi, [rsp + 20]
    call    emit_load_local
    mov     al, 0x49
    call    emit_u8
    mov     ax, 0xC189
    call    emit_u16

.fc_emit:
    mov     edi, r12d
    call    free_temp_qwords
    add     rsp, 32

    mov     edi, ebx
    call    find_or_add_fn
    mov     edi, eax
    call    emit_call_fn_idx

.done:
    pop     r14
    pop     r13
    pop     r12
    pop     rbx
    ret

walk_struct_fields_rval:
    push    rbx
    mov     ebx, edi
.loop:
    cmp     ebx, TY_STRUCT_BASE
    jb      .done_scalar
    call    peek_tok_ptr
    cmp     byte [rax], TOK_DOT
    jne     .ret_whole_struct
    inc     dword [tok_idx]         ; consume `.`
    call    next_tok_ptr
    cmp     byte [rax], TOK_IDENT
    jne     parse_err_at_tok
    mov     esi, [rax + 8]
    mov     edi, ebx
    sub     edi, TY_STRUCT_BASE
    call    find_type_field
    test    rax, rax
    jz      parse_err_cur_tok

    mov     ecx, [rax + 4]
    mov     ebx, [rax + 12]
    test    ecx, ecx
    jz      .chk_nested
    push    rcx
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC081
    call    emit_u16
    pop     rax
    call    emit_u32
.chk_nested:
    jmp     .loop
.ret_whole_struct:
    mov     [last_expr_ty], ebx
    pop     rbx
    ret
.done_scalar:
    mov     [last_expr_ty], ebx
    mov     edi, ebx
    call    emit_load_mem_typed
    pop     rbx
    ret

parse_bitpack_literal:
    push    rbx
    push    r13
    push    r14

    call    alloc_temp_qword
    mov     r13d, eax
    xor     eax, eax
    call    emit_mov_rax_imm64
    mov     edi, r13d
    call    emit_store_local

    inc     dword [tok_idx]         ; consume `<`
.fld_loop:
    call    next_tok_ptr
    cmp     byte [rax], TOK_IDENT
    jne     parse_err_at_tok
    mov     esi, [rax + 8]
    mov     edi, r12d
    call    find_type_field
    test    rax, rax
    jz      parse_err_cur_tok
    mov     r14, rax

    mov     al, TOK_COLON
    call    expect_tok_kind

    call    parse_bitpack_clause_expr

    mov     ecx, [r14 + 8]
    cmp     ecx, 64
    jae     .do_shift
    push    rax
    call    width_to_mask64
    mov     rbx, rax
    pop     rax
    mov     rax, rbx
    call    emit_mov_rcx_imm64
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC821
    call    emit_u16
.do_shift:
    mov     ecx, [r14 + 4]
    test    ecx, ecx
    jz      .or_acc
    push    rcx
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xE0C1
    call    emit_u16
    pop     rax
    call    emit_u8
.or_acc:
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0x8509
    call    emit_u16
    mov     eax, r13d
    call    emit_u32

    call    peek_tok_ptr
    cmp     byte [rax], TOK_PIPE
    jne     .close_bp
    inc     dword [tok_idx]
    jmp     .fld_loop

.close_bp:
    mov     al, TOK_GT
    call    expect_tok_kind
    mov     edi, r13d
    call    emit_load_local
    mov     edi, 1
    call    free_temp_qwords

    lea     eax, [r12 + TY_BITPACK_BASE]
    mov     [last_expr_ty], eax

    pop     r14
    pop     r13
    pop     rbx
    ret

; -----------------------------------------------------------------------------
; Lexically Scoped Blocks (`parse_block` Rewinds `local_cnt` on `}`)
; -----------------------------------------------------------------------------
; Parses `{ ... }` incrementing `scope_depth` and rewinding `local_cnt` at `}`!
parse_block:
    mov     eax, [local_cnt]
    push    rax
    inc     dword [scope_depth]

    call    parse_block_no_scope_push

    dec     dword [scope_depth]
    pop     rax
    mov     [local_cnt], eax
    ret

; Used by `parse_fn_decl` so function body shares scope_depth=1 with parameters
parse_block_no_scope_push:
    mov     al, TOK_LBRACE
    call    expect_tok_kind
.loop:
    call    peek_tok_ptr
    movzx   ecx, byte [rax]
    cmp     cl, TOK_RBRACE
    je      .done
    cmp     cl, TOK_EOF
    je      parse_err_at_tok
    call    parse_stmt
    jmp     .loop
.done:
    inc     dword [tok_idx]
    ret

parse_guarded_block_stmt:
    push    rbx
    call    block_has_trailing_recover
    mov     ebx, eax
    test    ebx, ebx
    jz      .plain
    call    rec_push_scope
    call    parse_block
    call    parse_trailing_recover_body
    pop     rbx
    ret
.plain:
    call    parse_block
    pop     rbx
    ret

; Binds `err` strictly inside the `recover` block's own scope (§5.2)
parse_trailing_recover_body:
    push    rbx
    mov     eax, [local_cnt]
    push    rax
    inc     dword [scope_depth]

    mov     al, TOK_KW
    mov     ah, KW_RECOVER
    call    expect_tok_kw

    call    next_tok_ptr
    cmp     byte [rax], TOK_IDENT
    jne     parse_err_at_tok
    mov     edi, [rax + 8]
    mov     esi, TY_I64
    mov     edx, [rax + 4]
    call    alloc_local_scoped
    mov     ebx, eax                ; `err` rbp_disp32

    call    emit_jmp_rel32
    push    rax                     ; healthy_done patch

    call    rec_pop_and_patch_here

    cmp     byte [in_isr_mode], 0
    jne     err_isr_ban_cur
    call    emit_fault_take_rax
    mov     edi, ebx
    call    emit_store_local

    ; Body runs in this same `recover` scope so `err` dies at closing `}`!
    call    parse_block_no_scope_push

    pop     rdi
    call    patch_rel32_here

    dec     dword [scope_depth]
    pop     rax
    mov     [local_cnt], eax
    pop     rbx
    ret

parse_stmt:
    call    peek_tok_ptr
    movzx   ecx, byte [rax]
    cmp     cl, TOK_LBRACE
    je      parse_guarded_block_stmt
    cmp     cl, TOK_AT
    je      parse_at_intrinsic
    cmp     cl, TOK_KW
    je      .kw_stmt
    cmp     cl, TOK_IDENT
    je      .chk_ident_or_match
    jmp     parse_err_at_tok

.chk_ident_or_match:
    mov     edx, [rax + 8]
    cmp     edx, [sym_tile_id]
    je      .chk_tile_blk
    cmp     edx, [sym_match_id]
    jne     parse_ident_stmt
    cmp     byte [rax + 16], TOK_WALRUS
    je      parse_ident_stmt
    cmp     byte [rax + 16], TOK_COLON
    je      parse_ident_stmt
    cmp     byte [rax + 16], TOK_ASSIGN
    je      parse_ident_stmt
    jmp     parse_stmt_match
.chk_tile_blk:
    cmp     byte [rax + 16], TOK_LBRACE
    je      parse_stmt_tile
    jmp     parse_ident_stmt

.kw_stmt:
    movzx   edx, byte [rax + 1]
    cmp     dl, KW_PRINT
    je      parse_stmt_print
    cmp     dl, KW_IF
    je      parse_stmt_if
    cmp     dl, KW_FOR
    je      parse_stmt_for
    cmp     dl, KW_LOOP
    je      parse_stmt_loop
    cmp     dl, KW_RETURN
    je      parse_stmt_return
    cmp     dl, KW_BREAK
    je      parse_stmt_break
    cmp     dl, KW_CONTINUE
    je      parse_stmt_continue
    cmp     dl, KW_FAIL
    je      parse_stmt_fail
    jmp     parse_err_at_tok

parse_stmt_fail:
    inc     dword [tok_idx]
    mov     edi, 1
    call    parse_expr
    ; Dynamic `0` is rewritten to fault code `1` (§5.3)
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC085
    call    emit_u16
    mov     ax, 0x0575
    call    emit_u16
    mov     al, 0xB8
    call    emit_u8
    mov     eax, 1
    call    emit_u32

    call    emit_fault_store_rax

    cmp     dword [rec_depth], 0
    jg      .jump_rec
    mov     ecx, [tile_lex_depth]
    call    emit_tile_unwind_count
    mov     eax, 0xC3C9C031
    jmp     emit_u32
.jump_rec:
    jmp     emit_unwind_to_recover

; -----------------------------------------------------------------------------
; Variable Declarations & Lvalues (Bounds-Checked, Truncated & Scoped)
; -----------------------------------------------------------------------------
parse_ident_stmt:
    push    rbx
    push    r12
    push    r13
    push    r14
    push    r15
    sub     rsp, 8

    mov     ebx, [rax + 8]          ; sym_id
    mov     eax, [rax + 4]
    mov     [rsp], eax              ; save decl line number!
    inc     dword [tok_idx]

    call    peek_tok_ptr
    movzx   ecx, byte [rax]
    cmp     cl, TOK_WALRUS
    je      .decl_walrus
    cmp     cl, TOK_COLON
    je      .decl_typed
    cmp     cl, TOK_LPAREN
    je      .call_stmt

    xor     r14d, r14d              ; 1 if bitfield lvalue

    mov     edi, ebx
    call    find_space
    test    rax, rax
    jnz     .lv_space

    mov     edi, ebx
    call    find_local_entry
    test    eax, eax
    jz      parse_err_cur_tok
    mov     r12d, eax
    mov     r13d, edx

    test    r13d, TY_FLAG_SPAN
    jnz     .lv_span
    test    r13d, TY_FLAG_ATOMIC
    jnz     .lv_atomic
    test    r13d, TY_FLAG_VIEW_ONE
    jnz     .lv_view1

    mov     eax, r13d
    and     eax, TY_BASE_MASK
    cmp     eax, TY_CUSTOM_PTR_BASE
    jb      .lv_plain_local
    call    peek_tok_ptr
    cmp     byte [rax], TOK_LBRACK
    jne     .lv_plain_local
    call    parse_custom_ptr_index_stmt
    jmp     .done

.lv_atomic:
    call    peek_tok_ptr
    cmp     byte [rax], TOK_DOT
    jne     .lv_plain_local
    call    parse_atomic_rval_or_stmt
    jmp     .done

.lv_plain_local:
    mov     edi, r12d
    call    emit_lea_local
    jmp     .lv_walk_fields

.lv_span:
    mov     al, TOK_LBRACK
    call    expect_tok_kind
    mov     edi, 1
    call    parse_expr              ; index in `rax`
    mov     al, TOK_RBRACK
    call    expect_tok_kind

    mov     edi, r12d
    call    emit_span_bounds_check_rax ; `idx >= len` -> `fail 2`

    mov     edi, r13d
    and     edi, TY_BASE_MASK
    and     r13d, TY_BASE_MASK | TY_FLAG_STREAM ; preserve `movnti` streaming flag!
    call    type_size_align
    cmp     eax, 1
    jbe     .lv_span_base
    call    emit_mov_rcx_imm64
    mov     eax, 0xC1AF0F48
    call    emit_u32
.lv_span_base:
    mov     edi, r12d
    call    emit_load_local_rcx
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC801              ; add rax, rcx
    call    emit_u16
    jmp     .lv_walk_fields

.lv_view1:
    call    peek_tok_ptr
    cmp     byte [rax], TOK_DOT
    je      .lv_view1_field
    ; Reassigning the `@Type` view pointer variable itself!
    mov     edi, r12d
    call    emit_lea_local
    jmp     .lv_have_addr
.lv_view1_field:
    mov     edi, r12d
    call    emit_load_local         ; `rax` = pointed address
    test    r13d, TY_FLAG_MMIO
    jnz     .lv_v1_mask
    call    emit_null_view_check_rax
.lv_v1_mask:
    and     r13d, TY_BASE_MASK
    cmp     r13d, TY_STRUCT_BASE
    jae     .lv_walk_fields

    ; 1-arg scalar/bitpack `@mmio<T>(addr).write(val)` / `@view<T>(addr).write(val)`!
    call    alloc_temp_qword
    mov     r14d, eax               ; save target address
    mov     edi, r14d
    call    emit_store_local

    inc     dword [tok_idx]         ; consume `.`
    call    next_tok_ptr
    cmp     byte [rax], TOK_IDENT
    jne     parse_err_at_tok
    mov     ecx, [rax + 8]
    cmp     ecx, [sym_write_id]
    jne     parse_err_cur_tok
    mov     al, TOK_LPAREN
    call    expect_tok_kind
    mov     edi, 1
    call    parse_expr              ; val -> `rax`
    mov     al, TOK_RPAREN
    call    expect_tok_kind

    mov     edi, r14d
    call    emit_load_local_rcx     ; rcx = target addr
    mov     edi, r13d
    call    emit_store_mem_typed
    mov     edi, 1
    call    free_temp_qwords
    jmp     .done

.lv_space:
    mov     r15, rax                ; &SpaceEntry
    cmp     dword [r15 + 4], SKIND_PORT
    jne     .lv_space_mem
    call    parse_port_stmt
    jmp     .done
.lv_space_mem:
    mov     al, TOK_LBRACK
    call    expect_tok_kind
    mov     edi, 1
    call    parse_expr              ; `rax` = index
    mov     al, TOK_RBRACK
    call    expect_tok_kind

    ; `@ring` / `@dma_queue` wrap power-of-2 (`idx & (cap - 1)`); `@slab` / `@arena` check `idx < cap` (`fail 2`)
    mov     rax, [r15 + 24]
    cmp     dword [r15 + 4], SKIND_RING
    je      .lv_sp_wrap
    cmp     dword [r15 + 4], SKIND_DMA_QUEUE
    je      .lv_sp_wrap
    call    emit_bounds_check_rax
    jmp     .lv_sp_mul
.lv_sp_wrap:
    dec     rax
    call    emit_mov_rcx_imm64
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC821              ; and rax, rcx
    call    emit_u16
.lv_sp_mul:
    mov     eax, [r15 + 16]
    cmp     eax, 1
    jbe     .lv_sp_base
    call    emit_mov_rcx_imm64
    mov     eax, 0xC1AF0F48
    call    emit_u32
.lv_sp_base:
    mov     rax, [r15 + 8]
    call    emit_load_space_base_rcx
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC801
    call    emit_u16
    mov     r13d, [r15 + 20]

.lv_walk_fields:
    call    peek_tok_ptr
    cmp     byte [rax], TOK_DOT
    je      .lv_do_dot
    cmp     byte [rax], TOK_LBRACK
    jne     .lv_have_addr

    ; Lvalue hardware bit-slice `target[hi:lo] = val`!
    mov     edx, r13d
    and     edx, TY_BASE_MASK
    cmp     edx, TY_STRUCT_BASE
    jae     parse_err_at_tok
    call    parse_bitslice_bounds   ; eax = lo, edx = width
    lea     r15, [bit_slice_desc]
    mov     dword [r15 + 0], 0
    mov     [r15 + 4], eax          ; shift = lo
    mov     [r15 + 8], edx          ; width = hi - lo + 1
    mov     dword [r15 + 12], TY_U64
    mov     r14d, 1                 ; bitfield RMW flag
    jmp     .lv_have_addr

.lv_do_dot:
    inc     dword [tok_idx]
    call    next_tok_ptr
    cmp     byte [rax], TOK_IDENT
    jne     parse_err_at_tok
    mov     esi, [rax + 8]

    cmp     r13d, TY_STRUCT_BASE
    jae     .lv_struct_dot
    cmp     r13d, TY_BITPACK_BASE
    jb      parse_err_cur_tok

    mov     edi, r13d
    sub     edi, TY_BITPACK_BASE
    call    find_type_field
    test    rax, rax
    jz      parse_err_cur_tok
    mov     r15, rax
    mov     r14d, 1
    jmp     .lv_have_addr

.lv_struct_dot:
    mov     edi, r13d
    sub     edi, TY_STRUCT_BASE
    call    find_type_field
    test    rax, rax
    jz      parse_err_cur_tok
    mov     ecx, [rax + 4]
    mov     r13d, [rax + 12]
    test    ecx, ecx
    jz      .lv_walk_fields
    push    rcx
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC081
    call    emit_u16
    pop     rax
    call    emit_u32
    jmp     .lv_walk_fields

.lv_have_addr:
    call    alloc_temp_qword
    mov     r12d, eax
    mov     edi, r12d
    call    emit_store_local

    call    next_tok_ptr
    movzx   ebx, byte [rax]         ; assign op kind

    mov     edi, 1
    call    parse_expr              ; rhs -> `rax`

    test    r14d, r14d
    jnz     .lv_store_bitfield

    cmp     ebx, TOK_ASSIGN
    je      .lv_write_mem

    mov     edi, r13d
    and     edi, TY_BASE_MASK
    cmp     edi, TY_STRUCT_BASE
    jae     parse_err_cur_tok

    mov     al, 0x49
    call    emit_u8
    mov     ax, 0xC089              ; mov r8, rax
    call    emit_u16
    mov     edi, r12d
    call    emit_load_local
    mov     edi, r13d
    call    emit_load_mem_typed
    mov     al, 0x4C
    call    emit_u8
    mov     ax, 0xC189              ; mov rcx, r8
    call    emit_u16
    mov     edi, ebx
    mov     esi, r13d               ; pass target `ty_tag` for signed/unsigned `/=`!
    call    emit_compound_alu

.lv_write_mem:
    mov     edi, r12d
    call    emit_load_local_rcx
    mov     edi, r13d
    call    emit_store_mem_typed

    mov     edi, 1
    call    free_temp_qwords
    jmp     .done

.lv_store_bitfield:
    mov     al, 0x49
    call    emit_u8
    mov     ax, 0xC089
    call    emit_u16

    mov     edi, r12d
    call    emit_load_local
    mov     edi, r13d
    call    emit_load_mem_typed
    mov     al, 0x49
    call    emit_u8
    mov     ax, 0xC189
    call    emit_u16

    cmp     ebx, TOK_ASSIGN
    je      .bf_use_r8

    mov     edi, [r15 + 4]
    mov     esi, [r15 + 8]
    call    emit_extract_bitfield
    mov     al, 0x4C
    call    emit_u8
    mov     ax, 0xC189
    call    emit_u16
    mov     edi, ebx
    mov     esi, TY_U64             ; bitpack fields are unsigned!
    call    emit_compound_alu
    jmp     .bf_pack_new

.bf_use_r8:
    mov     al, 0x4C
    call    emit_u8
    mov     ax, 0xC089
    call    emit_u16

.bf_pack_new:
    mov     ecx, [r15 + 8]
    call    width_to_mask64
    mov     r14, rax
    cmp     dword [r15 + 8], 64
    jae     .bf_shift_val
    call    emit_mov_rcx_imm64
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC821
    call    emit_u16

.bf_shift_val:
    mov     ecx, [r15 + 4]
    test    ecx, ecx
    jz      .bf_merge
    shl     r14, cl
    push    rcx
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xE0C1
    call    emit_u16
    pop     rax
    call    emit_u8

.bf_merge:
    not     r14
    mov     rax, r14
    call    emit_mov_rcx_imm64
    mov     al, 0x49
    call    emit_u8
    mov     ax, 0xC921
    call    emit_u16
    mov     al, 0x4C
    call    emit_u8
    mov     ax, 0xC809
    call    emit_u16
    jmp     .lv_write_mem

.decl_typed:
    inc     dword [tok_idx]
    call    peek_tok_ptr
    cmp     byte [rax], TOK_AT
    jne     .dt_norm
    inc     dword [tok_idx]
    call    parse_resolved_type_tag
    or      eax, TY_FLAG_VIEW_ONE
    mov     r13d, eax
    jmp     .dt_scalar
.dt_norm:
    call    parse_fn_type_annot
    mov     r13d, eax
    test    r13d, TY_FLAG_SPAN
    jnz     .decl_span_typed
    test    r13d, TY_FLAG_VIEW_ONE | TY_FLAG_ATOMIC
    jnz     .dt_scalar
    mov     edx, r13d
    and     edx, TY_BASE_MASK
    cmp     edx, TY_CUSTOM_PTR_BASE
    jae     .dt_scalar
    cmp     edx, TY_STRUCT_BASE
    jae     .decl_struct_var
.dt_scalar:
    mov     al, TOK_ASSIGN
    call    expect_tok_kind
    mov     edi, 1
    call    parse_expr               ; value in `rax`

    mov     edi, ebx
    mov     esi, r13d
    mov     edx, [rsp]              ; line_num
    call    alloc_local_scoped
    mov     r12d, eax

    ; Store via `emit_store_mem_typed` so `u8/u16/u32` truncate cleanly on store!
    mov     edi, r12d
    call    emit_lea_local_rcx
    mov     edi, r13d
    call    emit_store_mem_typed
    jmp     .done

.decl_span_typed:
    mov     al, TOK_ASSIGN
    call    expect_tok_kind
    mov     edi, 1
    call    parse_expr
    mov     esi, r13d
    jmp     .decl_span_walrus

.decl_struct_var:
    mov     edi, ebx
    mov     esi, r13d
    mov     edx, [rsp]
    call    alloc_local_scoped
    mov     r12d, eax

    call    peek_tok_ptr
    cmp     byte [rax], TOK_ASSIGN
    jne     .decl_struct_zero
    inc     dword [tok_idx]         ; consume `=`
    mov     edi, 1
    call    parse_expr              ; `rax` = src struct address
    mov     edi, r12d
    call    emit_lea_local_rcx      ; `rcx` = dst struct address
    mov     edi, r13d
    call    emit_store_mem_typed    ; `rep movsb`!
    jmp     .done

.decl_struct_zero:
    mov     edi, r13d
    call    type_size_align
    add     eax, 7
    shr     eax, 3
    mov     r14d, eax
    xor     eax, eax
    call    emit_mov_rax_imm64
    xor     r15d, r15d
.zero_st:
    cmp     r15d, r14d
    jae     .done
    lea     edi, [r12 + r15 * 8]
    call    emit_store_local
    inc     r15d
    jmp     .zero_st

.decl_walrus:
    inc     dword [tok_idx]
    mov     edi, 1
    call    parse_expr
    mov     esi, [last_expr_ty]
    test    esi, TY_FLAG_SPAN
    jnz     .decl_span_walrus
    mov     edi, ebx
    mov     edx, [rsp]
    call    alloc_local_scoped
    mov     r12d, eax
    mov     edi, r12d
    call    emit_lea_local_rcx
    mov     edi, [last_expr_ty]
    call    emit_store_mem_typed
    jmp     .done

.decl_span_walrus:
    mov     edi, ebx
    mov     edx, [rsp]
    call    alloc_local_scoped
    mov     r12d, eax               ; 16B destination `[rbp + r12d]`
    mov     al, 0x49
    call    emit_u8
    mov     ax, 0xC089              ; 49 89 C0 = mov r8, rax
    call    emit_u16
    mov     al, 0x49
    call    emit_u8
    mov     ax, 0x008B              ; 49 8B 00 = mov rax, [r8] (`base`)
    call    emit_u16
    mov     edi, r12d
    call    emit_store_local
    mov     eax, 0x08408B49         ; 49 8B 40 08 = mov rax, [r8 + 8] (`len`)
    call    emit_u32
    lea     edi, [r12d + 8]
    call    emit_store_local
    jmp     .done

.call_stmt:
    dec     dword [tok_idx]
    call    parse_ident_rvalue

.done:
    add     rsp, 8
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbx
    ret

; edi = TOK_*_EQ, esi = target `ty_tag` (for signed vs unsigned `/=` + div0 guard!)
emit_compound_alu:
    push    rsi
    cmp     edi, TOK_PLUS_EQ
    je      .add
    cmp     edi, TOK_MINUS_EQ
    je      .sub
    cmp     edi, TOK_STAR_EQ
    je      .mul
    cmp     edi, TOK_SLASH_EQ
    je      .div
    cmp     edi, TOK_PIPE_EQ
    je      .or
    cmp     edi, TOK_AMP_EQ
    je      .and
    cmp     edi, TOK_CARET_EQ
    je      .xor
    cmp     edi, TOK_PERCENT_EQ
    je      .mod
    cmp     edi, TOK_SHL_EQ
    je      .shl
    cmp     edi, TOK_SHR_EQ
    je      .shr
    cmp     edi, TOK_ROL_EQ
    je      .rol
    cmp     edi, TOK_ROR_EQ
    je      .ror
    jmp     parse_err_cur_tok
.add:
    pop     rsi
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC801
    jmp     emit_u16
.sub:
    pop     rsi
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC829
    jmp     emit_u16
.mul:
    pop     rsi
    mov     eax, 0xC1AF0F48
    jmp     emit_u32
.div:
    call    emit_div_zero_check_rcx
    pop     rdi                     ; target ty_tag
    call    is_ty_unsigned
    test    eax, eax
    jnz     .udiv
    mov     ax, 0x9948
    call    emit_u16
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xF9F7
    jmp     emit_u16
.udiv:
    mov     ax, 0xD231
    call    emit_u16
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xF1F7
    jmp     emit_u16
.or:
    pop     rsi
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC809
    jmp     emit_u16
.and:
    pop     rsi
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC821
    jmp     emit_u16
.xor:
    pop     rsi
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC831
    jmp     emit_u16
.mod:
    call    emit_div_zero_check_rcx
    pop     rdi
    call    is_ty_unsigned
    test    eax, eax
    jnz     .umod
    mov     ax, 0x9948              ; cqo
    call    emit_u16
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xF9F7              ; idiv rcx
    call    emit_u16
    jmp     .mod_mv
.umod:
    mov     ax, 0xD231
    call    emit_u16
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xF1F7              ; div rcx
    call    emit_u16
.mod_mv:
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xD089              ; mov rax, rdx
    jmp     emit_u16
.shl:
    pop     rsi
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xE0D3              ; shl rax, cl
    jmp     emit_u16
.shr:
    pop     rdi
    call    is_ty_unsigned
    test    eax, eax
    jnz     .ushr
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xF8D3              ; sar rax, cl
    jmp     emit_u16
.ushr:
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xE8D3              ; shr rax, cl
    jmp     emit_u16
.rol:
    pop     rsi
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC0D3              ; rol rax, cl
    jmp     emit_u16
.ror:
    pop     rsi
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC8D3              ; ror rax, cl
    jmp     emit_u16

; -----------------------------------------------------------------------------
; Control Flow (`return`, `if`, Scoped `for` & `loop`)
; -----------------------------------------------------------------------------
parse_stmt_return:
    inc     dword [tok_idx]
    cmp     dword [cur_fn_attr], 1  ; `@isr` returns via `iretq` (no return value)!
    jne     .ret_norm
    call    peek_tok_ptr
    cmp     byte [rax], TOK_RBRACE
    je      emit_isr_epilogue
    jmp     err_isr_ban_cur
.ret_norm:
    call    peek_tok_ptr
    cmp     byte [rax], TOK_RBRACE
    je      .ret_void
    mov     edi, 1
    call    parse_expr
    test    dword [cur_fn_ret_ty], TY_FLAG_SPAN
    jz      .epi
    ; Return 16-byte `{ base, len }` slice in `RAX:RDX`!
    mov     eax, 0x08508B48         ; 48 8B 50 08 = mov rdx, [rax + 8]
    call    emit_u32
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0x008B              ; 48 8B 00 = mov rax, [rax]
    call    emit_u16
    jmp     .epi
.ret_void:
    mov     ax, 0xC031
    call    emit_u16
.epi:
    cmp     dword [tile_lex_depth], 0
    jle     .epi_leave
    push    rax
    mov     al, 0x49
    call    emit_u8
    mov     ax, 0xC089              ; mov r8, rax (preserve return value across tile unwind!)
    call    emit_u16
    mov     ecx, [tile_lex_depth]
    call    emit_tile_unwind_count
    mov     al, 0x4C
    call    emit_u8
    mov     ax, 0xC089              ; mov rax, r8
    call    emit_u16
    pop     rax
.epi_leave:
    mov     ax, 0xC3C9
    jmp     emit_u16

parse_stmt_if:
    push    rbx
    push    r12
    inc     dword [tok_idx]
    call    try_eval_const_if
    test    eax, eax
    jz      .runtime_if

    test    rdx, rdx
    jz      .const_false
    call    parse_block
    call    skip_else_chain
    pop     r12
    pop     rbx
    ret

.const_false:
    call    skip_brace_block
    call    peek_tok_ptr
    cmp     byte [rax], TOK_KW
    jne     .cf_done
    cmp     byte [rax + 1], KW_ELSE
    jne     .cf_done
    inc     dword [tok_idx]         ; consume `else`
    call    peek_tok_ptr
    cmp     byte [rax], TOK_KW
    jne     .cf_else_blk
    cmp     byte [rax + 1], KW_IF
    jne     .cf_else_blk
    pop     r12
    pop     rbx
    jmp     parse_stmt_if           ; tail-evaluate `else if`!
.cf_else_blk:
    call    parse_block
.cf_done:
    pop     r12
    pop     rbx
    ret

.runtime_if:
    mov     edi, 1
    call    parse_expr
    call    emit_jz_rel32
    mov     ebx, eax
    call    parse_block

    call    peek_tok_ptr
    cmp     byte [rax], TOK_KW
    jne     .no_else
    cmp     byte [rax + 1], KW_ELSE
    jne     .no_else

    inc     dword [tok_idx]
    call    emit_jmp_rel32
    mov     r12d, eax
    mov     edi, ebx
    call    patch_rel32_here

    call    peek_tok_ptr
    cmp     byte [rax], TOK_KW
    jne     .else_blk
    cmp     byte [rax + 1], KW_IF
    jne     .else_blk
    call    parse_stmt_if
    jmp     .end_if
.else_blk:
    call    parse_block
.end_if:
    mov     edi, r12d
    call    patch_rel32_here
    pop     r12
    pop     rbx
    ret
.no_else:
    mov     edi, ebx
    call    patch_rel32_here
    pop     r12
    pop     rbx
    ret

loop_push_scope:
    mov     eax, [loop_depth]
    mov     rcx, [space_ast]
    mov     dword [rcx + AST_BRK_CNT_OFF + rax * 4], 0
    mov     dword [rcx + AST_CNT_CNT_OFF + rax * 4], 0
    mov     edx, [tile_lex_depth]
    mov     [rcx + AST_LOOP_TILE_OFF + rax * 4], edx
    inc     dword [loop_depth]
    ret

emit_unwind_loop_tiles:
    mov     edx, [loop_depth]
    dec     edx
    mov     rcx, [space_ast]
    mov     eax, [tile_lex_depth]
    sub     eax, [rcx + AST_LOOP_TILE_OFF + rdx * 4]
    jle     .ok
    mov     ecx, eax
    jmp     emit_tile_unwind_count
.ok:
    ret

parse_stmt_break:
    cmp     dword [loop_depth], 0
    je      parse_err_cur_tok
    inc     dword [tok_idx]
    call    emit_unwind_loop_tiles
    call    emit_jmp_rel32
    mov     edx, [loop_depth]
    dec     edx
    mov     rcx, [space_ast]
    mov     esi, [rcx + AST_BRK_CNT_OFF + rdx * 4]
    inc     dword [rcx + AST_BRK_CNT_OFF + rdx * 4]
    shl     edx, 8
    lea     rdi, [rcx + AST_BRK_PATCH_OFF + rdx]
    mov     [rdi + rsi * 4], eax
    ret

parse_stmt_continue:
    cmp     dword [loop_depth], 0
    je      parse_err_cur_tok
    inc     dword [tok_idx]
    call    emit_unwind_loop_tiles
    call    emit_jmp_rel32
    mov     edx, [loop_depth]
    dec     edx
    mov     rcx, [space_ast]
    mov     esi, [rcx + AST_CNT_CNT_OFF + rdx * 4]
    inc     dword [rcx + AST_CNT_CNT_OFF + rdx * 4]
    shl     edx, 8
    lea     rdi, [rcx + AST_CNT_PATCH_OFF + rdx]
    mov     [rdi + rsi * 4], eax
    ret

loop_patch_continues_here:
    push    rbx
    mov     ebx, [loop_depth]
    dec     ebx
    mov     rcx, [space_ast]
    mov     r8d, [rcx + AST_CNT_CNT_OFF + rbx * 4]
    mov     dword [rcx + AST_CNT_CNT_OFF + rbx * 4], 0 ; reset count for `<unroll: K>` steps!
    shl     ebx, 8
.c_loop:
    test    r8d, r8d
    jz      .done
    dec     r8d
    mov     rcx, [space_ast]
    lea     rcx, [rcx + AST_CNT_PATCH_OFF + rbx]
    mov     edi, [rcx + r8 * 4]
    push    r8
    call    patch_rel32_here
    pop     r8
    jmp     .c_loop
.done:
    pop     rbx
    ret

loop_pop_and_patch_breaks_here:
    push    rbx
    dec     dword [loop_depth]
    mov     ebx, [loop_depth]
    mov     rcx, [space_ast]
    mov     r8d, [rcx + AST_BRK_CNT_OFF + rbx * 4]
    shl     ebx, 8
.b_loop:
    test    r8d, r8d
    jz      .done
    dec     r8d
    mov     rcx, [space_ast]
    lea     rcx, [rcx + AST_BRK_PATCH_OFF + rbx]
    mov     edi, [rcx + r8 * 4]
    push    r8
    call    patch_rel32_here
    pop     r8
    jmp     .b_loop
.done:
    pop     rbx
    ret

parse_stmt_for:
    jmp     parse_stmt_for_ext

parse_stmt_loop:
    push    rbx
    push    r12
    push    r13
    push    r14
    push    r15
    sub     rsp, 16                 ; [rsp+0] = has_rec, [rsp+4] = saved_local_cnt, [rsp+8] = has_tile, [rsp+12] = tile_line

    mov     eax, [local_cnt]
    mov     [rsp + 4], eax
    mov     dword [rsp + 8], 0

    inc     dword [tok_idx]         ; consume `loop`
    xor     r12d, r12d
    xor     r13d, r13d
    xor     ebx,  ebx

    call    peek_tok_ptr
    cmp     byte [rax], TOK_LT
    jne     .check_rec

    inc     dword [tok_idx]
.lens_loop:
    call    next_tok_ptr
    cmp     byte [rax], TOK_IDENT
    jne     parse_err_at_tok
    mov     ecx, [rax + 8]
    mov     esi, [rax + 4]

    cmp     ecx, [sym_tile_id]
    je      .parse_tile_clause

    mov     al, TOK_COLON
    call    expect_tok_kind

    cmp     ecx, [sym_max_id]
    je      .parse_max_clause
    cmp     ecx, [sym_until_id]
    je      .parse_until_clause
.bad_loop_lens:
    lea     rdi, [msg_unimpl_lens]
    mov     edx, msg_unimpl_lens_len
    jmp     err_msg_at_line

.parse_tile_clause:
    call    peek_tok_ptr
    cmp     byte [rax], TOK_COLON
    je      .bad_loop_lens          ; `<tile: 32KB>` stays rejected until Phase 4
    mov     dword [rsp + 8], 1
    mov     [rsp + 12], esi
    jmp     .next_lens

.parse_max_clause:
    xor     eax, eax
    call    emit_mov_rax_imm64
    call    alloc_anon_local
    mov     r12d, eax
    mov     edi, r12d
    call    emit_store_local

    call    parse_loop_lens_expr
    call    alloc_anon_local
    mov     r13d, eax
    mov     edi, r13d
    call    emit_store_local
    jmp     .next_lens

.parse_until_clause:
    mov     ebx, [tok_idx]
    call    advance_to_loop_lens_delim
    jmp     .next_lens

.next_lens:
    call    peek_tok_ptr
    cmp     byte [rax], TOK_PIPE
    jne     .close_lens
    inc     dword [tok_idx]
    jmp     .lens_loop
.close_lens:
    mov     al, TOK_GT
    call    expect_tok_kind

.check_rec:
    call    block_has_trailing_recover
    mov     [rsp], eax
    test    eax, eax
    jz      .start_loop
    call    rec_push_scope

.start_loop:
    cmp     dword [rsp + 8], 0
    je      .lp_no_tile_in
    mov     esi, [rsp + 12]
    call    emit_tile_enter
.lp_no_tile_in:
    call    loop_push_scope
    mov     r14d, [code_off]
    xor     r15d, r15d

    test    r12d, r12d
    jz      .chk_until
    mov     edi, r12d
    call    emit_load_local
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0x853B
    call    emit_u16
    mov     eax, r13d
    call    emit_u32
    mov     ax, 0x8D0F
    call    emit_u16
    mov     r15d, [code_off]
    xor     eax, eax
    call    emit_u32

.chk_until:
    test    ebx, ebx
    jz      .body
    mov     eax, [tok_idx]
    push    rax
    mov     [tok_idx], ebx
    call    parse_loop_lens_expr
    pop     rax
    mov     [tok_idx], eax

    call    emit_jnz_rel32
    mov     edx, [loop_depth]
    dec     edx
    mov     rcx, [space_ast]
    mov     esi, [rcx + AST_BRK_CNT_OFF + rdx * 4]
    inc     dword [rcx + AST_BRK_CNT_OFF + rdx * 4]
    shl     edx, 8
    lea     rcx, [rcx + AST_BRK_PATCH_OFF + rdx]
    mov     [rcx + rsi * 4], eax

.body:
    call    parse_block

    call    loop_patch_continues_here
    test    r12d, r12d
    jz      .jump_back
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0x85FF
    call    emit_u16
    mov     eax, r12d
    call    emit_u32

.jump_back:
    mov     edi, r14d
    call    emit_jmp_to

    test    r15d, r15d
    jz      .patch_brks
    mov     edi, r15d
    call    patch_rel32_here
.patch_brks:
    call    loop_pop_and_patch_breaks_here
    cmp     dword [rsp + 8], 0
    je      .lp_no_tile_out
    call    emit_tile_exit
.lp_no_tile_out:

    ; Rewind hidden loop locals before `recover` (§5.2)
    mov     eax, [rsp + 4]
    mov     [local_cnt], eax

    cmp     dword [rsp], 0
    je      .done
    call    parse_trailing_recover_body

.done:
    add     rsp, 16
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbx
    ret

; -----------------------------------------------------------------------------
; Compile-Time Interpolated `print(...)`
; -----------------------------------------------------------------------------
parse_stmt_print:
    cmp     byte [mode_efi], 1
    jne     .print_ok
    call    peek_tok_ptr
    mov     esi, [rax + 4]
    lea     rdi, [msg_no_host_in_efi]
    mov     edx, msg_no_host_in_efi_len
    jmp     err_msg_at_line
.print_ok:
    push    rbx
    push    r12
    push    r13
    push    r14

    inc     dword [tok_idx]
    mov     al, TOK_LPAREN
    call    expect_tok_kind

    call    peek_tok_ptr
    cmp     byte [rax], TOK_STR
    je      .str_interp

    mov     edi, 1
    call    parse_expr
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC789
    call    emit_u16
    mov     edi, [rt_write_i64_off]
    call    emit_call_direct_off

    lea     rsi, [nl_byte]
    mov     ecx, 1
    call    emit_write_rodata_slice

    mov     al, TOK_RPAREN
    call    expect_tok_kind
    jmp     .done

.str_interp:
    call    next_tok_ptr
    movzx   r13d, word [rax + 2]
    mov     r12, [space_rodata]
    add     r12, [rax + 8]
    add     r13, r12

    mov     al, TOK_RPAREN
    call    expect_tok_kind

    mov     r14, r12
    mov     rbx, r12

.scan_str:
    cmp     rbx, r13
    jae     .flush_tail
    cmp     byte [rbx], '{'
    jne     .adv_chr

    mov     rcx, rbx
    sub     rcx, r14
    mov     rsi, r14
    call    emit_write_rodata_slice

    inc     rbx
    mov     r14, rbx
.find_rbrace:
    cmp     rbx, r13
    jae     parse_err_cur_tok
    cmp     byte [rbx], '}'
    je      .eval_subexpr
    inc     rbx
    jmp     .find_rbrace

.eval_subexpr:
    mov     rcx, rbx
    sub     rcx, r14
    mov     rdi, [space_ast]
    add     rdi, AST_INTERP_BUF_OFF
    mov     r8,  rdi
    mov     r9,  rcx
    mov     rsi, r14
    rep     movsb
    mov     word [rdi], 0

    mov     eax, [tok_idx]
    mov     edx, [tok_count]
    push    rax
    push    rdx

    mov     [tok_idx], edx
    mov     rdi, r8
    mov     rsi, r9
    call    lexer_tokenize_range

    mov     edi, 1
    call    parse_expr
    mov     al, TOK_EOF
    call    expect_tok_kind

    pop     rdx
    pop     rax
    mov     [tok_count], edx
    mov     [tok_idx],   eax

    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC789
    call    emit_u16
    mov     edi, [rt_write_i64_off]
    call    emit_call_direct_off

    inc     rbx
    mov     r14, rbx
    jmp     .scan_str

.adv_chr:
    inc     rbx
    jmp     .scan_str

.flush_tail:
    mov     rcx, r13
    sub     rcx, r14
    mov     rdi, [space_ast]
    add     rdi, AST_INTERP_BUF_OFF
    mov     r8,  rdi
    mov     rsi, r14
    mov     edx, ecx
    rep     movsb
    mov     byte [rdi], 10
    inc     edx

    mov     rsi, r8
    mov     ecx, edx
    call    emit_write_rodata_slice

.done:
    pop     r14
    pop     r13
    pop     r12
    pop     rbx
    ret

; -----------------------------------------------------------------------------
; Function Signatures, Definitions & Module Driver
; -----------------------------------------------------------------------------
prescan_fn_signatures:
    ; Zero `AST_FN_ATTR_OFF` (`4096` bytes = `512` qwords)
    mov     rdi, [space_ast]
    add     rdi, AST_FN_ATTR_OFF
    xor     eax, eax
    mov     ecx, 512
    rep     stosq

    mov     dword [tok_idx], 0
.loop:
    call    peek_tok_ptr
    movzx   ecx, byte [rax]
    test    cl, cl
    jz      .done
    xor     r8d, r8d                ; `r8b = 0` (normal), `1` (`@isr`), `2` (`@isr_ok`)
    cmp     cl, TOK_AT
    jne     .chk_kw_fn
    cmp     byte [rax + 16], TOK_IDENT
    jne     .adv_non_fn
    mov     edx, [rax + 24]
    cmp     edx, [sym_isr_id]
    je      .ps_is_isr
    cmp     edx, [sym_isr_ok_id]
    jne     .adv_non_fn
    mov     r8b, 2
    add     dword [tok_idx], 2
    jmp     .got_attr
.ps_is_isr:
    mov     r8b, 1
    add     dword [tok_idx], 2
.got_attr:
    call    peek_tok_ptr
    movzx   ecx, byte [rax]
.chk_kw_fn:
    cmp     cl, TOK_KW
    jne     .adv_non_fn
    cmp     byte [rax + 1], KW_FN
    jne     .adv_non_fn

    inc     dword [tok_idx]
    push    r8
    call    next_tok_ptr
    mov     edi, [rax + 8]
    call    find_or_add_fn
    pop     r8
    mov     rcx, [space_ast]
    mov     [rcx + AST_FN_ATTR_OFF + rax], r8b
    push    rax

    mov     al, TOK_LPAREN
    call    expect_tok_kind
    mov     ecx, 1
.sk_p:
    call    next_tok_ptr
    movzx   edx, byte [rax]
    cmp     dl, TOK_LPAREN
    jne     .sk_rp
    inc     ecx
    jmp     .sk_p
.sk_rp:
    cmp     dl, TOK_RPAREN
    jne     .sk_p
    dec     ecx
    jnz     .sk_p

    mov     esi, TY_I64
    call    peek_tok_ptr
    cmp     byte [rax], TOK_LBRACE
    je      .save_ret
    cmp     byte [rax], TOK_FAT_ARROW
    je      .save_ret
    cmp     byte [rax], TOK_ARROW
    jne     .parse_rt
    inc     dword [tok_idx]
.parse_rt:
    call    parse_fn_type_annot
    mov     esi, eax
.save_ret:
    pop     rax
    shl     eax, 4
    mov     rcx, [space_ast]
    mov     [rcx + AST_FN_OFF + rax + 12], esi
    call    peek_tok_ptr
    cmp     byte [rax], TOK_FAT_ARROW
    je      .sk_arrow_sig
    call    skip_brace_block
    jmp     .loop
.sk_arrow_sig:
    inc     dword [tok_idx]
    call    skip_const_rhs_tokens
    jmp     .loop

.adv_non_fn:
    inc     dword [tok_idx]
    jmp     .loop
.done:
    ret

parse_fn_decl:
    push    rbx
    push    r12
    push    r13
    push    r14
    push    r15

    inc     dword [tok_idx]
    call    next_tok_ptr
    cmp     byte [rax], TOK_IDENT
    jne     parse_err_at_tok

    mov     edi, [rax + 8]
    call    find_or_add_fn
    mov     ebx, eax

    mov     rcx, [space_ast]
    add     rcx, AST_FN_OFF
    mov     edx, ebx
    shl     edx, 4
    cmp     dword [rcx + rdx + 8], 1
    je      parse_err_cur_tok
    mov     eax, [code_off]
    mov     [rcx + rdx + 4], eax
    mov     dword [rcx + rdx + 8], 1

    mov     dword [local_cnt], 0
    mov     dword [scope_depth], 1  ; outermost function scope = 1
    mov     dword [frame_qwords], 0
    mov     dword [temp_depth], 0
    mov     dword [max_frame_qwords], 0
    mov     dword [loop_depth], 0
    mov     dword [rec_depth], 0
    mov     dword [tile_lex_depth], 0
    mov     byte  [in_fn_body], 1
    mov     eax, [cur_fn_attr]
    mov     [in_isr_mode], al

    cmp     eax, 1                  ; `@isr`?
    je      .prolog_isr

    mov     al, 0x55
    call    emit_u8
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xE589
    call    emit_u16
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xEC81
    call    emit_u16
    mov     r12d, [code_off]
    xor     eax, eax
    call    emit_u32
    jmp     .parse_params

.prolog_isr:
    call    emit_isr_prologue
    mov     r12d, eax

.parse_params:
    mov     al, TOK_LPAREN
    call    expect_tok_kind
    xor     r13d, r13d

    call    peek_tok_ptr
    cmp     byte [rax], TOK_RPAREN
    je      .params_done
    cmp     dword [cur_fn_attr], 1  ; `@isr` takes 0 software arguments!
    je      err_isr_ban_cur

.param_loop:
    cmp     r13d, 6
    jae     parse_err_cur_tok
    call    next_tok_ptr
    cmp     byte [rax], TOK_IDENT
    jne     parse_err_at_tok
    mov     r14d, [rax + 8]         ; param sym_id
    mov     r15d, [rax + 4]         ; param line

    mov     al, TOK_COLON
    call    expect_tok_kind
    call    parse_fn_type_annot
    push    rax                     ; [rsp] = param_ty
    mov     edi, r14d
    mov     esi, eax
    mov     edx, r15d
    call    alloc_local_scoped
    mov     edi, eax
    pop     rcx                     ; ecx = param_ty

    test    ecx, TY_FLAG_SPAN
    jnz     .sp_span

    cmp     r13d, 0
    je      .sp0
    cmp     r13d, 1
    je      .sp1
    cmp     r13d, 2
    je      .sp2
    cmp     r13d, 3
    je      .sp3
    cmp     r13d, 4
    je      .sp4
    push    rdi
    mov     al, 0x4C
    call    emit_u8
    mov     ax, 0x8D89
    call    emit_u16
    pop     rax
    call    emit_u32
    jmp     .sp_done
.sp_span:
    ; Copy 16B `{ base, len }` pointed to by `arg_reg` into `[rbp + edi]` using `r11` & `rax` (preserves `r10`!)
    push    rdi
    cmp     r13d, 0
    je      .sps0
    cmp     r13d, 1
    je      .sps1
    cmp     r13d, 2
    je      .sps2
    cmp     r13d, 3
    je      .sps3
    cmp     r13d, 4
    je      .sps4
    mov     al, 0x4D
    call    emit_u8
    mov     ax, 0xCB89              ; 4D 89 CB = mov r11, r9
    call    emit_u16
    jmp     .sps_cp
.sps4:
    mov     al, 0x4D
    call    emit_u8
    mov     ax, 0xC389              ; 4D 89 C3 = mov r11, r8
    call    emit_u16
    jmp     .sps_cp
.sps3:
    cmp     byte [mode_efi], 1
    je      .sps3_efi
    mov     al, 0x49
    call    emit_u8
    mov     ax, 0xCB89              ; 49 89 CB = mov r11, rcx (`abi-host`)
    call    emit_u16
    jmp     .sps_cp
.sps3_efi:
    mov     al, 0x4D
    call    emit_u8
    mov     ax, 0xD389              ; 4D 89 D3 = mov r11, r10 (`Reactor ABI`)
    call    emit_u16
    jmp     .sps_cp
.sps2:
    mov     al, 0x49
    call    emit_u8
    mov     ax, 0xD389              ; 49 89 D3 = mov r11, rdx
    call    emit_u16
    jmp     .sps_cp
.sps1:
    mov     al, 0x49
    call    emit_u8
    mov     ax, 0xF389              ; 49 89 F3 = mov r11, rsi
    call    emit_u16
    jmp     .sps_cp
.sps0:
    mov     al, 0x49
    call    emit_u8
    mov     ax, 0xFB89              ; 49 89 FB = mov r11, rdi
    call    emit_u16
.sps_cp:
    mov     al, 0x49
    call    emit_u8
    mov     ax, 0x038B              ; 49 8B 03 = mov rax, [r11]
    call    emit_u16
    mov     rdi, [rsp]
    call    emit_store_local
    mov     eax, 0x08438B49         ; 49 8B 43 08 = mov rax, [r11 + 8]
    call    emit_u32
    pop     rdi
    add     edi, 8
    call    emit_store_local
    jmp     .sp_done
.sp4:
    push    rdi
    mov     al, 0x4C
    call    emit_u8
    mov     ax, 0x8589
    call    emit_u16
    pop     rax
    call    emit_u32
    jmp     .sp_done
.sp3:
    push    rdi
    cmp     byte [mode_efi], 1
    je      .sp3_efi
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0x8D89              ; 48 89 8D <disp32> = mov [rbp + disp32], rcx (`abi-host`)
    call    emit_u16
    pop     rax
    call    emit_u32
    jmp     .sp_done
.sp3_efi:
    mov     al, 0x4C
    call    emit_u8
    mov     ax, 0x9589              ; 4C 89 95 <disp32> = mov [rbp + disp32], r10 (`Reactor ABI` §6.1)
    call    emit_u16
    pop     rax
    call    emit_u32
    jmp     .sp_done
.sp2:
    push    rdi
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0x9589
    call    emit_u16
    pop     rax
    call    emit_u32
    jmp     .sp_done
.sp1:
    push    rdi
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xB589
    call    emit_u16
    pop     rax
    call    emit_u32
    jmp     .sp_done
.sp0:
    push    rdi
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xBD89
    call    emit_u16
    pop     rax
    call    emit_u32

.sp_done:
    inc     r13d
    call    peek_tok_ptr
    cmp     byte [rax], TOK_COMMA
    jne     .params_done
    inc     dword [tok_idx]
    jmp     .param_loop

.params_done:
    mov     al, TOK_RPAREN
    call    expect_tok_kind

    mov     dword [cur_fn_ret_ty], TY_I64
    call    peek_tok_ptr
    cmp     byte [rax], TOK_LBRACE
    je      .fn_body
    cmp     byte [rax], TOK_FAT_ARROW
    je      .fn_arrow_body
    cmp     byte [rax], TOK_ARROW
    jne     .rt
    inc     dword [tok_idx]
.rt:
    call    parse_fn_type_annot
    mov     [cur_fn_ret_ty], eax
    call    peek_tok_ptr
    cmp     byte [rax], TOK_FAT_ARROW
    je      .fn_arrow_body

.fn_body:
    call    parse_block_no_scope_push
    cmp     dword [cur_fn_attr], 1  ; `@isr`?
    jne     .fn_norm_epi
    call    emit_isr_epilogue
    jmp     .fn_finalize
.fn_norm_epi:
    mov     eax, 0xC3C9C031
    call    emit_u32
    jmp     .fn_finalize

.fn_arrow_body:
    cmp     dword [cur_fn_attr], 1  ; `@isr` cannot be value-returning `=>`
    je      err_isr_ban_cur
    inc     dword [tok_idx]         ; consume `=>`
    mov     edi, 1
    call    parse_expr
    test    dword [cur_fn_ret_ty], TY_FLAG_SPAN
    jz      .fn_arrow_epi
    mov     eax, 0x08508B48         ; mov rdx, [rax + 8]
    call    emit_u32
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0x008B              ; mov rax, [rax]
    call    emit_u16
.fn_arrow_epi:
    mov     ax, 0xC3C9              ; leave; ret (`rax` holds expression result!)
    call    emit_u16

.fn_finalize:
    mov     dword [local_cnt], 0
    mov     dword [scope_depth], 0
    mov     byte  [in_fn_body], 0
    mov     byte  [in_isr_mode], 0
    mov     r8d,  [cur_fn_attr]
    mov     dword [cur_fn_attr], 0

    ; Record exact machine-code byte length of `ebx` in `AST_FNLEN_OFF` (`@code_bytes`)!
    mov     rcx, [space_ast]
    mov     edx, ebx
    shl     edx, 4
    mov     eax, [code_off]
    sub     eax, [rcx + AST_FN_OFF + rdx + 4]
    mov     [rcx + AST_FNLEN_OFF + rbx * 4], eax

    mov     eax, [max_frame_qwords]
    shl     eax, 3
    cmp     byte [mode_efi], 1
    jne     .align16_frame
    cmp     r8d, 1                  ; `@isr` aligns via `and rsp, -16`
    je      .align16_frame
    ; Inductive 64B `RSP` alignment for Reactor ABI & `<tile>` checkpoints (§6.1):
    ; `call` (-8) + `push rbp` (-8) + `sub rsp, (64k + 48)` => `RSP ≡ 0 (mod 64)`!
    add     eax, 79
    and     eax, ~63
    sub     eax, 16
    jmp     .write_frame
.align16_frame:
    add     eax, 15
    and     eax, ~15
.write_frame:
    mov     rdx, [space_out]
    mov     [rdx + r12], eax

    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbx
    ret

compile_module:
    mov     dword [fn_cnt], 0
    mov     dword [call_fix_cnt], 0
    mov     dword [fnlen_fix_cnt], 0
    mov     dword [code_off], TARGET_HDR_LEN
    cmp     byte [mode_capsule], 1
    je      .hdr_cap
    cmp     byte [mode_efi], 1
    jne     .hdr_set
    mov     dword [code_off], PE_HDR_FILE_SIZE
    jmp     .hdr_set
.hdr_cap:
    mov     dword [code_off], ZCAP_HDR_SIZE
.hdr_set:
    mov     dword [tgt_ro_off], 0
    mov     dword [fixup_cnt], 0
    mov     byte  [in_fn_body], 0

    call    prescan_module
    call    prescan_fn_signatures
    cmp     byte [mode_capsule], 1
    je      .emit_cap_rt
    cmp     byte [mode_efi], 1
    je      .emit_efi_rt
    call    emit_target_runtime
    jmp     .start_fns
.emit_cap_rt:
    call    emit_capsule_target_runtime
    jmp     .start_fns
.emit_efi_rt:
    call    emit_efi_target_runtime
.start_fns:

    mov     dword [tok_idx], 0
.top_loop:
    call    peek_tok_ptr
    movzx   ecx, byte [rax]
    cmp     cl, TOK_EOF
    je      .resolve_calls

    cmp     cl, TOK_KW
    jne     .skip_non_fn
    cmp     byte [rax + 1], KW_FN
    jne     .skip_non_fn
    mov     dword [cur_fn_attr], 0
    call    parse_fn_decl
    jmp     .top_loop

.skip_non_fn:
    cmp     cl, TOK_AT
    je      .skip_at_decl
    add     dword [tok_idx], 2
    call    peek_tok_ptr
    cmp     byte [rax], TOK_LBRACE
    je      .sk_br
    cmp     byte [rax], TOK_AT
    je      .sk_ptr_decl
    cmp     byte [rax], TOK_LT
    jne     .sk_const
    call    skip_angle_block
    call    peek_tok_ptr
    cmp     byte [rax], TOK_LBRACE
    je      .sk_br
    jmp     .top_loop
.sk_ptr_decl:
    cmp     byte [rax + 16], TOK_KW
    jne     .sk_ptr_ident
    cmp     byte [rax + 17], KW_CELL64
    jne     .sk_const
    add     dword [tok_idx], 2
    jmp     .sk_br
.sk_ptr_ident:
    cmp     byte [rax + 16], TOK_IDENT
    jne     .sk_const
    mov     edx, [sym_ptr_id]
    cmp     [rax + 24], rdx
    jne     .sk_const
    add     dword [tok_idx], 2
    call    skip_angle_block
    jmp     .top_loop
.sk_br:
    call    skip_brace_block
    jmp     .top_loop
.sk_const:
    call    skip_const_rhs_tokens
    jmp     .top_loop
.skip_at_decl:
    cmp     byte [rax + 16], TOK_IDENT
    jne     .sk_at_space
    mov     edx, [rax + 24]
    cmp     edx, [sym_isr_id]
    je      .top_isr_fn
    cmp     edx, [sym_isr_ok_id]
    je      .top_isr_ok_fn
.sk_at_space:
    add     dword [tok_idx], 4
    call    skip_angle_block
    jmp     .top_loop
.top_isr_fn:
    add     dword [tok_idx], 2
    mov     dword [cur_fn_attr], 1
    call    parse_fn_decl
    jmp     .top_loop
.top_isr_ok_fn:
    add     dword [tok_idx], 2
    mov     dword [cur_fn_attr], 2
    call    parse_fn_decl
    jmp     .top_loop

.resolve_calls:
    mov     r8,  [space_ast]
    lea     r9,  [r8 + AST_CALLFIX_OFF]
    lea     r10, [r8 + AST_FN_OFF]
    mov     r11, [space_out]
    xor     ecx, ecx
    mov     edx, [call_fix_cnt]
.patch_calls:
    cmp     ecx, edx
    jae     .resolve_fnlens
    mov     edi, [r9 + rcx * 8]
    mov     esi, [r9 + rcx * 8 + 4]
    shl     esi, 4
    cmp     dword [r10 + rsi + 8], 1
    jne     err_undef_fn
    mov     eax, [r10 + rsi + 4]
    lea     r8d, [rdi + 4]
    sub     eax, r8d
    mov     [r11 + rdi], eax
    inc     ecx
    jmp     .patch_calls

.resolve_fnlens:
    mov     r8,  [space_ast]
    lea     r9,  [r8 + AST_FNLEN_FIX_OFF]
    lea     r10, [r8 + AST_FN_OFF]
    xor     ecx, ecx
    mov     edx, [fnlen_fix_cnt]
.patch_fnlens:
    cmp     ecx, edx
    jae     .done
    mov     edi, [r9 + rcx * 8]
    mov     esi, [r9 + rcx * 8 + 4]
    mov     eax, esi
    shl     eax, 4
    cmp     dword [r10 + rax + 8], 1
    jne     err_undef_fn
    mov     eax, [r8 + AST_FNLEN_OFF + rsi * 4]
    mov     [r11 + rdi], eax
    inc     ecx
    jmp     .patch_fnlens
.done:
    ret

parse_err_cur_tok:
    call    peek_tok_ptr
    jmp     parse_err_at_tok

; -----------------------------------------------------------------------------
; Context-Exact `< ... >` Clause Delimiter Scanners (§5.4)
; -----------------------------------------------------------------------------
; Returns `eax = 1` iff `rax` (`TOK_PIPE`) is followed by `TOK_IDENT` + `TOK_COLON`
is_field_sep_pipe:
    cmp     byte [rax + 16], TOK_IDENT
    jne     .no
    cmp     byte [rax + 32], TOK_COLON
    jne     .no
    mov     eax, 1
    ret
.no:
    xor     eax, eax
    ret

; Returns `eax = 1` if token at `[rax + 16]` is on the SAME line as `[rax]` AND
; can start a RHS operand of binary `>` (`ident`, `int`, `char`, `(`, `-`, `~`, `!`, `@`, `true`/`false`).
gt_followed_by_same_line_expr:
    mov     ecx, [rax + 4]          ; line of `>`
    cmp     ecx, [rax + 20]         ; line of next token
    jne     .no
    movzx   ecx, byte [rax + 16]    ; kind of next token
    cmp     cl, TOK_INT
    je      .yes
    cmp     cl, TOK_CHAR
    je      .yes
    cmp     cl, TOK_LPAREN
    je      .yes
    cmp     cl, TOK_MINUS
    je      .yes
    cmp     cl, TOK_TILDE
    je      .yes
    cmp     cl, TOK_BANG
    je      .yes
    cmp     cl, TOK_AT
    je      .yes
    cmp     cl, TOK_IDENT
    je      .chk_not_assign
    cmp     cl, TOK_KW
    jne     .no
    movzx   edx, byte [rax + 17]
    cmp     dl, KW_TRUE
    je      .yes
    cmp     dl, KW_FALSE
    je      .yes
    jmp     .no
.chk_not_assign:
    ; Exclude `x :=` / `x =` statement start on same line
    movzx   edx, byte [rax + 32]
    cmp     dl, TOK_WALRUS
    je      .no
    cmp     dl, TOK_ASSIGN
    je      .no
.yes:
    mov     eax, 1
    ret
.no:
    xor     eax, eax
    ret

; Delimiter Scanner #1: Bitpack Literals `Type<f1: e1 | f2: e2>` (§5.4)
advance_to_bitpack_delim:
    xor     r8d, r8d                ; paren depth
.loop:
    call    peek_tok_ptr
    movzx   ecx, byte [rax]
    test    cl, cl
    jz      parse_err_at_tok
    cmp     cl, TOK_LPAREN
    jne     .rp
    inc     r8d
    inc     dword [tok_idx]
    jmp     .loop
.rp:
    cmp     cl, TOK_RPAREN
    jne     .chk
    dec     r8d
    inc     dword [tok_idx]
    jmp     .loop
.chk:
    test    r8d, r8d
    jnz     .adv
    cmp     cl, TOK_PIPE
    jne     .chk_gt
    push    rax
    call    is_field_sep_pipe
    mov     edx, eax
    pop     rax
    test    edx, edx
    jnz     .found
    jmp     .adv
.chk_gt:
    cmp     cl, TOK_GT
    jne     .adv
    push    rax
    call    gt_followed_by_same_line_expr
    mov     edx, eax
    pop     rax
    test    edx, edx
    jz      .found                  ; next token cannot start same-line expr -> closes `>`!
.adv:
    inc     dword [tok_idx]
    jmp     .loop
.found:
    ret

parse_bitpack_clause_expr:
    push    rbx
    push    r12
    mov     ebx, [tok_idx]
    call    advance_to_bitpack_delim
    mov     r12, rax
    movzx   edx, byte [r12]
    push    rdx
    mov     byte [r12], TOK_EOF
    mov     [tok_idx], ebx
    mov     edi, 1
    call    parse_expr
    pop     rdx
    mov     [r12], dl
    pop     r12
    pop     rbx
    ret

; Delimiter Scanner #2: `loop <max: 4 | until: n | 1> {` (§5.4)
advance_to_loop_lens_delim:
    xor     r8d, r8d
.loop:
    call    peek_tok_ptr
    movzx   ecx, byte [rax]
    test    cl, cl
    jz      parse_err_at_tok
    cmp     cl, TOK_LPAREN
    jne     .rp
    inc     r8d
    inc     dword [tok_idx]
    jmp     .loop
.rp:
    cmp     cl, TOK_RPAREN
    jne     .chk
    dec     r8d
    inc     dword [tok_idx]
    jmp     .loop
.chk:
    test    r8d, r8d
    jnz     .adv
    cmp     cl, TOK_PIPE
    jne     .chk_gt
    push    rax
    call    is_field_sep_pipe       ; `|` closes clause ONLY before `ident :`!
    mov     edx, eax
    pop     rax
    test    edx, edx
    jnz     .found
    jmp     .adv
.chk_gt:
    cmp     cl, TOK_GT
    jne     .adv
    cmp     byte [rax + 16], TOK_LBRACE
    je      .found
.adv:
    inc     dword [tok_idx]
    jmp     .loop
.found:
    ret

parse_loop_lens_expr:
    push    rbx
    push    r12
    mov     ebx, [tok_idx]
    call    advance_to_loop_lens_delim
    mov     r12, rax
    movzx   edx, byte [r12]
    push    rdx
    mov     byte [r12], TOK_EOF
    mov     [tok_idx], ebx
    mov     edi, 1
    call    parse_expr
    pop     rdx
    mov     [r12], dl
    pop     r12
    pop     rbx
    ret
