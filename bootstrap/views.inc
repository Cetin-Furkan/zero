; =============================================================================
; Zero Stage-0 Bootstrap — Hardware Views, Ports & Value Types (`bootstrap/views.inc`)
; =============================================================================
; Implements hardware-typed `Zero` language features cleanly outside `parser.inc`:
;   - `@sizeof(Type)` / `@alignof(Type)` compile-time & runtime queries
;   - `Struct{ field: expr, ... }` stack-allocated value literals & struct copies
;   - `@view<T>(addr)` (single overlay pointer + `fail 3` null guard)
;   - `@view<T>(addr, len)` & `@mmio<T>(addr, len)` (16B `{base, len}` bounds-checked spans)
;   - `@port name : <u8|u16|u32 | port>` (`.read()`, `.write(val)`, `.port`)
;   - `try_eval_const_if` compile-time dead-branch elimination
; =============================================================================

; -----------------------------------------------------------------------------
; `@sizeof(T)` (`edi = 0`) & `@alignof(T)` (`edi = 1`)
; Consumes `(<Type>)`, returns value in `rax`.
; Sets `[const_unresolved_hit] = 1` if called in `const_trial_mode` on an unresolved struct.
; -----------------------------------------------------------------------------
eval_type_query_intrinsic:
    push    rbx
    push    r12
    mov     ebx, edi                ; 0 = sizeof, 1 = alignof

    mov     al, TOK_LPAREN
    call    expect_tok_kind

    call    peek_tok_ptr
    cmp     byte [rax], TOK_AT
    jne     .named_type
    inc     dword [tok_idx]
    call    parse_resolved_type_tag ; validate pointed-to type exists
    mov     al, TOK_RPAREN
    call    expect_tok_kind
    mov     eax, 8                  ; @Type view pointer is always 8 bytes, 8-byte aligned
    pop     r12
    pop     rbx
    ret

.named_type:
    call    parse_resolved_type_tag
    mov     r12d, eax               ; r12d = TY_*

    cmp     r12d, TY_STRUCT_BASE
    jb      .resolved
    mov     eax, r12d
    sub     eax, TY_STRUCT_BASE
    shl     eax, 5
    mov     rcx, [space_ast]
    cmp     dword [rcx + AST_TYPE_OFF + rax + 8], 0
    jne     .resolved

    ; Struct size not yet computed!
    mov     al, TOK_RPAREN
    call    expect_tok_kind
    cmp     byte [const_trial_mode], 1
    jne     parse_err_cur_tok
    mov     byte [const_unresolved_hit], 1
    xor     eax, eax
    pop     r12
    pop     rbx
    ret

.resolved:
    mov     al, TOK_RPAREN
    call    expect_tok_kind
    mov     edi, r12d
    call    type_size_align         ; eax = size, edx = align
    test    ebx, ebx
    jz      .done
    mov     eax, edx
.done:
    mov     eax, eax                ; zero-extend to 64-bit rax
    pop     r12
    pop     rbx
    ret

parse_runtime_sizeof_alignof:       ; edi = 0 (sizeof) or 1 (alignof)
    call    eval_type_query_intrinsic
    call    emit_mov_rax_imm64
    mov     dword [last_expr_ty], TY_U64
    ret

; -----------------------------------------------------------------------------
; Anonymous Frame Slot Allocator (`edi = qword_count`)
; Skips above live expression temporaries (`[frame_qwords] + [temp_depth]`)
; so anonymous spans or struct literals allocated mid-expression never clobber
; enclosing lvalue/binary-op temporaries. Returns `eax = negative rbp_disp`.
; -----------------------------------------------------------------------------
alloc_anon_span_slot:
    mov     edi, 2
alloc_anon_frame_qwords:
    mov     eax, [frame_qwords]
    add     eax, [temp_depth]
    add     eax, edi
    mov     [frame_qwords], eax
    cmp     eax, [max_frame_qwords]
    jbe     .calc
    mov     [max_frame_qwords], eax
.calc:
    shl     eax, 3
    neg     eax
    ret

