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
    cmp     byte [rax], TOK_LBRACK
    je      .brack

    ; Bare span rvalue (`s2 := s`, `fn_call(s)`, `return s`) -> `&{base, len}` in `rax`
    mov     edi, r12d
    call    emit_lea_local
    mov     [last_expr_ty], r13d
    ret

.brack:
    inc     dword [tok_idx]         ; consume `[`
    mov     edi, 1
    call    parse_expr              ; `rax` = index or `lo`
    call    peek_tok_ptr
    cmp     byte [rax], TOK_DOT2
    je      .subslice
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

.subslice:
    push    rbx
    push    r15
    inc     dword [tok_idx]         ; consume `..`
    call    alloc_temp_qword
    mov     ebx, eax                ; ebx = temp `lo`
    mov     edi, ebx
    call    emit_store_local

    mov     edi, 1
    call    parse_expr              ; `rax` = `hi`
    mov     al, TOK_RBRACK
    call    expect_tok_kind
    call    alloc_temp_qword
    mov     r15d, eax               ; r15d = temp `hi`
    mov     edi, r15d
    call    emit_store_local

    ; Bounds check: `(u64)lo <= (u64)hi && (u64)hi <= span.len`, else `fail 2`
    mov     edi, ebx
    call    emit_load_local         ; rax = lo
    mov     edi, r15d
    call    emit_load_local_rcx     ; rcx = hi
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC839              ; cmp rax, rcx (lo vs hi)
    call    emit_u16
    mov     ax, 0x870F              ; ja .oob
    call    emit_u16
    mov     eax, [code_off]
    push    rax
    xor     eax, eax
    call    emit_u32

    lea     edi, [r12d + 8]
    call    emit_load_local         ; rax = span.len
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC139              ; cmp rcx, rax (hi vs span.len)
    call    emit_u16
    mov     ax, 0x860F              ; jbe .ok
    call    emit_u16
    mov     eax, [code_off]
    push    rax
    xor     eax, eax
    call    emit_u32

    ; .oob:
    mov     rdx, [rsp + 8]          ; first `ja .oob` patch pos
    mov     edi, edx
    call    patch_rel32_here
    mov     edi, 2
    call    emit_raise_fault_imm

    ; .ok:
    pop     rdi
    call    patch_rel32_here
    add     rsp, 8                  ; pop first patch slot

    call    alloc_anon_span_slot
    mov     r14d, eax               ; new 16B `{ base, len }` slot

    ; `new_len = hi - lo`
    mov     edi, r15d
    call    emit_load_local
    mov     edi, ebx
    call    emit_load_local_rcx
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC829              ; sub rax, rcx
    call    emit_u16
    lea     edi, [r14d + 8]
    call    emit_store_local

    ; `new_base = span.base + lo * elem_size`
    mov     edi, r13d
    and     edi, TY_BASE_MASK
    call    type_size_align
    push    rax                     ; save elem_size across `emit_load_local` (`rdx` clobbered!)
    mov     edi, ebx
    call    emit_load_local         ; rax = lo
    pop     rcx                     ; rcx = elem_size (zero-extended)
    cmp     ecx, 1
    jbe     .ss_add_b
    mov     rax, rcx
    call    emit_mov_rcx_imm64
    mov     eax, 0xC1AF0F48         ; imul rax, rcx
    call    emit_u32
.ss_add_b:
    mov     edi, r12d
    call    emit_load_local_rcx     ; rcx = span.base
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC801              ; add rax, rcx
    call    emit_u16
    mov     edi, r14d
    call    emit_store_local

    mov     edi, 2
    call    free_temp_qwords
    mov     edi, r14d
    call    emit_lea_local
    mov     [last_expr_ty], r13d
    pop     r15
    pop     rbx
    ret

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
    jb      .scalar_read
    jmp     walk_struct_fields_rval
.scalar_read:
    push    rdi
    inc     dword [tok_idx]         ; consume `.`
    call    next_tok_ptr
    cmp     byte [rax], TOK_IDENT
    jne     parse_err_at_tok
    mov     ecx, [rax + 8]
    cmp     ecx, [sym_read_id]
    jne     parse_err_cur_tok
    mov     al, TOK_LPAREN
    call    expect_tok_kind
    mov     al, TOK_RPAREN
    call    expect_tok_kind
    pop     rdi
    mov     [last_expr_ty], edi
    jmp     emit_load_mem_typed

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

