; =============================================================================
; Zero Stage-0 Bootstrap — Type, Bit-Pack & Space Resolver (`bootstrap/types.inc`)
; =============================================================================
; Implements Multi-Stage Top-Level Prescan:
;   1. Rejects duplicate `::` declarations and unimplemented `@` spaces.
;   2. Resolves `NAME :: expr` constants in order-independent fixed-point order.
;   3. Resolves `Name :: <f1: u32 | ...>` bit-layouts (enforcing 8/16/32/64 sum).
;   4. Resolves `Name :: [<align: N>] { ... }` record structs with byte layout.
;   5. Allocates disjoint 64B-aligned `@slab` & `@arena` spaces in `0x10000000`.
; =============================================================================

; --- `space_ast` Partition Map (64 MiB Arena) ---
%define AST_CONST_OFF       0x000000    ; 2048 x 32B: [sym:u32, state:u32, val:i64, tok:u32, line:u32, pad:u64]
%define AST_TYPE_OFF        0x020000    ; 512  x 32B: [sym:u32, ukind:u8, fcnt:u8, align:u16, bsize:u32, ffirst:u32, tbits:u32, tok:u32, ...]
%define AST_FIELD_OFF       0x030000    ; 4096 x 16B: [sym:u32, off_or_shift:u32, width_or_bsize:u32, ty_tag:u32]
%define AST_SPACE_OFF       0x040000    ; 512  x 32B: [sym:u32, skind:u32, vaddr:u64, esize:u32, ety:u32, cap_or_tok:u64]
%define AST_FN_OFF          0x050000    ; 4096 x 16B: [sym:u32, code_off:u32, defined:u32, ret_ty:u32]
%define AST_CALLFIX_OFF     0x060000    ; 32768 x 8B: [patch_off:u32, fn_idx:u32]
%define AST_LOCAL_OFF       0x0A0000    ; 2048 x 16B: [sym:u32, rbp_disp:i32, ty_tag:u32, pad:u32]
%define AST_BRK_PATCH_OFF   0x0B0000    ; 32 x 64 x u32
%define AST_BRK_CNT_OFF     0x0B2000    ; 32 x u32
%define AST_CNT_PATCH_OFF   0x0B4000    ; 32 x 64 x u32
%define AST_CNT_CNT_OFF     0x0B6000    ; 32 x u32
%define AST_REC_PATCH_OFF   0x0B8000    ; 32 x 64 x u32 (active `recover` landing jumps)
%define AST_REC_CNT_OFF     0x0BA000    ; 32 x u32
%define AST_INTERP_BUF_OFF  0x0C0000    ; 64 KiB scratch buffer

; -----------------------------------------------------------------------------
; Lookup Helpers (`find_const`, `find_user_type`, `find_space`, `type_byte_size`)
; -----------------------------------------------------------------------------
; find_const_entry(edi = sym_id) -> rax = ptr to 32B ConstEntry, or 0
find_const_entry:
    mov     rcx, [space_ast]
    add     rcx, AST_CONST_OFF
    xor     edx, edx
    mov     r8d, [const_cnt]
.loop:
    cmp     edx, r8d
    jae     .none
    mov     r9d, edx
    shl     r9d, 5
    cmp     [rcx + r9], edi
    je      .hit
    inc     edx
    jmp     .loop
.hit:
    lea     rax, [rcx + r9]
    ret
.none:
    xor     eax, eax
    ret

; find_const(edi = sym_id) -> eax = 1 if resolved (rdx = i64 val), else 0
find_const:
    call    find_const_entry
    test    rax, rax
    jz      .no
    cmp     dword [rax + 4], 1      ; resolved == 1?
    jne     .no
    mov     rdx, [rax + 8]
    mov     eax, 1
    ret
.no:
    xor     eax, eax
    ret

; find_user_type(edi = sym_id) -> eax = type_idx (0..N), or -1 if not found
find_user_type:
    mov     rcx, [space_ast]
    add     rcx, AST_TYPE_OFF
    xor     eax, eax
    mov     r8d, [type_cnt]
.loop:
    cmp     eax, r8d
    jae     .none
    mov     r9d, eax
    shl     r9d, 5
    cmp     [rcx + r9], edi
    je      .done
    inc     eax
    jmp     .loop
.none:
    mov     eax, -1
.done:
    ret

; find_space(edi = sym_id) -> rax = ptr to 32B SpaceEntry, or 0
find_space:
    mov     rcx, [space_ast]
    add     rcx, AST_SPACE_OFF
    xor     edx, edx
    mov     r8d, [space_cnt]
.loop:
    cmp     edx, r8d
    jae     .none
    mov     r9d, edx
    shl     r9d, 5
    cmp     [rcx + r9], edi
    je      .hit
    inc     edx
    jmp     .loop
.hit:
    lea     rax, [rcx + r9]
    ret
.none:
    xor     eax, eax
    ret

; check_top_name_unique(edi = sym_id, esi = line) — rejects duplicate `::` / `@`
check_top_name_unique:
    push    rdi
    push    rsi
    call    find_const_entry
    test    rax, rax
    jnz     .dup
    mov     rdi, [rsp + 8]
    call    find_user_type
    cmp     eax, -1
    jne     .dup
    mov     rdi, [rsp + 8]
    call    find_space
    test    rax, rax
    jnz     .dup
    pop     rsi
    pop     rdi
    ret
.dup:
    pop     rsi
    pop     rdi
    lea     rdi, [msg_dup_sym]
    mov     edx, msg_dup_sym_len
    jmp     err_msg_at_line

; Returns `eax = byte_size`, `edx = natural_align` for `edi = TY_*`
type_size_align:
    test    edi, TY_FLAG_SPAN
    jz      .chk_view
    mov     eax, 16
    mov     edx, 8
    ret
.chk_view:
    test    edi, TY_FLAG_VIEW_ONE | TY_FLAG_ATOMIC
    jz      .base_kind
    mov     eax, 8
    mov     edx, 8
    ret
.base_kind:
    and     edi, TY_BASE_MASK
    cmp     edi, TY_CUSTOM_PTR_BASE
    jae     .s8_ptr
    cmp     edi, TY_STRUCT_BASE
    jae     .is_struct
    cmp     edi, TY_BITPACK_BASE
    jae     .is_bp
    cmp     edi, TY_U8
    je      .s1
    cmp     edi, TY_I8
    je      .s1
    cmp     edi, TY_U16
    je      .s2
    cmp     edi, TY_I16
    je      .s2
    cmp     edi, TY_U32
    je      .s4
    cmp     edi, TY_I32
    je      .s4