; -----------------------------------------------------------------------------
; `@view<T>(addr [, len])` (`edi = 0`) & `@mmio<T>(addr [, len])` (`edi = 1`)
; -----------------------------------------------------------------------------
parse_view_or_mmio_call:
    push    rbx
    push    r12
    push    r13
    mov     ebx, edi                ; is_mmio (0 or 1)

    mov     al, TOK_LT
    call    expect_tok_kind
    call    parse_resolved_type_tag
    mov     r13d, eax               ; elem_ty_tag
    mov     al, TOK_GT
    call    expect_tok_kind
    mov     al, TOK_LPAREN
    call    expect_tok_kind

    mov     edi, 1
    call    parse_expr              ; arg0 = base address -> `rax`

    call    peek_tok_ptr
    cmp     byte [rax], TOK_COMMA
    je      .two_arg_span

    ; --- 1-Arg Single Overlay `@view<T>(addr)` / `@mmio<T>(addr)` ---
    mov     al, TOK_RPAREN
    call    expect_tok_kind
    test    ebx, ebx
    jnz     .ret_view1
    call    emit_null_view_check_rax ; `addr == 0` -> `fail 3`
.ret_view1:
    mov     eax, r13d
    or      eax, TY_FLAG_VIEW_ONE
    test    ebx, ebx
    jz      .set_v1_ty
    or      eax, TY_FLAG_MMIO
.set_v1_ty:
    mov     [last_expr_ty], eax
    jmp     .done

.two_arg_span:
    inc     dword [tok_idx]         ; consume `,`
    call    alloc_anon_span_slot
    mov     r12d, eax               ; r12d = disp (`[rbp+disp]` = base, `[rbp+disp+8]` = len)
    mov     edi, r12d
    call    emit_store_local

    mov     edi, 1
    call    parse_expr              ; arg1 = element count -> `rax`
    mov     al, TOK_RPAREN
    call    expect_tok_kind

    test    ebx, ebx
    jnz     .store_len
    ; For non-MMIO `@view<T>(base, len)`: `if len != 0 && base == 0 -> fail 3`
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC085              ; test rax, rax (len)
    call    emit_u16
    mov     ax, 0x840F              ; jz .ok (0-length span skips null base check)
    call    emit_u16
    mov     eax, [code_off]
    push    rax
    xor     eax, eax
    call    emit_u32

    mov     edi, r12d
    call    emit_load_local_rcx     ; rcx = base
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC985              ; test rcx, rcx
    call    emit_u16
    mov     ax, 0x850F              ; jnz .ok
    call    emit_u16
    mov     eax, [code_off]
    push    rax
    xor     eax, eax
    call    emit_u32

    mov     edi, 3
    call    emit_raise_fault_imm    ; fail 3

    pop     rdi
    call    patch_rel32_here
    pop     rdi
    call    patch_rel32_here

.store_len:
    lea     edi, [r12d + 8]
    call    emit_store_local
    mov     edi, r12d
    call    emit_lea_local          ; `rax` = address of `{ base, len }` span pair

    mov     eax, r13d
    or      eax, TY_FLAG_SPAN
    test    ebx, ebx
    jz      .set_span_ty
    or      eax, TY_FLAG_MMIO
.set_span_ty:
    mov     [last_expr_ty], eax

.done:
    pop     r13
    pop     r12
    pop     rbx
    ret

; -----------------------------------------------------------------------------
; `Struct{ field: expr, ... }` Stack-Allocated Value Literal (`r12d = type_idx`)
; -----------------------------------------------------------------------------
parse_struct_literal:
    push    rbx
    push    r13
    push    r14
    push    r15

    mov     eax, r12d
    shl     eax, 5
    mov     rcx, [space_ast]
    mov     eax, [rcx + AST_TYPE_OFF + rax + 8] ; byte_size
    add     eax, 7
    shr     eax, 3                  ; qword_count
    test    eax, eax
    jnz     .alloc_lit
    mov     eax, 1
.alloc_lit:
    mov     ebx, eax                ; ebx = qword_count
    mov     edi, eax
    call    alloc_anon_frame_qwords
    mov     r13d, eax               ; r13d = struct base rbp_disp

    ; Zero-initialize all struct qwords (`xor eax, eax`)
    mov     ax, 0xC031
    call    emit_u16
    xor     r14d, r14d
.zero_loop:
    cmp     r14d, ebx
    jae     .open_brace
    lea     edi, [r13d + r14d * 8]
    call    emit_store_local
    inc     r14d
    jmp     .zero_loop

.open_brace:
    mov     al, TOK_LBRACE
    call    expect_tok_kind