; -----------------------------------------------------------------------------
; First-Class `"..."` String Literal Expression (`str` = `TY_U8 | TY_FLAG_SPAN`)
; Input: `rax` = &Token (`TOK_STR`, cursor NOT yet incremented)
; Works identically on Linux `ELF64` and relocatable `PE32+` `--target=efi-os`!
; -----------------------------------------------------------------------------
emit_str_literal_span:
    push    rbx
    push    r12
    push    r13
    push    r14

    movzx   r13d, word [rax + 2]    ; decoded byte length
    mov     r14, [space_rodata]
    add     r14, [rax + 8]          ; decoded bytes ptr
    inc     dword [tok_idx]         ; consume `TOK_STR`

    call    alloc_anon_span_slot
    mov     r12d, eax               ; 16B `{ base, len }` slot

    test    r13d, r13d
    jz      .empty_str

    ; Jump over inline `.rodata` bytes in `RX` `.text`: `E9 <r13d>`
    mov     al, 0xE9
    call    emit_u8
    mov     eax, r13d
    call    emit_u32

    mov     rsi, r14
    mov     ecx, r13d
    call    emit_bytes

    ; `lea rax, [rip - (r13d + 7)]` (`48 8D 05 <disp32>`)
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0x058D
    call    emit_u16
    mov     eax, r13d
    add     eax, 7
    neg     eax
    call    emit_u32

    mov     edi, r12d
    call    emit_store_local        ; span.base = rax
    mov     eax, r13d
    call    emit_mov_rax_imm64
    lea     edi, [r12d + 8]
    call    emit_store_local        ; span.len = r13d
    jmp     .ret_span

.empty_str:
    mov     eax, 1                  ; non-null sentinel for 0-len slice
    call    emit_mov_rax_imm64
    mov     edi, r12d
    call    emit_store_local
    xor     eax, eax
    call    emit_mov_rax_imm64
    lea     edi, [r12d + 8]
    call    emit_store_local

.ret_span:
    mov     edi, r12d
    call    emit_lea_local
    mov     dword [last_expr_ty], TY_U8 | TY_FLAG_SPAN
    pop     r14
    pop     r13
    pop     r12
    pop     rbx
    ret

; -----------------------------------------------------------------------------
; Hardware Bit-Slice Bounds Parser `[ hi : lo ]` (`[` NOT yet consumed)
; Returns: `eax = lo` (shift `0..63`), `edx = width` (`hi - lo + 1`, `1..64`)
; -----------------------------------------------------------------------------
parse_bitslice_bounds:
    push    rbx
    push    r12
    push    r13

    mov     al, TOK_LBRACK
    call    expect_tok_kind

    ; Scan & evaluate `hi` up to `:`
    mov     ebx, [tok_idx]
    xor     edx, edx
.hi_sc:
    call    peek_tok_ptr
    movzx   ecx, byte [rax]
    test    cl, cl
    jz      parse_err_at_tok
    cmp     cl, TOK_LPAREN
    je      .hi_inc
    cmp     cl, TOK_LBRACK
    je      .hi_inc
    cmp     cl, TOK_RPAREN
    je      .hi_dec
    cmp     cl, TOK_RBRACK
    je      .hi_dec
    test    edx, edx
    jnz     .hi_adv
    cmp     cl, TOK_COLON
    je      .hi_fn
    jmp     .hi_adv
.hi_inc:
    inc     edx
    jmp     .hi_adv
.hi_dec:
    test    edx, edx
    jz      parse_err_at_tok
    dec     edx
.hi_adv:
    inc     dword [tok_idx]
    jmp     .hi_sc
.hi_fn:
    mov     r12, rax
    movzx   edx, byte [r12]
    push    rdx
    mov     byte [r12], TOK_EOF
    mov     [tok_idx], ebx
    call    eval_const_expr
    pop     rcx
    mov     [r12], cl
    cmp     rax, 63
    ja      parse_err_cur_tok
    mov     r13d, eax               ; r13d = hi
    mov     al, TOK_COLON
    call    expect_tok_kind

    ; Scan & evaluate `lo` up to `]`
    mov     ebx, [tok_idx]
    xor     edx, edx