.s8_ptr:
    mov     eax, 8
    mov     edx, 8
    ret
.s1:
    mov     eax, 1
    mov     edx, 1
    ret
.s2:
    mov     eax, 2
    mov     edx, 2
    ret
.s4:
    mov     eax, 4
    mov     edx, 4
    ret
.is_bp:
    sub     edi, TY_BITPACK_BASE
    shl     edi, 5
    mov     rcx, [space_ast]
    mov     eax, [rcx + AST_TYPE_OFF + rdi + 8]
    mov     edx, eax
    ret
.is_struct:
    sub     edi, TY_STRUCT_BASE
    shl     edi, 5
    mov     rcx, [space_ast]
    mov     eax, [rcx + AST_TYPE_OFF + rdi + 8]
    movzx   edx, word [rcx + AST_TYPE_OFF + rdi + 6]
    ret

; Parses a field/element type token into `eax = TY_*` tag
parse_resolved_type_tag:
    call    next_tok_ptr
    movzx   ecx, byte [rax]
    cmp     cl, TOK_LBRACK
    je      .slice_brack
    cmp     cl, TOK_KW
    je      .prim_kw
    cmp     cl, TOK_IDENT
    jne     parse_err_at_tok
    push    rax
    mov     edi, [rax + 8]
    call    find_user_type
    pop     rcx
    cmp     eax, -1
    je      .bad_ty
    mov     edx, eax
    shl     edx, 5
    mov     r8, [space_ast]
    movzx   edx, byte [r8 + AST_TYPE_OFF + rdx + 4]
    cmp     dl, UKIND_BITPACK
    je      .ret_bp
    cmp     dl, UKIND_PTR
    je      .ret_ptr
    add     eax, TY_STRUCT_BASE
    ret
.ret_bp:
    add     eax, TY_BITPACK_BASE
    ret
.ret_ptr:
    add     eax, TY_CUSTOM_PTR_BASE
    ret
.slice_brack:
    mov     al, TOK_RBRACK
    call    expect_tok_kind
    call    parse_resolved_type_tag
    or      eax, TY_FLAG_SPAN
    ret
.bad_ty:
    mov     rax, rcx
    jmp     parse_err_at_tok

.prim_kw:
    movzx   edx, byte [rax + 1]
    cmp     dl, KW_STR
    je      .ret_u8_span
    cmp     dl, KW_BYTES
    je      .ret_u8_span
    cmp     dl, KW_U8
    mov     eax, TY_U8
    je      .ok
    cmp     dl, KW_BOOL
    je      .ok
    cmp     dl, KW_BIT
    je      .ok
    cmp     dl, KW_I8
    mov     eax, TY_I8
    je      .ok
    cmp     dl, KW_U16
    mov     eax, TY_U16
    je      .ok
    cmp     dl, KW_I16
    mov     eax, TY_I16
    je      .ok
    cmp     dl, KW_U32
    mov     eax, TY_U32
    je      .ok
    cmp     dl, KW_I32
    mov     eax, TY_I32
    je      .ok
    cmp     dl, KW_U64
    mov     eax, TY_U64
    je      .ok
    cmp     dl, KW_MASK64
    je      .ok
    cmp     dl, KW_I64
    mov     eax, TY_I64
    je      .ok
    jmp     parse_err_at_tok
.ret_u8_span:
    mov     eax, TY_U8 | TY_FLAG_SPAN
.ok:
    ret

; -----------------------------------------------------------------------------
; Bit-Width Parser for `<field: WidthType | ...>` (`1..64` bits)
; Accepts `bit`/`bool` (1), `u8..u64`/`i8..i64`, `u1..u64`/`b1..b64`, or integer `1..64`
; -----------------------------------------------------------------------------
parse_bit_width_spec:
    call    next_tok_ptr
    movzx   ecx, byte [rax]
    cmp     cl, TOK_INT
    je      .from_int
    cmp     cl, TOK_KW
    je      .from_kw
    cmp     cl, TOK_IDENT
    je      .from_ident
    jmp     parse_err_at_tok

.from_int:
    mov     rcx, [rax + 8]
    cmp     rcx, 1
    jb      parse_err_at_tok
    cmp     rcx, 64
    ja      parse_err_at_tok
    mov     eax, ecx
    ret

.from_kw:
    movzx   edx, byte [rax + 1]
    cmp     dl, KW_BIT
    mov     ecx, 1
    je      .ret_c
    cmp     dl, KW_BOOL
    je      .ret_c
    cmp     dl, KW_U8
    mov     ecx, 8
    je      .ret_c
    cmp     dl, KW_I8
    je      .ret_c
    cmp     dl, KW_U16
    mov     ecx, 16
    je      .ret_c
    cmp     dl, KW_I16
    je      .ret_c
    cmp     dl, KW_U32
    mov     ecx, 32
    je      .ret_c
    cmp     dl, KW_I32
    je      .ret_c
    cmp     dl, KW_U64
    mov     ecx, 64
    je      .ret_c
    cmp     dl, KW_I64
    je      .ret_c
    jmp     parse_err_at_tok
.ret_c:
    mov     eax, ecx
    ret

.from_ident:
    ; Parse `u<N>`, `i<N>`, or `b<N>` from SymMeta string (`1..64`)
    mov     edx, [rax + 8]          ; sym_id
    shl     edx, 4
    mov     r8,  [space_sym]
    mov     rsi, [r8 + SYM_META_OFF + rdx]      ; ptr
    mov     ecx, [r8 + SYM_META_OFF + rdx + 8]  ; len
    cmp     ecx, 2
    jb      parse_err_at_tok
    cmp     ecx, 3
    ja      parse_err_at_tok
    movzx   edx, byte [rsi]
    cmp     dl, 'u'
    je      .parse_digits
    cmp     dl, 'i'
    je      .parse_digits
    cmp     dl, 'b'
    jne     parse_err_at_tok
.parse_digits:
    xor     edi, edi
    mov     r9d, 1