.fld_loop:
    call    peek_tok_ptr
    cmp     byte [rax], TOK_RBRACE
    je      .close_brace

    call    next_tok_ptr
    cmp     byte [rax], TOK_IDENT
    jne     parse_err_at_tok
    mov     esi, [rax + 8]          ; field sym_id
    mov     edi, r12d               ; struct type_idx
    call    find_type_field
    test    rax, rax
    jz      parse_err_cur_tok
    mov     r14d, [rax + 4]         ; field byte offset
    mov     r15d, [rax + 12]        ; field ty_tag

    mov     al, TOK_COLON
    call    expect_tok_kind

    mov     edi, 1
    call    parse_expr              ; field value (or src struct ptr) -> `rax`

    lea     edi, [r13d + r14d]
    call    emit_lea_local_rcx      ; `rcx` = &[rbp + r13d + field_off]
    mov     edi, r15d
    call    emit_store_mem_typed

    call    peek_tok_ptr
    cmp     byte [rax], TOK_COMMA
    jne     .fld_loop
    inc     dword [tok_idx]
    jmp     .fld_loop

.close_brace:
    inc     dword [tok_idx]         ; consume `}`
    mov     edi, r13d
    call    emit_lea_local          ; `rax` = address of initialized struct literal
    lea     eax, [r12d + TY_STRUCT_BASE]
    mov     [last_expr_ty], eax

    pop     r15
    pop     r14
    pop     r13
    pop     rbx
    ret

; -----------------------------------------------------------------------------
; Span (`TY_FLAG_SPAN`) & Single Overlay (`TY_FLAG_VIEW_ONE`) Local Accessors
; Inputs: `r12d = rbp_disp`, `r13d = local ty_tag`
; -----------------------------------------------------------------------------
parse_span_local_rval:
    call    peek_tok_ptr
    cmp     byte [rax], TOK_DOT
    je      .prop

    mov     al, TOK_LBRACK
    call    expect_tok_kind
    mov     edi, 1
    call    parse_expr              ; `rax` = index
    mov     al, TOK_RBRACK
    call    expect_tok_kind

    mov     edi, r12d
    call    emit_span_bounds_check_rax ; `idx >= len` -> `fail 2`

    mov     edi, r13d
    and     edi, TY_BASE_MASK
    mov     r14d, edi               ; r14d = base elem_ty
    call    type_size_align         ; eax = elem_size
    cmp     eax, 1
    jbe     .add_base
    call    emit_mov_rcx_imm64
    mov     eax, 0xC1AF0F48         ; imul rax, rcx
    call    emit_u32
.add_base:
    mov     edi, r12d
    call    emit_load_local_rcx     ; rcx = span.base
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC801              ; add rax, rcx
    call    emit_u16

    cmp     r14d, TY_STRUCT_BASE
    jae     .as_struct
    mov     [last_expr_ty], r14d
    mov     edi, r14d
    jmp     emit_load_mem_typed
.as_struct:
    mov     edi, r14d
    jmp     walk_struct_fields_rval

.prop:
    inc     dword [tok_idx]         ; consume `.`
    call    next_tok_ptr
    cmp     byte [rax], TOK_IDENT
    jne     parse_err_at_tok
    mov     ecx, [rax + 8]
    cmp     ecx, [sym_base_id]
    je      .ret_base
    cmp     ecx, [sym_len_id]
    je      .ret_len
    cmp     ecx, [sym_max_id]
    je      .ret_len
    cmp     ecx, [sym_cap_id]
    je      .ret_len
    jmp     parse_err_cur_tok
.ret_base:
    mov     edi, r12d
    call    emit_load_local
    mov     dword [last_expr_ty], TY_U64
    ret
.ret_len:
    lea     edi, [r12d + 8]
    call    emit_load_local
    mov     dword [last_expr_ty], TY_U64
    ret

parse_view1_local_rval:
    mov     edi, r12d
    call    emit_load_local         ; `rax` = pointer
    call    peek_tok_ptr
    cmp     byte [rax], TOK_DOT
    je      .deref_field
    mov     [last_expr_ty], r13d    ; pass/copy view pointer without dereferencing
    ret
.deref_field:
    test    r13d, TY_FLAG_MMIO
    jnz     .chk_struct
    call    emit_null_view_check_rax
.chk_struct:
    mov     edi, r13d
    and     edi, TY_BASE_MASK
    cmp     edi, TY_STRUCT_BASE
    jb      parse_err_cur_tok
    jmp     walk_struct_fields_rval

; -----------------------------------------------------------------------------
; `@port` Hardware I/O Lowering (`SKIND_PORT`: `r14` or `r15` = &SpaceEntry)
; -----------------------------------------------------------------------------
parse_port_rval:
    mov     al, TOK_DOT
    call    expect_tok_kind
    call    next_tok_ptr
    cmp     byte [rax], TOK_IDENT
    jne     parse_err_at_tok
    mov     ecx, [rax + 8]
    cmp     ecx, [sym_read_id]
    je      .do_read
    cmp     ecx, [sym_port_id]
    jne     parse_err_cur_tok

    mov     rax, [r14 + 8]
    call    emit_mov_rax_imm64
    mov     dword [last_expr_ty], TY_U16
    ret