.lo_sc:
    call    peek_tok_ptr
    movzx   ecx, byte [rax]
    test    cl, cl
    jz      parse_err_at_tok
    cmp     cl, TOK_LPAREN
    je      .lo_inc
    cmp     cl, TOK_LBRACK
    je      .lo_inc
    cmp     cl, TOK_RPAREN
    je      .lo_dec
    cmp     cl, TOK_RBRACK
    je      .lo_chk_rb
    jmp     .lo_adv
.lo_inc:
    inc     edx
    jmp     .lo_adv
.lo_dec:
    test    edx, edx
    jz      parse_err_at_tok
    dec     edx
    jmp     .lo_adv
.lo_chk_rb:
    test    edx, edx
    jz      .lo_fn
    dec     edx
.lo_adv:
    inc     dword [tok_idx]
    jmp     .lo_sc
.lo_fn:
    mov     r12, rax
    movzx   edx, byte [r12]
    push    rdx
    mov     byte [r12], TOK_EOF
    mov     [tok_idx], ebx
    call    eval_const_expr
    pop     rcx
    mov     [r12], cl
    cmp     rax, 63
    ja      parse_err_cur_tok
    cmp     eax, r13d               ; require `lo <= hi`!
    ja      parse_err_cur_tok
    mov     ebx, eax                ; ebx = lo
    mov     al, TOK_RBRACK
    call    expect_tok_kind

    mov     edx, r13d
    sub     edx, ebx
    inc     edx                     ; width = hi - lo + 1
    mov     eax, ebx                ; lo

    pop     r13
    pop     r12
    pop     rbx
    ret

; Applies chained `[hi:lo]` bit-slice extractions when `rax` holds a scalar value
try_parse_postfix_bitslice:
.bs_loop:
    call    peek_tok_ptr
    cmp     byte [rax], TOK_LBRACK
    jne     .bs_done
    mov     eax, [last_expr_ty]
    test    eax, TY_FLAG_SPAN | TY_FLAG_VIEW_ONE | TY_FLAG_ATOMIC
    jnz     .bs_done
    and     eax, TY_BASE_MASK
    cmp     eax, TY_STRUCT_BASE
    jae     .bs_done

    call    parse_bitslice_bounds   ; eax = lo, edx = width
    mov     edi, eax
    mov     esi, edx
    call    emit_extract_bitfield
    jmp     .bs_loop
.bs_done:
    ret

; -----------------------------------------------------------------------------
; `match expr { p1, p2 => body  lo..hi => body  _ => body }` Statement
; -----------------------------------------------------------------------------
parse_stmt_match:
    push    rbx
    push    r12
    push    r13
    push    r14
    push    r15
    sub     rsp, 528                ; [rsp+0..255] = end_patches (64), [rsp+256..511] = hit_patches (64)

    inc     dword [tok_idx]         ; consume `match`
    mov     edi, 1
    call    parse_expr              ; scrutinee -> `rax`
    call    alloc_anon_local
    mov     r12d, eax               ; r12d = scrutinee slot
    mov     edi, r12d
    call    emit_store_local

    mov     al, TOK_LBRACE
    call    expect_tok_kind
    xor     r13d, r13d              ; r13d = end_patch_cnt

.arm_loop:
    call    peek_tok_ptr
    cmp     byte [rax], TOK_RBRACE
    je      .match_done

    ; Check wildcard `_ =>`
    cmp     byte [rax], TOK_IDENT
    jne     .pat_list
    mov     ecx, [rax + 8]
    cmp     ecx, [sym_wild_id]
    jne     .pat_list
    cmp     byte [rax + 16], TOK_FAT_ARROW
    jne     .pat_list

    add     dword [tok_idx], 2      ; consume `_` and `=>`
    call    .parse_arm_body
    call    emit_jmp_rel32
    mov     [rsp + r13 * 4], eax
    inc     r13d
    call    .skip_opt_comma
    jmp     .arm_loop

.pat_list:
    xor     r14d, r14d              ; r14d = hit_patch_cnt for this arm
.pat_item:
    call    .eval_match_pat_const   ; `rax` = v0
    mov     rbx, rax
    call    peek_tok_ptr
    cmp     byte [rax], TOK_DOT2
    je      .pat_range

    ; Exact match: `cmp scrutinee, rbx; je .arm_body`
    mov     edi, r12d
    call    emit_load_local
    mov     rax, rbx
    call    emit_mov_rcx_imm64
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC839              ; cmp rax, rcx
    call    emit_u16
    mov     ax, 0x840F              ; je rel32
    call    emit_u16
    mov     eax, [code_off]
    mov     [rsp + 256 + r14 * 4], eax
    inc     r14d
    xor     eax, eax
    call    emit_u32
    jmp     .pat_sep