.d_loop:
    cmp     r9d, ecx
    jae     .chk_range
    movzx   edx, byte [rsi + r9]
    cmp     dl, '0'
    jb      parse_err_at_tok
    cmp     dl, '9'
    ja      parse_err_at_tok
    sub     dl, '0'
    imul    edi, edi, 10
    add     edi, edx
    inc     r9d
    jmp     .d_loop
.chk_range:
    cmp     edi, 1
    jb      parse_err_at_tok
    cmp     edi, 64
    ja      parse_err_at_tok
    mov     eax, edi
    ret

; -----------------------------------------------------------------------------
; Non-Failing Trial Constant Evaluator (for order-independent `::` resolution)
; Sets `[const_unresolved_hit] = 1` if an unresolved symbol is encountered.
; -----------------------------------------------------------------------------
try_eval_const_at:                  ; edi = start tok_idx -> eax=1 ok (rdx=val), 0 wait
    mov     [tok_idx], edi
    mov     byte [const_unresolved_hit], 0
    call    eval_const_expr
    cmp     byte [const_unresolved_hit], 0
    jne     .pending
    mov     rdx, rax
    mov     eax, 1
    ret
.pending:
    xor     eax, eax
    ret

; -----------------------------------------------------------------------------
; Balanced Brace Skipper (`{ ... }`)
; -----------------------------------------------------------------------------
skip_brace_block:
    mov     al, TOK_LBRACE
    call    expect_tok_kind
    mov     ecx, 1
.loop:
    call    next_tok_ptr
    movzx   edx, byte [rax]
    test    dl, dl
    jz      parse_err_at_tok
    cmp     dl, TOK_LBRACE
    jne     .chk_close
    inc     ecx
    jmp     .loop
.chk_close:
    cmp     dl, TOK_RBRACE
    jne     .loop
    dec     ecx
    jnz     .loop
    ret

; Skips constant or `=>` arrow expression tokens until next top-level declaration
skip_const_rhs_tokens:
    xor     ecx, ecx                ; () / [] / {} nesting depth
.loop:
    call    peek_tok_ptr
    movzx   edx, byte [rax]
    test    dl, dl
    jz      .done
    cmp     dl, TOK_LPAREN
    je      .inc_d
    cmp     dl, TOK_LBRACK
    je      .inc_d
    cmp     dl, TOK_LBRACE
    je      .inc_d
    cmp     dl, TOK_RPAREN
    je      .dec_d
    cmp     dl, TOK_RBRACK
    je      .dec_d
    cmp     dl, TOK_RBRACE
    je      .dec_d
    jmp     .chk_top
.inc_d:
    inc     ecx
    inc     dword [tok_idx]
    jmp     .loop
.dec_d:
    test    ecx, ecx
    jz      .chk_top
    dec     ecx
    inc     dword [tok_idx]
    jmp     .loop
.chk_top:
    test    ecx, ecx
    jnz     .adv
    ; Top-level `@` declarations: `@ <kind> <ident> :` OR `@ <attr> fn`
    cmp     dl, TOK_AT
    jne     .chk_kw_fn
    cmp     byte [rax + 32], TOK_KW
    jne     .chk_at_sp
    cmp     byte [rax + 33], KW_FN
    je      .done
.chk_at_sp:
    cmp     byte [rax + 32], TOK_IDENT
    jne     .adv
    cmp     byte [rax + 48], TOK_COLON
    je      .done
    jmp     .adv
.chk_kw_fn:
    cmp     dl, TOK_KW
    jne     .chk_id_decl
    cmp     byte [rax + 1], KW_FN
    je      .done
    jmp     .adv
.chk_id_decl:
    cmp     dl, TOK_IDENT
    jne     .adv
    cmp     byte [rax + 16], TOK_COLON2
    je      .done
.adv:
    inc     dword [tok_idx]
    jmp     .loop
.done:
    ret

; Fixed-point constant solver (`edi = 1` fatal if stuck, `0` return pending flag)
run_const_fixpoint:
    push    rbx
    push    r12
    push    r13
    push    r14
    push    rdi                     ; [rsp] = fatal_if_stuck
    mov     byte [const_trial_mode], 1
.cf_pass:
    xor     r12d, r12d              ; r12d = unresolved count
    xor     r13d, r13d              ; r13d = newly resolved progress count
    xor     ebx,  ebx               ; index 0..const_cnt

.cf_item:
    cmp     ebx, [const_cnt]
    jae     .cf_check_pass
    mov     eax, ebx
    shl     eax, 5
    mov     rcx, [space_ast]
    lea     r14, [rcx + AST_CONST_OFF + rax]
    cmp     dword [r14 + 4], 1
    je      .cf_next

    inc     r12d
    mov     edi, [r14 + 16]         ; expr_tok_idx
    call    try_eval_const_at
    test    eax, eax
    jz      .cf_next

    mov     [r14 + 8], rdx          ; resolved value!
    mov     dword [r14 + 4], 1
    dec     r12d
    inc     r13d

.cf_next:
    inc     ebx
    jmp     .cf_item

.cf_check_pass:
    test    r12d, r12d
    jz      .cf_done_clean
    test    r13d, r13d
    jnz     .cf_pass

    cmp     qword [rsp], 0
    je      .cf_done_pending

    ; Unresolvable / cyclic constant -> find first unresolved line and fault!
    xor     ebx, ebx
.find_bad_const:
    mov     eax, ebx
    shl     eax, 5
    mov     rcx, [space_ast]
    cmp     dword [rcx + AST_CONST_OFF + rax + 4], 0
    je      .die_bad_const
    inc     ebx
    jmp     .find_bad_const
.die_bad_const:
    mov     esi, [rcx + AST_CONST_OFF + rax + 20]
    lea     rdi, [msg_bad_const]
    mov     edx, msg_bad_const_len
    jmp     err_msg_at_line

.cf_done_pending:
    mov     byte [const_trial_mode], 0
    mov     eax, 1
    add     rsp, 8
    pop     r14
    pop     r13
    pop     r12
    pop     rbx
    ret

.cf_done_clean:
    mov     byte [const_trial_mode], 0
    xor     eax, eax
    add     rsp, 8
    pop     r14
    pop     r13
    pop     r12
    pop     rbx
    ret