.do_read:
    mov     al, TOK_LPAREN
    call    expect_tok_kind
    mov     al, TOK_RPAREN
    call    expect_tok_kind

    mov     eax, [r14 + 20]
    mov     [last_expr_ty], eax
    mov     ecx, [r14 + 16]         ; width: 1, 2, or 4
    mov     rdx, [r14 + 8]          ; port: 0..65535

    cmp     rdx, 0xFF
    ja      .read_dx

    cmp     ecx, 1
    jne     .ri_16
    mov     ax, 0xC031              ; xor eax, eax
    call    emit_u16
    mov     al, 0xE4                ; in al, imm8
    call    emit_u8
    mov     eax, [r14 + 8]
    jmp     emit_u8
.ri_16:
    cmp     ecx, 2
    jne     .ri_32
    mov     ax, 0xC031
    call    emit_u16
    mov     al, 0x66
    call    emit_u8
    mov     al, 0xE5                ; in ax, imm8
    call    emit_u8
    mov     eax, [r14 + 8]
    jmp     emit_u8
.ri_32:
    mov     al, 0xE5                ; in eax, imm8
    call    emit_u8
    mov     eax, [r14 + 8]
    jmp     emit_u8

.read_dx:
    push    rcx
    mov     al, 0xBA                ; mov edx, port32
    call    emit_u8
    mov     eax, [r14 + 8]
    call    emit_u32
    pop     rcx
    cmp     ecx, 1
    jne     .rd_16
    mov     ax, 0xC031
    call    emit_u16
    mov     al, 0xEC                ; in al, dx
    jmp     emit_u8
.rd_16:
    cmp     ecx, 2
    jne     .rd_32
    mov     ax, 0xC031
    call    emit_u16
    mov     ax, 0xED66              ; in ax, dx
    jmp     emit_u16
.rd_32:
    mov     al, 0xED                ; in eax, dx
    jmp     emit_u8

parse_port_stmt:
    mov     al, TOK_DOT
    call    expect_tok_kind
    call    next_tok_ptr
    cmp     byte [rax], TOK_IDENT
    jne     parse_err_at_tok
    mov     ecx, [sym_write_id]
    cmp     [rax + 8], ecx
    jne     parse_err_cur_tok

    mov     al, TOK_LPAREN
    call    expect_tok_kind
    mov     edi, 1
    call    parse_expr              ; value in `rax`
    mov     al, TOK_RPAREN
    call    expect_tok_kind

    mov     ecx, [r15 + 16]         ; width: 1, 2, or 4
    mov     rdx, [r15 + 8]          ; port: 0..65535
    cmp     rdx, 0xFF
    ja      .write_dx

    cmp     ecx, 1
    jne     .wi_16
    mov     al, 0xE6                ; out imm8, al
    call    emit_u8
    mov     eax, [r15 + 8]
    jmp     emit_u8
.wi_16:
    cmp     ecx, 2
    jne     .wi_32
    mov     ax, 0xE766              ; out imm8, ax
    call    emit_u16
    mov     eax, [r15 + 8]
    jmp     emit_u8
.wi_32:
    mov     al, 0xE7                ; out imm8, eax
    call    emit_u8
    mov     eax, [r15 + 8]
    jmp     emit_u8

.write_dx:
    push    rcx
    mov     al, 0xBA                ; mov edx, port32
    call    emit_u8
    mov     eax, [r15 + 8]
    call    emit_u32
    pop     rcx
    cmp     ecx, 1
    jne     .wd_16
    mov     al, 0xEE                ; out dx, al
    jmp     emit_u8
.wd_16:
    cmp     ecx, 2
    jne     .wd_32
    mov     ax, 0xEF66              ; out dx, ax
    jmp     emit_u16
.wd_32:
    mov     al, 0xEF                ; out dx, eax
    jmp     emit_u8

; -----------------------------------------------------------------------------
; Compile-Time Constant `if` Evaluability Check (`try_eval_const_if`)
; Returns: `eax = 1` (`rdx = const value`, `[tok_idx]` right at `{`), else `0` (rewound).
; -----------------------------------------------------------------------------
try_eval_const_if:
    push    rbx
    push    r12
    push    r14
    mov     ebx, [tok_idx]          ; save condition start