.pat_range:
    inc     dword [tok_idx]         ; consume `..`
    call    .eval_match_pat_const   ; `rax` = v1 (half-open `v0..v1`)
    mov     r15, rax
    sub     r15, rbx                ; span = v1 - v0
    jbe     parse_err_cur_tok

    mov     edi, r12d
    call    emit_load_local
    mov     rax, rbx
    call    emit_mov_rcx_imm64
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC829              ; sub rax, rcx (scrutinee - v0)
    call    emit_u16
    mov     rax, r15
    call    emit_mov_rcx_imm64
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC839              ; cmp rax, rcx
    call    emit_u16
    mov     ax, 0x820F              ; jb rel32
    call    emit_u16
    mov     eax, [code_off]
    mov     [rsp + 256 + r14 * 4], eax
    inc     r14d
    xor     eax, eax
    call    emit_u32

.pat_sep:
    call    peek_tok_ptr
    cmp     byte [rax], TOK_COMMA
    jne     .pat_arrow
    inc     dword [tok_idx]
    jmp     .pat_item

.pat_arrow:
    mov     al, TOK_FAT_ARROW
    call    expect_tok_kind

    call    emit_jmp_rel32
    mov     r15d, eax               ; miss_patch -> next arm

    ; Patch all `hit_patches` to `.arm_body`
    xor     ebx, ebx
.patch_hits:
    cmp     ebx, r14d
    jae     .run_body
    mov     edi, [rsp + 256 + rbx * 4]
    call    patch_rel32_here
    inc     ebx
    jmp     .patch_hits

.run_body:
    call    .parse_arm_body
    call    emit_jmp_rel32
    mov     [rsp + r13 * 4], eax
    inc     r13d

    mov     edi, r15d
    call    patch_rel32_here        ; miss lands at next arm
    call    .skip_opt_comma
    jmp     .arm_loop

.match_done:
    inc     dword [tok_idx]         ; consume `}`
    xor     ebx, ebx
.patch_ends:
    cmp     ebx, r13d
    jae     .exit_match
    mov     edi, [rsp + rbx * 4]
    call    patch_rel32_here
    inc     ebx
    jmp     .patch_ends

.exit_match:
    add     rsp, 528
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbx
    ret

.parse_arm_body:
    call    peek_tok_ptr
    cmp     byte [rax], TOK_LBRACE
    je      parse_block
    jmp     parse_stmt

.skip_opt_comma:
    call    peek_tok_ptr
    cmp     byte [rax], TOK_COMMA
    jne     .soc_ret
    inc     dword [tok_idx]
.soc_ret:
    ret

.eval_match_pat_const:
    push    rbx
    push    r12
    mov     ebx, [tok_idx]
    xor     edx, edx
.mp_sc:
    call    peek_tok_ptr
    movzx   ecx, byte [rax]
    test    cl, cl
    jz      parse_err_at_tok
    cmp     cl, TOK_LPAREN
    je      .mp_inc
    cmp     cl, TOK_LBRACK
    je      .mp_inc
    cmp     cl, TOK_RPAREN
    je      .mp_dec
    cmp     cl, TOK_RBRACK
    je      .mp_dec
    test    edx, edx
    jnz     .mp_adv
    cmp     cl, TOK_COMMA
    je      .mp_fn
    cmp     cl, TOK_DOT2
    je      .mp_fn
    cmp     cl, TOK_FAT_ARROW
    je      .mp_fn
    jmp     .mp_adv
.mp_inc:
    inc     edx
    jmp     .mp_adv
.mp_dec:
    test    edx, edx
    jz      parse_err_at_tok
    dec     edx
.mp_adv:
    inc     dword [tok_idx]
    jmp     .mp_sc
.mp_fn:
    mov     r12, rax
    movzx   edx, byte [r12]
    push    rdx
    mov     byte [r12], TOK_EOF
    mov     [tok_idx], ebx
    call    eval_const_expr
    pop     rcx
    mov     [r12], cl
    pop     r12
    pop     rbx
    ret