; -----------------------------------------------------------------------------
; Top-Level Prescan & Resolution Pass (`prescan_module`)
; -----------------------------------------------------------------------------
prescan_module:
    push    rbx
    push    r12
    push    r13
    push    r14
    push    r15

    mov     dword [tok_idx], 0
    mov     dword [const_cnt], 0
    mov     dword [type_cnt], 0
    mov     dword [field_cnt], 0
    mov     dword [space_cnt], 0
    mov     qword [data_bss_off], 64    ; [0..64) reserved for fault/telemetry line
    cmp     byte [mode_efi], 1
    jne     .scan_loop
    mov     qword [data_bss_off], EFI_BSS_USER_START ; 4KiB-aligned after handoff & stack

    ; === Stage 1: Inventory all top-level `::` and `@` declarations ===
.scan_loop:
    call    peek_tok_ptr
    movzx   ecx, byte [rax]
    cmp     cl, TOK_EOF
    je      .stage_bitpacks

    cmp     cl, TOK_KW
    je      .scan_kw
    cmp     cl, TOK_AT
    je      .scan_at
    cmp     cl, TOK_IDENT
    je      .scan_ident_decl
    jmp     parse_err_at_tok

.scan_kw:
    cmp     byte [rax + 1], KW_FN
    jne     parse_err_at_tok
    inc     dword [tok_idx]         ; skip `fn`
    call    next_tok_ptr            ; fn name
    cmp     byte [rax], TOK_IDENT
    jne     parse_err_at_tok
    ; Skip `( ... )` parameter list
    mov     al, TOK_LPAREN
    call    expect_tok_kind
    mov     ecx, 1
.skip_fn_params:
    call    next_tok_ptr
    movzx   edx, byte [rax]
    test    dl, dl
    jz      parse_err_at_tok
    cmp     dl, TOK_LPAREN
    jne     .sfp_rp
    inc     ecx
    jmp     .skip_fn_params
.sfp_rp:
    cmp     dl, TOK_RPAREN
    jne     .skip_fn_params
    dec     ecx
    jnz     .skip_fn_params

    ; Skip optional return type before `{`
.skip_fn_ret:
    call    peek_tok_ptr
    movzx   edx, byte [rax]
    test    dl, dl
    jz      parse_err_at_tok
    cmp     dl, TOK_LBRACE
    je      .skip_fn_body
    cmp     dl, TOK_FAT_ARROW
    je      .skip_fn_arrow
    inc     dword [tok_idx]
    jmp     .skip_fn_ret
.skip_fn_arrow:
    inc     dword [tok_idx]
    call    skip_const_rhs_tokens
    jmp     .scan_loop
.skip_fn_body:
    call    skip_brace_block
    jmp     .scan_loop

.scan_at:
    mov     r14d, [rax + 4]         ; line number
    inc     dword [tok_idx]         ; consume `@`
    call    next_tok_ptr
    cmp     byte [rax], TOK_KW
    jne     .chk_at_ident
    movzx   r12d, byte [rax + 1]    ; KW_SLAB, KW_ARENA, or KW_RING
    cmp     r12d, KW_SLAB
    je      .at_ok_kw
    cmp     r12d, KW_ARENA
    je      .at_ok_kw
    cmp     r12d, KW_RING
    je      .at_ok_kw
    jmp     .unimpl_space
.chk_at_ident:
    cmp     byte [rax], TOK_IDENT
    jne     .unimpl_space
    mov     edx, [rax + 8]
    cmp     edx, [sym_isr_id]
    je      .at_fn_attr
    cmp     edx, [sym_isr_ok_id]
    je      .at_fn_attr
    cmp     edx, [sym_dma_queue_id]
    je      .at_is_dmaq
    cmp     edx, [sym_capsule_id]
    je      .at_is_capsule
    cmp     edx, [sym_port_id]
    jne     .unimpl_space
    cmp     byte [mode_efi], 1
    je      .at_is_port
.err_at_need_efi:
    mov     esi, r14d
    lea     rdi, [msg_need_efi]
    mov     edx, msg_need_efi_len
    jmp     err_msg_at_line
.at_fn_attr:
    cmp     byte [mode_efi], 1
    jne     .err_at_need_efi
    call    peek_tok_ptr
    cmp     byte [rax], TOK_KW
    jne     parse_err_at_tok
    cmp     byte [rax + 1], KW_FN
    jne     parse_err_at_tok
    jmp     .scan_kw
.at_is_capsule:
    mov     r12d, 0xFD
    jmp     .at_ok_kw
.at_is_dmaq:
    mov     r12d, 0xFE
    jmp     .at_ok_kw
.at_is_port:
    mov     r12d, 0xFF
.at_ok_kw:
    cmp     byte [mode_capsule], 1
    jne     .at_not_in_cap
    cmp     r12d, 0xFF              ; `@port` has no `.bss` state, allowed; mutable `.bss` spaces forbidden in `.zcap`!
    je      .at_not_in_cap
    mov     esi, r14d
    lea     rdi, [msg_bad_capsule]
    mov     edx, msg_bad_capsule_len
    jmp     err_msg_at_line
.at_not_in_cap:
    call    next_tok_ptr            ; space identifier
    cmp     byte [rax], TOK_IDENT
    jne     parse_err_at_tok
    mov     edi, [rax + 8]          ; sym_id
    mov     esi, [rax + 4]          ; line
    mov     r13d, edi
    call    check_top_name_unique

    mov     al, TOK_COLON
    call    expect_tok_kind

    ; Record SpaceEntry with deferred `<...>` tok_idx
    mov     eax, [space_cnt]
    inc     dword [space_cnt]
    shl     eax, 5
    mov     rcx, [space_ast]
    add     rcx, AST_SPACE_OFF
    mov     [rcx + rax], r13d       ; sym_id
    mov     edx, SKIND_SLAB
    cmp     r12d, KW_SLAB
    je      .set_sk
    mov     edx, SKIND_ARENA
    cmp     r12d, KW_ARENA
    je      .set_sk
    mov     edx, SKIND_RING
    cmp     r12d, KW_RING
    je      .set_sk
    mov     edx, SKIND_DMA_QUEUE
    cmp     r12d, 0xFE
    je      .set_sk
    mov     edx, SKIND_CAPSULE
    cmp     r12d, 0xFD
    je      .set_sk
    mov     edx, SKIND_PORT