.scan:
    call    peek_tok_ptr
    movzx   ecx, byte [rax]
    test    cl, cl
    jz      .not_const
    cmp     cl, TOK_LBRACE
    je      .at_brace
    cmp     cl, TOK_DOT
    je      .not_const
    cmp     cl, TOK_LBRACK
    je      .not_const
    cmp     cl, TOK_SLASH
    je      .not_const
    cmp     cl, TOK_PERCENT
    je      .not_const
    cmp     cl, TOK_AT
    je      .chk_at
    cmp     cl, TOK_IDENT
    jne     .adv

    ; Must NOT be a function call (`IDENT(`) or Descriptor/Struct (`IDENT<` / `IDENT{`)
    movzx   edx, byte [rax + 16]
    cmp     dl, TOK_LPAREN
    je      .not_const
    cmp     dl, TOK_LT
    je      .not_const
    cmp     dl, TOK_LBRACE
    je      .not_const

    ; Must NOT be shadowed by a local or space, and MUST be a resolved `::` const
    push    rax
    mov     edi, [rax + 8]
    call    find_local_entry
    test    eax, eax
    pop     rax
    jnz     .not_const

    push    rax
    mov     edi, [rax + 8]
    call    find_space
    test    rax, rax
    pop     rax
    jnz     .not_const

    mov     edi, [rax + 8]
    call    find_const
    test    eax, eax
    jz      .not_const
    jmp     .adv

.chk_at:
    ; Allow only `@sizeof(...)` / `@alignof(...)`
    inc     dword [tok_idx]
    call    peek_tok_ptr
    cmp     byte [rax], TOK_IDENT
    jne     .not_const
    mov     edx, [rax + 8]
    cmp     edx, [sym_sizeof_id]
    je      .skip_at_type_arg
    cmp     edx, [sym_alignof_id]
    jne     .not_const
.skip_at_type_arg:
    inc     dword [tok_idx]
    call    peek_tok_ptr
    cmp     byte [rax], TOK_LPAREN
    jne     .not_const
    inc     dword [tok_idx]
    mov     ecx, 1
.paren_loop:
    call    next_tok_ptr
    movzx   edx, byte [rax]
    test    dl, dl
    jz      .not_const
    cmp     dl, TOK_LPAREN
    jne     .p_rp
    inc     ecx
    jmp     .paren_loop
.p_rp:
    cmp     dl, TOK_RPAREN
    jne     .paren_loop
    dec     ecx
    jnz     .paren_loop
    jmp     .scan

.adv:
    inc     dword [tok_idx]
    jmp     .scan

.at_brace:
    mov     r12d, [tok_idx]
    cmp     r12d, ebx
    je      .not_const
    mov     r14, rax                ; ptr to `{` token
    mov     byte [r14], TOK_EOF
    mov     byte [const_trial_mode], 1
    mov     edi, ebx
    call    try_eval_const_at
    mov     byte [const_trial_mode], 0
    mov     byte [r14], TOK_LBRACE
    test    eax, eax
    jz      .not_const
    cmp     [tok_idx], r12d
    jne     .not_const
    mov     eax, 1                  ; `rdx` already holds evaluated constant value
    pop     r14
    pop     r12
    pop     rbx
    ret

.not_const:
    mov     [tok_idx], ebx
    xor     eax, eax
    pop     r14
    pop     r12
    pop     rbx
    ret

; Skips any trailing `else { ... }` / `else if cond { ... }` chain cleanly
skip_else_chain:
    call    peek_tok_ptr
    cmp     byte [rax], TOK_KW
    jne     .done
    cmp     byte [rax + 1], KW_ELSE
    jne     .done
    inc     dword [tok_idx]         ; consume `else`
    call    peek_tok_ptr
    cmp     byte [rax], TOK_KW
    jne     .skip_blk
    cmp     byte [rax + 1], KW_IF
    jne     .skip_blk
    inc     dword [tok_idx]         ; consume `if`
    xor     ecx, ecx
.find_if_brace:
    call    peek_tok_ptr
    movzx   edx, byte [rax]
    test    dl, dl
    jz      parse_err_at_tok
    cmp     dl, TOK_LPAREN
    jne     .fib_rp
    inc     ecx
    inc     dword [tok_idx]
    jmp     .find_if_brace
.fib_rp:
    cmp     dl, TOK_RPAREN
    jne     .fib_chk_lb
    dec     ecx
    inc     dword [tok_idx]
    jmp     .find_if_brace
.fib_chk_lb:
    test    ecx, ecx
    jnz     .fib_adv
    cmp     dl, TOK_LBRACE
    je      .skip_blk
.fib_adv:
    inc     dword [tok_idx]
    jmp     .find_if_brace
.skip_blk:
    call    skip_brace_block
    jmp     skip_else_chain
.done:
    ret