.set_sk:
    mov     [rcx + rax + 4], edx    ; skind
    mov     edx, [tok_idx]
    mov     [rcx + rax + 24], rdx   ; save `<...>` start tok_idx temporarily
    call    skip_angle_block
    jmp     .scan_loop

.unimpl_space:
    mov     esi, r14d
    lea     rdi, [msg_unimpl_space]
    mov     edx, msg_unimpl_space_len
    jmp     err_msg_at_line

.scan_ident_decl:
    mov     r13d, [rax + 8]         ; sym_id
    mov     r14d, [rax + 4]         ; decl_line
    inc     dword [tok_idx]
    mov     al, TOK_COLON2
    call    expect_tok_kind

    mov     edi, r13d
    mov     esi, r14d
    call    check_top_name_unique

    call    peek_tok_ptr
    movzx   ecx, byte [rax]
    cmp     cl, TOK_LBRACE
    je      .record_struct_decl
    cmp     cl, TOK_AT
    je      .chk_ptr_decl
    cmp     cl, TOK_LT
    jne     .record_const_decl
    jmp     .disambig_lt

.chk_ptr_decl:
    cmp     byte [rax + 16], TOK_KW
    jne     .chk_ptr_ident
    cmp     byte [rax + 17], KW_CELL64
    jne     .record_const_decl
    add     dword [tok_idx], 2      ; consume `@` and `cell64`
    mov     r15d, [tok_idx]         ; `{` tok_idx
    mov     eax, [type_cnt]
    inc     dword [type_cnt]
    shl     eax, 5
    mov     rcx, [space_ast]
    add     rcx, AST_TYPE_OFF
    mov     [rcx + rax], r13d
    mov     byte  [rcx + rax + 4],  UKIND_STRUCT
    mov     word  [rcx + rax + 6],  64
    mov     dword [rcx + rax + 8],  64
    mov     dword [rcx + rax + 16], 1       ; `1` = `@cell64` hardware 64B cacheline!
    mov     [rcx + rax + 20], r15d
    mov     [rcx + rax + 24], r14d
    call    skip_brace_block
    jmp     .scan_loop
.chk_ptr_ident:
    cmp     byte [rax + 16], TOK_IDENT
    jne     .record_const_decl
    mov     edx, [sym_ptr_id]
    cmp     [rax + 24], rdx
    jne     .record_const_decl
    add     dword [tok_idx], 2      ; consume `@` and `ptr`
    mov     r15d, [tok_idx]         ; `<` tok_idx
    call    skip_angle_block
    mov     eax, [type_cnt]
    inc     dword [type_cnt]
    shl     eax, 5
    mov     rcx, [space_ast]
    add     rcx, AST_TYPE_OFF
    mov     [rcx + rax], r13d
    mov     byte [rcx + rax + 4], UKIND_PTR
    mov     word [rcx + rax + 6], 8
    mov     dword [rcx + rax + 8], 8
    mov     [rcx + rax + 20], r15d
    mov     [rcx + rax + 24], r14d
    jmp     .scan_loop

.disambig_lt:
    ; Disambiguate `Name :: <align: 64B> { ... }` (Struct) vs `Name :: <f: u32 | ...>` (Bitpack)!
    mov     r15d, [tok_idx]         ; save `<` tok_idx
    call    skip_angle_block
    call    peek_tok_ptr
    cmp     byte [rax], TOK_LBRACE
    je      .record_aligned_struct

    ; --- Bit-Partitioned Type (`UKIND_BITPACK`) ---
    mov     eax, [type_cnt]
    inc     dword [type_cnt]
    shl     eax, 5
    mov     rcx, [space_ast]
    add     rcx, AST_TYPE_OFF
    mov     [rcx + rax], r13d               ; sym_id
    mov     byte [rcx + rax + 4], UKIND_BITPACK
    mov     word [rcx + rax + 6], 0         ; align = 0 until resolved
    mov     dword [rcx + rax + 8], 0        ; bsize = 0 until resolved
    mov     [rcx + rax + 20], r15d          ; saved `<` tok_idx
    mov     [rcx + rax + 24], r14d          ; decl_line
    jmp     .scan_loop

.record_aligned_struct:
    mov     eax, [type_cnt]
    inc     dword [type_cnt]
    shl     eax, 5
    mov     rcx, [space_ast]
    add     rcx, AST_TYPE_OFF
    mov     [rcx + rax], r13d
    mov     byte [rcx + rax + 4], UKIND_STRUCT
    mov     word [rcx + rax + 6], 0         ; align = 0 until resolved
    mov     dword [rcx + rax + 8], 0        ; bsize = 0 until resolved
    mov     [rcx + rax + 20], r15d          ; saved `<align:...>` tok_idx
    mov     [rcx + rax + 24], r14d
    call    skip_brace_block
    jmp     .scan_loop

.record_struct_decl:
    mov     r15d, [tok_idx]
    mov     eax, [type_cnt]
    inc     dword [type_cnt]
    shl     eax, 5
    mov     rcx, [space_ast]
    add     rcx, AST_TYPE_OFF
    mov     [rcx + rax], r13d
    mov     byte [rcx + rax + 4], UKIND_STRUCT
    mov     word [rcx + rax + 6], 0         ; align = 0 until resolved
    mov     dword [rcx + rax + 8], 0        ; bsize = 0 until resolved
    mov     [rcx + rax + 20], r15d          ; `{` tok_idx
    mov     [rcx + rax + 24], r14d
    call    skip_brace_block
    jmp     .scan_loop

.record_const_decl:
    mov     r15d, [tok_idx]
    mov     eax, [const_cnt]
    inc     dword [const_cnt]
    shl     eax, 5
    mov     rcx, [space_ast]
    add     rcx, AST_CONST_OFF
    mov     [rcx + rax],      r13d          ; sym_id
    mov     dword [rcx + rax + 4], 0        ; resolved = 0
    mov     [rcx + rax + 16], r15d          ; expr_tok_idx
    mov     [rcx + rax + 20], r14d          ; decl_line
    call    skip_const_rhs_tokens
    jmp     .scan_loop

    ; === Stage 3 First: Bit-Partitioned Layouts (`UKIND_BITPACK`) ===
.stage_bitpacks:
    xor     ebx, ebx
.bp_loop:
    cmp     ebx, [type_cnt]
    jae     .stage_consts_early
    mov     eax, ebx
    shl     eax, 5
    mov     rcx, [space_ast]
    lea     r14, [rcx + AST_TYPE_OFF + rax]
    cmp     byte [r14 + 4], UKIND_BITPACK
    jne     .bp_next

    mov     eax, [r14 + 20]
    mov     [tok_idx], eax          ; `<` token
    mov     al, TOK_LT
    call    expect_tok_kind

    mov     eax, [field_cnt]
    mov     [r14 + 12], eax         ; field_first
    xor     r12d, r12d              ; r12d = field count in this bitpack
    xor     r13d, r13d              ; r13d = total bit width sum

.bp_field:
    call    next_tok_ptr
    cmp     byte [rax], TOK_IDENT
    jne     parse_err_at_tok
    mov     r15d, [rax + 8]         ; field sym_id

    mov     al, TOK_COLON
    call    expect_tok_kind
    call    parse_bit_width_spec    ; eax = bit width (1..64)

    mov     edx, [field_cnt]
    inc     dword [field_cnt]
    shl     edx, 4
    mov     rcx, [space_ast]
    add     rcx, AST_FIELD_OFF
    mov     [rcx + rdx],     r15d   ; sym_id
    mov     [rcx + rdx + 8], eax    ; width in bits

    add     r13d, eax
    inc     r12d

    call    peek_tok_ptr
    cmp     byte [rax], TOK_PIPE
    jne     .bp_end
    inc     dword [tok_idx]
    jmp     .bp_field

.bp_end:
    mov     al, TOK_GT
    call    expect_tok_kind

    ; Enforce static rule: total bits MUST equal 8, 16, 32, or 64!
    cmp     r13d, 8
    je      .bp_valid_w
    cmp     r13d, 16
    je      .bp_valid_w
    cmp     r13d, 32
    je      .bp_valid_w
    cmp     r13d, 64
    je      .bp_valid_w

    mov     esi, [r14 + 24]         ; decl_line
    lea     rdi, [msg_bad_bp_width]
    mov     edx, msg_bad_bp_width_len
    jmp     err_msg_at_line

.bp_valid_w:
    mov     [r14 + 5],  r12b        ; field_cnt
    mov     [r14 + 16], r13d        ; total_bits
    mov     eax, r13d
    shr     eax, 3                  ; byte_size = total_bits / 8 (1, 2, 4, 8)
    mov     [r14 + 8],  eax
    mov     [r14 + 6],  ax          ; align_bytes = byte_size

    ; Compute MSB-to-LSB shifts for each field
    mov     edx, r13d               ; running bit top = total_bits
    mov     ecx, [r14 + 12]         ; field_first
    xor     esi, esi
    mov     r8, [space_ast]
    add     r8, AST_FIELD_OFF
.bp_shifts:
    cmp     esi, r12d
    jae     .bp_next
    mov     edi, ecx
    add     edi, esi
    shl     edi, 4
    sub     edx, [r8 + rdi + 8]     ; running_top -= width
    mov     [r8 + rdi + 4], edx     ; shift = running_top!
    inc     esi
    jmp     .bp_shifts

.bp_next:
    inc     ebx
    jmp     .bp_loop

    ; === Stage 2a: Early Fixed-Point Constant Resolution (non-fatal if struct-dependent) ===
.stage_consts_early:
    xor     edi, edi
    call    run_const_fixpoint
    push    rax                     ; remember if Pass 2b needed (non-zero => pending)

    ; === Stage 4: Resolve Record Struct Layouts (`UKIND_STRUCT`) ===
.struct_loop_init:
    xor     ebx, ebx
.st_loop:
    cmp     ebx, [type_cnt]
    jae     .stage_consts_late
    mov     eax, ebx
    shl     eax, 5
    mov     rcx, [space_ast]
    lea     r14, [rcx + AST_TYPE_OFF + rax]
    cmp     byte [r14 + 4], UKIND_STRUCT
    jne     .st_next

    mov     eax, [r14 + 20]
    mov     [tok_idx], eax
    mov     r15d, 1                 ; r15d = max alignment (starts at 1)
    cmp     dword [r14 + 16], 1     ; `@cell64`?
    jne     .st_chk_lt
    mov     r15d, 64
.st_chk_lt:
    xor     edx, edx                ; edx = 1 if `<packed>`

    ; Optional `<packed>` or `<align: Bytes>`
    call    peek_tok_ptr
    cmp     byte [rax], TOK_LT
    jne     .st_open_brace
    inc     dword [tok_idx]
    call    next_tok_ptr
    cmp     byte [rax], TOK_IDENT
    jne     parse_err_at_tok
    mov     ecx, [rax + 8]
    cmp     ecx, [sym_packed_id]
    je      .st_packed
    cmp     ecx, [sym_align_id]
    jne     parse_err_at_tok
    mov     al, TOK_COLON
    call    expect_tok_kind
    call    parse_const_lens_expr
    mov     r15d, eax               ; explicit alignment
    mov     al, TOK_GT
    call    expect_tok_kind
    xor     edx, edx
    jmp     .st_open_brace
.st_packed:
    mov     al, TOK_GT
    call    expect_tok_kind
    mov     edx, 1

.st_open_brace:
    push    rdx                     ; [rsp] = is_packed
    mov     al, TOK_LBRACE
    call    expect_tok_kind

    mov     eax, [field_cnt]
    mov     [r14 + 12], eax         ; field_first
    xor     r12d, r12d              ; r12d = field_cnt
    xor     r13d, r13d              ; r13d = current byte offset

.st_field_loop:
    call    peek_tok_ptr
    cmp     byte [rax], TOK_RBRACE
    je      .st_finish

    ; Reject `@cold` / field attributes until layout-splitting pass exists
    cmp     byte [rax], TOK_AT
    jne     .st_fname
    mov     esi, [rax + 4]
    lea     rdi, [msg_unimpl_cold]
    mov     edx, msg_unimpl_cold_len
    jmp     err_msg_at_line

.st_fname:
    call    next_tok_ptr
    cmp     byte [rax], TOK_IDENT
    jne     parse_err_at_tok
    push    qword [rax + 8]         ; push field sym_id

    mov     al, TOK_COLON
    call    expect_tok_kind
    call    parse_resolved_type_tag ; eax = ty_tag
    mov     edi, eax
    push    rdi                     ; push ty_tag
    call    type_size_align         ; eax = f_size, edx = f_align

    ; `[rsp + 16]` is `is_packed` (`+0`: ty_tag, `+8`: field_sym_id, `+16`: is_packed)
    cmp     qword [rsp + 16], 1
    je      .st_packed_field

    ; Update struct max_align
    cmp     edx, r15d
    jbe     .st_align_off
    mov     r15d, edx
.st_align_off:
    ; Align `r13d` up to `edx` (power-of-2 alignment)
    lea     ecx, [rdx - 1]
    add     r13d, ecx
    not     ecx
    and     r13d, ecx
.st_packed_field:

    pop     rsi                     ; rsi = ty_tag
    pop     rdi                     ; rdi = field sym_id

    mov     ecx, [field_cnt]
    inc     dword [field_cnt]
    shl     ecx, 4
    mov     r8, [space_ast]
    add     r8, AST_FIELD_OFF
    mov     [r8 + rcx],      edi    ; sym_id
    mov     [r8 + rcx + 4],  r13d   ; byte offset
    mov     [r8 + rcx + 8],  eax    ; byte size
    mov     [r8 + rcx + 12], esi    ; ty_tag

    add     r13d, eax
    inc     r12d

    ; Optional comma separator between fields
    call    peek_tok_ptr
    cmp     byte [rax], TOK_COMMA
    jne     .st_field_loop
    inc     dword [tok_idx]
    jmp     .st_field_loop

.st_finish:
    inc     dword [tok_idx]         ; consume '}'
    pop     rdx                     ; is_packed
    cmp     dword [r14 + 16], 1     ; `@cell64`?
    jne     .st_norm_fin
    cmp     r13d, 64
    ja      .err_cell64_sz
    mov     r13d, 64                ; auto-pad `@cell64` to exact 64B cacheline
    mov     r15d, 64
    jmp     .st_save
.err_cell64_sz:
    mov     esi, [r14 + 24]
    lea     rdi, [msg_bad_cell64]
    mov     edx, msg_bad_cell64_len
    jmp     err_msg_at_line
.st_norm_fin:
    cmp     edx, 1
    je      .st_chk_empty
    ; Round total struct size up to `r15d` (`max_align`)
    lea     ecx, [r15 - 1]
    add     r13d, ecx
    not     ecx
    and     r13d, ecx
.st_chk_empty:
    test    r13d, r13d
    jnz     .st_save
    mov     r13d, 1
.st_save:
    mov     [r14 + 5], r12b         ; field_cnt
    mov     [r14 + 6], r15w         ; align_bytes
    mov     [r14 + 8], r13d         ; byte_size

.st_next:
    inc     ebx
    jmp     .st_loop

    ; === Stage 2b: Late Fixed-Point Constant Resolution (for `@sizeof`/`@alignof`) ===
.stage_consts_late:
    pop     rax                     ; Pass 2a pending flag
    test    eax, eax
    jz      .stage_ptrs
    mov     edi, 1
    call    run_const_fixpoint

.stage_ptrs:
    call    resolve_custom_ptr_types

    ; === Stage 5: Resolve `@slab`, `@arena`, & `@port` Declarations ===
.space_loop_init:
    xor     ebx, ebx
.sp_loop:
    cmp     ebx, [space_cnt]
    jae     .prescan_done
    mov     eax, ebx
    shl     eax, 5
    mov     rcx, [space_ast]
    lea     r14, [rcx + AST_SPACE_OFF + rax]

    mov     eax, [r14 + 24]
    mov     [tok_idx], eax          ; `<` token
    mov     al, TOK_LT
    call    expect_tok_kind

    cmp     dword [r14 + 4], SKIND_PORT
    je      .resolve_port
    cmp     dword [r14 + 4], SKIND_ARENA
    je      .resolve_arena
    cmp     dword [r14 + 4], SKIND_CAPSULE
    je      .resolve_capsule
    cmp     dword [r14 + 4], SKIND_RING
    je      .resolve_ring_or_dmaq
    cmp     dword [r14 + 4], SKIND_DMA_QUEUE
    je      .resolve_ring_or_dmaq

    ; `@slab name : <ElemType | max: const_expr>`
    call    parse_resolved_type_tag ; eax = elem_ty_tag
    mov     [r14 + 20], eax
    mov     edi, eax
    call    type_size_align         ; eax = elem_size
    mov     [r14 + 16], eax
    mov     r12d, eax               ; r12d = elem_size

    mov     al, TOK_PIPE
    call    expect_tok_kind

    call    next_tok_ptr            ; expect `max`
    cmp     byte [rax], TOK_IDENT
    jne     parse_err_at_tok
    mov     edx, [sym_max_id]
    cmp     [rax + 8], edx
    jne     parse_err_at_tok

    mov     al, TOK_COLON
    call    expect_tok_kind
    call    parse_const_lens_expr   ; rax = max element count
    mov     [r14 + 24], rax         ; capacity = max count
    mov     r13, rax
    imul    r13, r12                ; r13 = total byte reservation

    mov     al, TOK_GT
    call    expect_tok_kind
    jmp     .alloc_space_vaddr

.resolve_ring_or_dmaq:
    ; `@ring name : <Cell64 | cap: Pow2>` / `@dma_queue name : <Desc | cap: Pow2>`
    mov     eax, [r14 + 24]
    shl     eax, 4
    add     rax, [space_tok]
    mov     r15d, [rax + 4]         ; decl line number
    call    parse_resolved_type_tag ; eax = elem_ty
    mov     [r14 + 20], eax
    mov     edi, eax
    call    type_size_align         ; eax = elem_size, edx = elem_align
    mov     [r14 + 16], eax
    mov     r12d, eax

    cmp     dword [r14 + 4], SKIND_RING
    jne     .chk_dmaq_elem
    cmp     eax, 64
    jne     .err_bad_ring
    cmp     edx, 64
    jne     .err_bad_ring
    jmp     .ring_pipe
.chk_dmaq_elem:
    cmp     eax, 16
    je      .ring_pipe
    cmp     eax, 32
    je      .ring_pipe
    cmp     eax, 64
    jne     .err_bad_ring

.ring_pipe:
    mov     al, TOK_PIPE
    call    expect_tok_kind
    call    next_tok_ptr            ; expect `cap`
    cmp     byte [rax], TOK_IDENT
    jne     parse_err_at_tok
    mov     edx, [sym_cap_id]
    cmp     [rax + 8], edx
    jne     parse_err_at_tok
    mov     al, TOK_COLON
    call    expect_tok_kind
    call    parse_const_lens_expr   ; rax = cap
    cmp     rax, 2
    jb      .err_bad_ring
    lea     rdx, [rax - 1]
    test    rax, rdx                ; cap must be power of 2!
    jnz     .err_bad_ring
    mov     [r14 + 24], rax
    mov     r13, rax
    imul    r13, r12
    mov     al, TOK_GT
    call    expect_tok_kind
    jmp     .alloc_space_vaddr
.err_bad_ring:
    mov     esi, r15d
    lea     rdi, [msg_bad_ring]
    mov     edx, msg_bad_ring_len
    jmp     err_msg_at_line

.resolve_port:
    ; `@port name : <u8|u16|u32 | port_const_expr>`
    mov     eax, [r14 + 24]
    shl     eax, 4
    add     rax, [space_tok]
    mov     r15d, [rax + 4]         ; port declaration line number
    call    parse_resolved_type_tag ; eax = width type
    cmp     eax, TY_U8
    je      .rp_w_ok
    cmp     eax, TY_U16
    je      .rp_w_ok
    cmp     eax, TY_U32
    jne     .err_bad_port
.rp_w_ok:
    mov     [r14 + 20], eax         ; elem_ty_tag
    mov     edi, eax
    call    type_size_align         ; eax = 1, 2, or 4
    mov     [r14 + 16], eax
    mov     al, TOK_PIPE
    call    expect_tok_kind
    call    parse_const_lens_expr   ; rax = hardware I/O port number (0..0xFFFF)
    cmp     rax, 0xFFFF
    ja      .err_bad_port
    mov     [r14 + 8], rax          ; port number stored directly in vaddr slot!
    mov     qword [r14 + 24], 0
    mov     al, TOK_GT
    call    expect_tok_kind
    inc     ebx
    jmp     .sp_loop
.err_bad_port:
    mov     esi, r15d
    lea     rdi, [msg_bad_port]
    mov     edx, msg_bad_port_len
    jmp     err_msg_at_line

.resolve_capsule:
    ; `@capsule name : <cap: 4KiB_aligned_bytes>` -> DMA-ready page-aligned capsule slot!
    mov     eax, [r14 + 24]
    shl     eax, 4
    add     rax, [space_tok]
    mov     r15d, [rax + 4]         ; decl line
    call    next_tok_ptr
    cmp     byte [rax], TOK_IDENT
    jne     parse_err_at_tok
    mov     edx, [sym_cap_id]
    cmp     [rax + 8], edx
    jne     parse_err_at_tok
    mov     al, TOK_COLON
    call    expect_tok_kind
    call    parse_const_lens_expr   ; rax = byte capacity
    cmp     rax, 4096
    jb      .err_bad_cap_sz
    test    rax, 4095               ; must be 4KiB page-aligned for hardware W^X sealing!
    jnz     .err_bad_cap_sz
    mov     dword [r14 + 16], 1
    mov     dword [r14 + 20], TY_U8
    mov     [r14 + 24], rax
    mov     r13, rax
    mov     al, TOK_GT
    call    expect_tok_kind
    jmp     .alloc_space_vaddr
.err_bad_cap_sz:
    mov     esi, r15d
    lea     rdi, [msg_bad_capsule]
    mov     edx, msg_bad_capsule_len
    jmp     err_msg_at_line

.resolve_arena:
    ; `@arena name : <cap: const_expr>` -> byte-indexed (`TY_U8`, elem_size=1)
    call    next_tok_ptr            ; expect `cap`
    cmp     byte [rax], TOK_IDENT
    jne     parse_err_at_tok
    mov     edx, [sym_cap_id]
    cmp     [rax + 8], edx
    jne     parse_err_at_tok

    mov     al, TOK_COLON
    call    expect_tok_kind
    call    parse_const_lens_expr   ; rax = byte capacity
    mov     dword [r14 + 16], 1     ; elem_size = 1 byte
    mov     dword [r14 + 20], TY_U8 ; elem_ty_tag = u8
    mov     [r14 + 24], rax         ; capacity
    mov     r13, rax

    mov     al, TOK_GT
    call    expect_tok_kind

.alloc_space_vaddr:
    ; Align `data_bss_off` to 4096-byte page boundary (also 64B cacheline-aligned,
    ; 0 bytes on disk, and directly usable for hardware `CR3` page-table slabs!)
    mov     rax, [data_bss_off]
    add     rax, 4095
    and     rax, ~4095
    lea     rdx, [rax + DATA_VADDR_BASE]
    mov     [r14 + 8], rdx          ; vaddr_base!
    add     rax, r13
    mov     [data_bss_off], rax

    inc     ebx
    jmp     .sp_loop

.prescan_done:
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbx
    ret

; Evaluates a compile-time constant expression bounded by `|` or `>` inside `<...>`
parse_const_lens_expr:
    push    rbx
    push    r12
    mov     ebx, [tok_idx]
    ; Find top-level `|` or `>`
    xor     edx, edx
.scan:
    call    peek_tok_ptr
    movzx   ecx, byte [rax]
    test    cl, cl
    jz      parse_err_at_tok
    cmp     cl, TOK_LPAREN
    jne     .chk_rp
    inc     edx
    inc     dword [tok_idx]
    jmp     .scan
.chk_rp:
    cmp     cl, TOK_RPAREN
    jne     .chk_end
    dec     edx
    inc     dword [tok_idx]
    jmp     .scan
.chk_end:
    test    edx, edx
    jnz     .adv
    cmp     cl, TOK_PIPE
    je      .fenced
    cmp     cl, TOK_GT
    je      .fenced
.adv:
    inc     dword [tok_idx]
    jmp     .scan
.fenced:
    mov     r12, rax
    movzx   edx, byte [r12]
    push    rdx
    mov     byte [r12], TOK_EOF
    mov     [tok_idx], ebx
    call    eval_const_expr
    pop     rdx
    mov     [r12], dl
    pop     r12
    pop     rbx
    ret
