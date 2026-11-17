; =============================================================================
; Zero Stage-0 Bootstrap — Programmable Pointers & Silicon Ops (`bootstrap/pointers.inc`)
; =============================================================================
; Lowers directly to native x86-64 machine instructions (zero helper calls):
;   1. `Name :: @ptr<Elem | mask: M | wrap: W | stride: S | bswap>` custom pointers
;   2. `@stream<u32|u64>(addr, len)` non-temporal `movnti` cache-bypassing spans
;   3. `@atomic<u32|u64>(addr)` `lock xadd` / `lock cmpxchg` / `xchg` pointers
;   4. `@code_bytes(fn)`, `@call(ptr, args...)`, and `@emit(b0, ...)` code ops
;   5. `@popcnt`, `@lzcnt`, `@tzcnt`, `@bswap16/32/64`, `@pdep`, `@pext`,
;      `@crc32c`, `@prefetch`, `@clflush`, `@lfence`, `@sfence`, `@mfence`
; =============================================================================

%define PTR_FLAG_MASK       1
%define PTR_FLAG_WRAP       2
%define PTR_FLAG_BSWAP      4

; -----------------------------------------------------------------------------
; Prescan Pass: Resolve all `Name :: @ptr<Elem [| clause]*>` declarations
; -----------------------------------------------------------------------------
resolve_custom_ptr_types:
    push    rbx
    push    r12
    push    r13
    push    r14
    push    r15

    xor     ebx, ebx
.pt_loop:
    cmp     ebx, [type_cnt]
    jae     .pt_done

    mov     eax, ebx
    shl     eax, 5
    mov     rcx, [space_ast]
    lea     r14, [rcx + AST_TYPE_OFF + rax]
    cmp     byte [r14 + 4], UKIND_PTR
    jne     .pt_next

    lea     r15, [rcx + AST_PTRMETA_OFF + rax] ; 32B PtrMeta for `type_idx = ebx`
    mov     eax, [r14 + 20]         ; saved `<` tok_idx
    mov     [tok_idx], eax
    mov     al, TOK_LT
    call    expect_tok_kind

    call    parse_resolved_type_tag ; eax = elem_ty
    mov     r12d, eax
    mov     edi, eax
    call    type_size_align         ; eax = default elem_size
    mov     r13d, eax               ; r13d = elem_size

    mov     qword [r15 + 0],  -1    ; addr_mask = 0xFFFFFFFFFFFFFFFF
    mov     qword [r15 + 8],  0     ; wrap_mask = 0
    mov     [r15 + 16], r13d        ; byte_stride = elem_size
    mov     [r15 + 20], r12d        ; elem_ty
    mov     dword [r15 + 24], 0     ; flags = 0

.clause_loop:
    call    peek_tok_ptr
    cmp     byte [rax], TOK_PIPE
    jne     .pt_close
    inc     dword [tok_idx]         ; consume `|`

    call    next_tok_ptr
    cmp     byte [rax], TOK_IDENT
    jne     parse_err_at_tok
    mov     ecx, [rax + 8]

    cmp     ecx, [sym_mask_id]
    je      .cl_mask
    cmp     ecx, [sym_wrap_id]
    je      .cl_wrap
    cmp     ecx, [sym_stride_id]
    je      .cl_stride
    cmp     ecx, [sym_bswap_id]
    je      .cl_bswap
    jmp     parse_err_cur_tok

.cl_mask:
    mov     al, TOK_COLON
    call    expect_tok_kind
    call    parse_const_lens_expr
    test    rax, rax
    jz      parse_err_cur_tok
    mov     [r15 + 0], rax
    or      dword [r15 + 24], PTR_FLAG_MASK
    jmp     .clause_loop

.cl_wrap:
    mov     al, TOK_COLON
    call    expect_tok_kind
    call    parse_const_lens_expr
    cmp     rax, 2
    jb      parse_err_cur_tok
    lea     rdx, [rax - 1]
    test    rax, rdx                ; must be power of 2!
    jnz     parse_err_cur_tok
    mov     [r15 + 8], rdx          ; wrap_mask = cap - 1
    or      dword [r15 + 24], PTR_FLAG_WRAP
    jmp     .clause_loop

.cl_stride:
    mov     al, TOK_COLON
    call    expect_tok_kind
    call    parse_const_lens_expr
    cmp     rax, 1
    jb      parse_err_cur_tok
    mov     [r15 + 16], eax
    jmp     .clause_loop

.cl_bswap:
    cmp     r12d, TY_STRUCT_BASE
    jae     parse_err_cur_tok
    cmp     r13d, 2
    je      .bsw_ok
    cmp     r13d, 4
    je      .bsw_ok
    cmp     r13d, 8
    jne     parse_err_cur_tok
.bsw_ok:
    or      dword [r15 + 24], PTR_FLAG_BSWAP
    jmp     .clause_loop

.pt_close:
    mov     al, TOK_GT
    call    expect_tok_kind
    mov     word  [r14 + 6], 8      ; align = 8
    mov     dword [r14 + 8], 8      ; byte_size = 8

.pt_next:
    inc     ebx
    jmp     .pt_loop

.pt_done:
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbx
    ret

; -----------------------------------------------------------------------------
; Emits inline byte-swap on `rax` for `edi` = elem_ty (`2`, `4`, or `8` bytes)
; -----------------------------------------------------------------------------
emit_bswap_for_ty:
    push    rbx
    mov     ebx, edi                ; save `elem_ty` for signed vs unsigned extension
    call    type_size_align         ; eax = 2, 4, or 8
    cmp     eax, 2
    je      .b16
    cmp     eax, 4
    je      .b32
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC80F              ; 48 0F C8 = bswap rax
    call    emit_u16
    pop     rbx
    ret
.b32:
    mov     ax, 0xC80F              ; 0F C8 = bswap eax
    call    emit_u16
    cmp     ebx, TY_I32
    jne     .b32_dn
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC063              ; 48 63 C0 = movsxd rax, eax
    call    emit_u16
.b32_dn:
    pop     rbx
    ret
.b16:
    mov     eax, 0x08C0C166         ; 66 C1 C0 08 = rol ax, 8
    call    emit_u32
    cmp     ebx, TY_I16
    je      .b16_sx
    mov     ax, 0xB70F              ; 0F B7 C0 = movzx eax, ax
    call    emit_u16
    mov     al, 0xC0
    call    emit_u8
    pop     rbx
    ret
.b16_sx:
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xBF0F              ; 48 0F BF C0 = movsx rax, ax
    call    emit_u16
    mov     al, 0xC0
    call    emit_u8
    pop     rbx
    ret

; -----------------------------------------------------------------------------
; Custom `@ptr` Indexed Effective Address Emitter
; Input: `r12d = rbp_disp`, `r13d = TY_CUSTOM_PTR_BASE + type_idx`
; Consumes `[ expr ]`, emits masked + wrapped + strided address into `rax`!
; Returns `r15` = &PtrMeta
; -----------------------------------------------------------------------------
emit_custom_ptr_elem_addr:
    push    rbx
    push    r14

    mov     eax, r13d
    and     eax, TY_BASE_MASK
    sub     eax, TY_CUSTOM_PTR_BASE
    shl     eax, 5
    mov     rcx, [space_ast]
    lea     r15, [rcx + AST_PTRMETA_OFF + rax]

    ; 1. Load raw pointer word -> `rax`, apply `addr_mask` if `PTR_FLAG_MASK`, check non-null
    mov     edi, r12d
    call    emit_load_local
    test    dword [r15 + 24], PTR_FLAG_MASK
    jz      .chk_null
    mov     rax, [r15 + 0]
    call    emit_mov_rcx_imm64
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC821              ; 48 21 C8 = and rax, rcx
    call    emit_u16
.chk_null:
    call    emit_null_view_check_rax ; `masked_base == 0` -> `fail 3`

    call    alloc_temp_qword
    mov     r14d, eax               ; temp slot holding `masked_base`
    mov     edi, r14d
    call    emit_store_local

    ; 2. Parse `[ idx ]` -> `rax`
    mov     al, TOK_LBRACK
    call    expect_tok_kind
    mov     edi, 1
    call    parse_expr
    mov     al, TOK_RBRACK
    call    expect_tok_kind

    ; 3. Optional power-of-2 index wrap (`idx & wrap_mask`)
    test    dword [r15 + 24], PTR_FLAG_WRAP
    jz      .scale_stride
    mov     rax, [r15 + 8]
    call    emit_mov_rcx_imm64
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC821              ; and rax, rcx
    call    emit_u16

.scale_stride:
    mov     eax, [r15 + 16]         ; byte_stride
    cmp     eax, 1
    jbe     .add_base
    call    emit_mov_rcx_imm64
    mov     eax, 0xC1AF0F48         ; imul rax, rcx
    call    emit_u32

.add_base:
    mov     edi, r14d
    call    emit_load_local_rcx     ; rcx = masked_base
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC801              ; add rax, rcx
    call    emit_u16

    mov     edi, 1
    call    free_temp_qwords

    pop     r14
    pop     rbx
    ret

; -----------------------------------------------------------------------------
; Custom `@ptr` Local Rvalue (`p.raw`, `p[i]`, `p[i].field`, or bare `p`)
; Input: `r12d = rbp_disp`, `r13d = ty_tag`
; -----------------------------------------------------------------------------
parse_custom_ptr_rval:
    push    r15
    call    peek_tok_ptr
    cmp     byte [rax], TOK_DOT
    je      .prop_raw
    cmp     byte [rax], TOK_LBRACK
    je      .indexed

    ; Bare custom pointer value (`p2 := p` or fn arg)
    mov     edi, r12d
    call    emit_load_local
    mov     [last_expr_ty], r13d
    pop     r15
    ret

.prop_raw:
    inc     dword [tok_idx]         ; consume `.`
    call    next_tok_ptr
    cmp     byte [rax], TOK_IDENT
    jne     parse_err_at_tok
    mov     ecx, [rax + 8]
    cmp     ecx, [sym_raw_id]
    jne     parse_err_cur_tok
    mov     edi, r12d
    call    emit_load_local
    mov     dword [last_expr_ty], TY_U64
    pop     r15
    ret

.indexed:
    call    emit_custom_ptr_elem_addr ; `rax` = elem_addr, `r15` = &PtrMeta
    mov     edx, [r15 + 20]         ; elem_ty
    cmp     edx, TY_STRUCT_BASE
    jae     .st_elem
    mov     edi, edx
    call    emit_load_mem_typed
    test    dword [r15 + 24], PTR_FLAG_BSWAP
    jz      .set_ty
    mov     edi, [r15 + 20]
    call    emit_bswap_for_ty
.set_ty:
    mov     eax, [r15 + 20]
    mov     [last_expr_ty], eax
    pop     r15
    ret

.st_elem:
    mov     edi, edx
    pop     r15
    jmp     walk_struct_fields_rval

; -----------------------------------------------------------------------------
; Custom `@ptr` Lvalue Assignment (`p[i] = val`, `p[i] += val`)
; Input: `r12d = rbp_disp`, `r13d = ty_tag` (`[` already peeked!)
; -----------------------------------------------------------------------------
parse_custom_ptr_index_stmt:
    push    rbx
    push    r12
    push    r14
    push    r15

    call    emit_custom_ptr_elem_addr ; `rax` = elem_addr, `r15` = &PtrMeta
    mov     r13d, [r15 + 20]        ; elem_ty

    ; If struct element (`p[i].field = val`), walk `.field` offsets
.walk_st:
    call    peek_tok_ptr
    cmp     byte [rax], TOK_DOT
    jne     .have_addr
    cmp     r13d, TY_STRUCT_BASE
    jb      parse_err_at_tok
    inc     dword [tok_idx]
    call    next_tok_ptr
    cmp     byte [rax], TOK_IDENT
    jne     parse_err_at_tok
    mov     esi, [rax + 8]
    mov     edi, r13d
    sub     edi, TY_STRUCT_BASE
    call    find_type_field
    test    rax, rax
    jz      parse_err_cur_tok
    mov     ecx, [rax + 4]
    mov     r13d, [rax + 12]
    test    ecx, ecx
    jz      .walk_st
    push    rcx
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC081              ; add rax, imm32
    call    emit_u16
    pop     rax
    call    emit_u32
    jmp     .walk_st

.have_addr:
    call    alloc_temp_qword
    mov     r14d, eax               ; temp slot holding target address
    mov     edi, r14d
    call    emit_store_local

    call    next_tok_ptr
    movzx   ebx, byte [rax]         ; assign op

    mov     edi, 1
    call    parse_expr              ; rhs -> `rax`

    cmp     ebx, TOK_ASSIGN
    je      .do_store

    ; Compound assignment (`+=`, `^=`, etc. — unswap before ALU if `bswap`!)
    mov     al, 0x49
    call    emit_u8
    mov     ax, 0xC089              ; mov r8, rax (rhs)
    call    emit_u16
    mov     edi, r14d
    call    emit_load_local
    mov     edi, r13d
    call    emit_load_mem_typed
    test    dword [r15 + 24], PTR_FLAG_BSWAP
    jz      .do_alu
    mov     edi, r13d
    call    emit_bswap_for_ty
.do_alu:
    mov     al, 0x4C
    call    emit_u8
    mov     ax, 0xC189              ; mov rcx, r8
    call    emit_u16
    mov     edi, ebx
    mov     esi, r13d
    call    emit_compound_alu

.do_store:
    test    dword [r15 + 24], PTR_FLAG_BSWAP
    jz      .write_final
    cmp     r13d, TY_STRUCT_BASE
    jae     .write_final
    mov     edi, r13d
    call    emit_bswap_for_ty
.write_final:
    mov     edi, r14d
    call    emit_load_local_rcx
    mov     edi, r13d
    call    emit_store_mem_typed

    mov     edi, 1
    call    free_temp_qwords

    pop     r15
    pop     r14
    pop     r12
    pop     rbx
    ret

; -----------------------------------------------------------------------------
; `@atomic<u32|u64>` Method Lowering (`load`, `store`, `swap`, `add`, `sub`, `cas`)
; Input: `r12d = rbp_disp`, `r13d = elem_ty | TY_FLAG_ATOMIC`
; -----------------------------------------------------------------------------
parse_atomic_rval_or_stmt:
    push    rbx
    push    r14
    push    r15

    call    peek_tok_ptr
    cmp     byte [rax], TOK_DOT
    je      .method
    ; Bare atomic pointer value (`a2 := a` or param)
    mov     edi, r12d
    call    emit_load_local
    mov     [last_expr_ty], r13d
    jmp     .done

.method:
    inc     dword [tok_idx]         ; consume `.`
    call    next_tok_ptr
    mov     ebx, [rax + 8]          ; method sym_id (or KW_AND/OR?)
    cmp     byte [rax], TOK_IDENT
    jne     parse_err_at_tok

    mov     r14d, r13d
    and     r14d, TY_BASE_MASK      ; TY_U32/TY_I32 (size 4) or TY_U64/TY_I64 (size 8)
    mov     edi, r14d
    call    type_size_align
    mov     r15d, eax               ; r15d = 4 or 8

    cmp     ebx, [sym_load_id]
    je      .m_load
    cmp     ebx, [sym_store_id]
    je      .m_xchg
    cmp     ebx, [sym_swap_id]
    je      .m_xchg
    cmp     ebx, [sym_add_id]
    je      .m_add
    cmp     ebx, [sym_sub_id]
    je      .m_sub
    cmp     ebx, [sym_cas_id]
    je      .m_cas
    jmp     parse_err_cur_tok

.m_load:
    mov     al, TOK_LPAREN
    call    expect_tok_kind
    mov     al, TOK_RPAREN
    call    expect_tok_kind
    mov     edi, r12d
    call    emit_load_local         ; rax = addr
    mov     edi, r14d
    call    emit_load_mem_typed
    mov     [last_expr_ty], r14d
    jmp     .done

.m_xchg:
    mov     al, TOK_LPAREN
    call    expect_tok_kind
    mov     edi, 1
    call    parse_expr              ; rax = val
    mov     al, TOK_RPAREN
    call    expect_tok_kind
    mov     edi, r12d
    call    emit_load_local_rcx     ; rcx = addr
    cmp     r15d, 8
    jne     .xchg32
    mov     al, 0x48
    call    emit_u8
.xchg32:
    mov     ax, 0x0187              ; [48] 87 01 = xchg [rcx], eax/rax
    call    emit_u16
    cmp     r14d, TY_I32
    jne     .xchg_ty
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC063              ; 48 63 C0 = movsxd rax, eax
    call    emit_u16
.xchg_ty:
    mov     [last_expr_ty], r14d
    jmp     .done

.m_sub:
    mov     al, TOK_LPAREN
    call    expect_tok_kind
    mov     edi, 1
    call    parse_expr              ; rax = val
    mov     al, TOK_RPAREN
    call    expect_tok_kind
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xD8F7              ; 48 F7 D8 = neg rax
    call    emit_u16
    jmp     .emit_xadd

.m_add:
    mov     al, TOK_LPAREN
    call    expect_tok_kind
    mov     edi, 1
    call    parse_expr              ; rax = val
    mov     al, TOK_RPAREN
    call    expect_tok_kind
.emit_xadd:
    mov     edi, r12d
    call    emit_load_local_rcx     ; rcx = addr
    mov     al, 0xF0                ; LOCK prefix
    call    emit_u8
    cmp     r15d, 8
    jne     .xadd32
    mov     al, 0x48
    call    emit_u8
.xadd32:
    mov     ax, 0xC10F              ; F0 [48] 0F C1 01 = lock xadd [rcx], eax/rax
    call    emit_u16
    mov     al, 0x01
    call    emit_u8
    cmp     r14d, TY_I32
    jne     .xadd_ty
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC063              ; 48 63 C0 = movsxd rax, eax
    call    emit_u16
.xadd_ty:
    mov     [last_expr_ty], r14d
    jmp     .done

.m_cas:
    mov     al, TOK_LPAREN
    call    expect_tok_kind
    mov     edi, 1
    call    parse_expr              ; expected -> rax
    call    alloc_temp_qword
    mov     ebx, eax
    mov     edi, ebx
    call    emit_store_local

    mov     al, TOK_COMMA
    call    expect_tok_kind
    mov     edi, 1
    call    parse_expr              ; desired -> rax
    mov     al, TOK_RPAREN
    call    expect_tok_kind

    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC289              ; 48 89 C2 = mov rdx, rax (desired)
    call    emit_u16
    mov     edi, ebx
    call    emit_load_local         ; rax = expected
    mov     edi, r12d
    call    emit_load_local_rcx     ; rcx = addr

    mov     al, 0xF0                ; LOCK prefix
    call    emit_u8
    cmp     r15d, 8
    jne     .cas32
    mov     al, 0x48
    call    emit_u8
.cas32:
    mov     ax, 0xB10F              ; F0 [48] 0F B1 11 = lock cmpxchg [rcx], edx/rdx
    call    emit_u16
    mov     al, 0x11
    call    emit_u8

    ; Convert ZF -> `rax = 1` (swapped) or `0` (failed)
    mov     ax, 0x940F              ; 0F 94 C0 = sete al
    call    emit_u16
    mov     al, 0xC0
    call    emit_u8
    mov     ax, 0xB60F              ; 0F B6 C0 = movzx eax, al
    call    emit_u16
    mov     al, 0xC0
    call    emit_u8

    mov     edi, 1
    call    free_temp_qwords
    mov     dword [last_expr_ty], TY_U8

.done:
    pop     r15
    pop     r14
    pop     rbx
    ret

; -----------------------------------------------------------------------------
; Extended `@` Intrinsics (Valid on BOTH Host ELF64 and `--target=efi-os`!)
; Input: `ebx = sym_id`, `r12d = line`
; Returns: `eax = 1` if handled (`rax` & `[last_expr_ty]` emitted), `0` otherwise.
; -----------------------------------------------------------------------------
try_parse_ext_intrinsic:
    cmp     ebx, [sym_span_id]
    je      .do_span
    cmp     ebx, [sym_stream_id]
    je      .do_stream
    cmp     ebx, [sym_atomic_id]
    je      .do_atomic
    cmp     ebx, [sym_code_bytes_id]
    je      .do_code_bytes
    cmp     ebx, [sym_call_id]
    je      .do_indirect_call
    cmp     ebx, [sym_emit_id]
    je      .do_raw_emit

    cmp     ebx, [sym_popcnt_id]
    je      .do_popcnt
    cmp     ebx, [sym_lzcnt_id]
    je      .do_lzcnt
    cmp     ebx, [sym_tzcnt_id]
    je      .do_tzcnt
    cmp     ebx, [sym_bswap16_id]
    je      .do_bswap16
    cmp     ebx, [sym_bswap32_id]
    je      .do_bswap32
    cmp     ebx, [sym_bswap64_id]
    je      .do_bswap64
    cmp     ebx, [sym_pdep_id]
    je      .do_pdep
    cmp     ebx, [sym_pext_id]
    je      .do_pext
    cmp     ebx, [sym_crc32c_id]
    je      .do_crc32c
    cmp     ebx, [sym_prefetch_id]
    je      .do_prefetch
    cmp     ebx, [sym_clflush_id]
    je      .do_clflush
    cmp     ebx, [sym_lfence_id]
    je      .do_lfence
    cmp     ebx, [sym_sfence_id]
    je      .do_sfence
    cmp     ebx, [sym_mfence_id]
    je      .do_mfence

    ; Check if Phase-3 Reactor / Direct-Silicon intrinsic (`@reactor_id`, `@movdiri`, `@eoi`, etc.)
    call    try_parse_reactor_intrinsic
    test    eax, eax
    jnz     .already_handled

    ; Check if `@UserPtrType(raw_expr)`!
    mov     edi, ebx
    call    find_user_type
    cmp     eax, -1
    je      .not_handled
    mov     edx, eax
    shl     edx, 5
    mov     rcx, [space_ast]
    cmp     byte [rcx + AST_TYPE_OFF + rdx + 4], UKIND_PTR
    jne     .not_handled

    ; Construct `@MyPtr(raw_expr)`!
    push    rax
    mov     al, TOK_LPAREN
    call    expect_tok_kind
    mov     edi, 1
    call    parse_expr
    mov     al, TOK_RPAREN
    call    expect_tok_kind
    pop     rax
    add     eax, TY_CUSTOM_PTR_BASE
    mov     [last_expr_ty], eax
.already_handled:
    mov     eax, 1
    ret

.not_handled:
    xor     eax, eax
    ret

.do_span:
    xor     edi, edi
    call    parse_view_or_mmio_call
    test    dword [last_expr_ty], TY_FLAG_SPAN
    jz      parse_err_cur_tok       ; `@span<T>(base, len)` requires 2 args
    mov     eax, 1
    ret

.do_stream:
    xor     edi, edi                ; enforce non-null (`fail 3`) on both ELF64 and EFI
    call    parse_view_or_mmio_call
    mov     eax, [last_expr_ty]
    test    eax, TY_FLAG_SPAN
    jz      parse_err_cur_tok
    mov     edi, eax
    and     edi, TY_BASE_MASK
    cmp     edi, TY_STRUCT_BASE
    jae     parse_err_cur_tok
    call    type_size_align
    cmp     eax, 4
    je      .st_ok
    cmp     eax, 8
    jne     parse_err_cur_tok       ; `movnti` supports 32-bit and 64-bit stores!
.st_ok:
    or      dword [last_expr_ty], TY_FLAG_STREAM
    mov     eax, 1
    ret

.do_atomic:
    mov     al, TOK_LT
    call    expect_tok_kind
    call    parse_resolved_type_tag
    mov     r13d, eax
    cmp     eax, TY_STRUCT_BASE
    jae     parse_err_cur_tok
    mov     edi, eax
    call    type_size_align
    cmp     eax, 4
    je      .at_w_ok
    cmp     eax, 8
    jne     parse_err_cur_tok
.at_w_ok:
    mov     al, TOK_GT
    call    expect_tok_kind
    mov     al, TOK_LPAREN
    call    expect_tok_kind
    mov     edi, 1
    call    parse_expr              ; addr -> `rax`
    mov     al, TOK_RPAREN
    call    expect_tok_kind
    call    emit_null_view_check_rax
    or      r13d, TY_FLAG_ATOMIC
    mov     [last_expr_ty], r13d
    mov     eax, 1
    ret

.do_code_bytes:
    push    r12
    mov     al, TOK_LPAREN
    call    expect_tok_kind
    call    next_tok_ptr
    cmp     byte [rax], TOK_IDENT
    jne     parse_err_at_tok
    mov     edi, [rax + 8]
    call    find_or_add_fn
    mov     ebx, eax                ; fn_idx
    mov     al, TOK_RPAREN
    call    expect_tok_kind

    call    alloc_anon_span_slot
    mov     r12d, eax               ; 16B span slot `{ base, len }`

    ; 1. `lea rax, [rip + rel32]` -> `base` (patched via `AST_CALLFIX_OFF`)
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0x058D
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
    mov     edi, r12d
    call    emit_store_local

    ; 2. `mov eax, imm32` -> `len` (patched via `AST_FNLEN_FIX_OFF`)
    mov     al, 0xB8
    call    emit_u8
    mov     edx, [code_off]
    mov     rcx, [space_ast]
    add     rcx, AST_FNLEN_FIX_OFF
    mov     eax, [fnlen_fix_cnt]
    mov     [rcx + rax * 8],     edx
    mov     [rcx + rax * 8 + 4], ebx
    inc     dword [fnlen_fix_cnt]
    xor     eax, eax
    call    emit_u32
    lea     edi, [r12d + 8]
    call    emit_store_local

    mov     edi, r12d
    call    emit_lea_local
    mov     dword [last_expr_ty], TY_U8 | TY_FLAG_SPAN
    pop     r12
    mov     eax, 1
    ret

.do_indirect_call:
    cmp     byte [in_isr_mode], 0
    jne     err_isr_ban_cur
    push    r12
    push    r14
    sub     rsp, 40                 ; [rsp + 0] = fn_ptr temp, [rsp + 4..24] = arg0..5 temps

    mov     al, TOK_LPAREN
    call    expect_tok_kind

    ; Evaluate `fn_ptr` -> check non-null (`fail 3`) -> store in `[rsp + 0]`
    mov     edi, 1
    call    parse_expr
    call    emit_null_view_check_rax
    call    alloc_temp_qword
    mov     [rsp + 0], eax
    mov     edi, eax
    call    emit_store_local

    xor     r12d, r12d              ; arg count (0..6)
.ic_arg_lp:
    call    peek_tok_ptr
    cmp     byte [rax], TOK_COMMA
    jne     .ic_args_done
    cmp     r12d, 6
    jae     parse_err_at_tok
    inc     dword [tok_idx]         ; consume `,`
    mov     edi, 1
    call    parse_expr
    call    alloc_temp_qword
    mov     [rsp + 4 + r12 * 4], eax
    mov     edi, eax
    call    emit_store_local
    inc     r12d
    jmp     .ic_arg_lp

.ic_args_done:
    mov     al, TOK_RPAREN
    call    expect_tok_kind

    cmp     r12d, 1
    jb      .ic_do_call
    mov     edi, [rsp + 4]
    call    emit_load_local
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC789              ; mov rdi, rax
    call    emit_u16
    cmp     r12d, 2
    jb      .ic_do_call
    mov     edi, [rsp + 8]
    call    emit_load_local
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC689              ; mov rsi, rax
    call    emit_u16
    cmp     r12d, 3
    jb      .ic_do_call
    mov     edi, [rsp + 12]
    call    emit_load_local
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC289              ; mov rdx, rax
    call    emit_u16
    cmp     r12d, 4
    jb      .ic_do_call
    mov     edi, [rsp + 16]
    call    emit_load_local
    call    emit_move_rax_to_arg3
    cmp     r12d, 5
    jb      .ic_do_call
    mov     edi, [rsp + 20]
    call    emit_load_local
    mov     al, 0x49
    call    emit_u8
    mov     ax, 0xC089              ; mov r8, rax
    call    emit_u16
    cmp     r12d, 6
    jb      .ic_do_call
    mov     edi, [rsp + 24]
    call    emit_load_local
    mov     al, 0x49
    call    emit_u8
    mov     ax, 0xC189              ; mov r9, rax
    call    emit_u16

.ic_do_call:
    mov     edi, [rsp + 0]
    call    emit_load_local
    mov     al, 0x49
    call    emit_u8
    mov     ax, 0xC389              ; 49 89 C3 = mov r11, rax
    call    emit_u16

    lea     edi, [r12d + 1]
    call    free_temp_qwords

    mov     al, 0x41
    call    emit_u8
    mov     ax, 0xD3FF              ; 41 FF D3 = call r11
    call    emit_u16

    ; Post-call scalar fault unwind check (matches `emit_call_fn_idx`)
    call    emit_fault_cmp_zero
    mov     ax, 0x840F              ; jz .no_fault
    call    emit_u16
    mov     r14d, [code_off]
    xor     eax, eax
    call    emit_u32
    cmp     dword [rec_depth], 0
    jg      .ic_jmp_rec
    mov     ecx, [tile_lex_depth]
    call    emit_tile_unwind_count
    mov     eax, 0xC3C9C031         ; xor eax, eax; leave; ret
    call    emit_u32
    jmp     .ic_patch_ok
.ic_jmp_rec:
    call    emit_unwind_to_recover
.ic_patch_ok:
    mov     edi, r14d
    call    patch_rel32_here

    mov     dword [last_expr_ty], TY_I64
    add     rsp, 40
    pop     r14
    pop     r12
    mov     eax, 1
    ret

.do_raw_emit:
    mov     al, TOK_LPAREN
    call    expect_tok_kind
.em_lp:
    call    .eval_emit_byte_const   ; `rax` = 0..255
    call    emit_u8
    call    peek_tok_ptr
    cmp     byte [rax], TOK_COMMA
    jne     .em_dn
    inc     dword [tok_idx]
    jmp     .em_lp
.em_dn:
    mov     al, TOK_RPAREN
    call    expect_tok_kind
    mov     dword [last_expr_ty], TY_U64
    mov     eax, 1
    ret

.eval_emit_byte_const:
    push    rbx
    push    r14
    mov     ebx, [tok_idx]
    xor     edx, edx
.eb_sc:
    call    peek_tok_ptr
    movzx   ecx, byte [rax]
    test    cl, cl
    jz      parse_err_at_tok
    cmp     cl, TOK_LPAREN
    jne     .eb_rp
    inc     edx
    inc     dword [tok_idx]
    jmp     .eb_sc
.eb_rp:
    cmp     cl, TOK_RPAREN
    jne     .eb_cm
    test    edx, edx
    jz      .eb_fn
    dec     edx
    inc     dword [tok_idx]
    jmp     .eb_sc
.eb_cm:
    test    edx, edx
    jnz     .eb_nx
    cmp     cl, TOK_COMMA
    je      .eb_fn
.eb_nx:
    inc     dword [tok_idx]
    jmp     .eb_sc
.eb_fn:
    mov     r14, rax
    movzx   edx, byte [r14]
    push    rdx
    mov     byte [r14], TOK_EOF
    mov     [tok_idx], ebx
    call    eval_const_expr
    pop     rcx
    mov     [r14], cl
    cmp     rax, 0xFF
    ja      parse_err_cur_tok
    pop     r14
    pop     rbx
    ret

; --- Inline Silicon Bit, Endian, CRC32C & Cache Instructions ---
.eval_1_arg:
    mov     al, TOK_LPAREN
    call    expect_tok_kind
    mov     edi, 1
    call    parse_expr
    mov     al, TOK_RPAREN
    jmp     expect_tok_kind

.eval_2_args_rcx_rax:
    push    rbx
    mov     al, TOK_LPAREN
    call    expect_tok_kind
    mov     edi, 1
    call    parse_expr              ; arg0 -> temp (`rcx`)
    call    alloc_temp_qword
    mov     ebx, eax
    mov     edi, ebx
    call    emit_store_local
    mov     al, TOK_COMMA
    call    expect_tok_kind
    mov     edi, 1
    call    parse_expr              ; arg1 -> `rax`
    mov     al, TOK_RPAREN
    call    expect_tok_kind
    mov     edi, ebx
    call    emit_load_local_rcx
    mov     edi, 1
    call    free_temp_qwords
    pop     rbx
    ret

.do_popcnt:
    call    .eval_1_arg
    lea     rsi, [seq_popcnt]
    mov     ecx, seq_popcnt_end - seq_popcnt
    call    emit_bytes
    mov     dword [last_expr_ty], TY_U64
    mov     eax, 1
    ret

.do_lzcnt:
    call    .eval_1_arg
    lea     rsi, [seq_lzcnt]
    mov     ecx, seq_lzcnt_end - seq_lzcnt
    call    emit_bytes
    mov     dword [last_expr_ty], TY_U64
    mov     eax, 1
    ret

.do_tzcnt:
    call    .eval_1_arg
    lea     rsi, [seq_tzcnt]
    mov     ecx, seq_tzcnt_end - seq_tzcnt
    call    emit_bytes
    mov     dword [last_expr_ty], TY_U64
    mov     eax, 1
    ret

.do_bswap16:
    call    .eval_1_arg
    mov     edi, TY_U16
    call    emit_bswap_for_ty
    mov     dword [last_expr_ty], TY_U16
    mov     eax, 1
    ret

.do_bswap32:
    call    .eval_1_arg
    mov     edi, TY_U32
    call    emit_bswap_for_ty
    mov     dword [last_expr_ty], TY_U32
    mov     eax, 1
    ret

.do_bswap64:
    call    .eval_1_arg
    mov     edi, TY_U64
    call    emit_bswap_for_ty
    mov     dword [last_expr_ty], TY_U64
    mov     eax, 1
    ret

.do_pdep:
    call    .eval_2_args_rcx_rax    ; rcx = val, rax = mask
    lea     rsi, [seq_pdep]
    mov     ecx, seq_pdep_end - seq_pdep
    call    emit_bytes
    mov     dword [last_expr_ty], TY_U64
    mov     eax, 1
    ret

.do_pext:
    call    .eval_2_args_rcx_rax    ; rcx = val, rax = mask
    lea     rsi, [seq_pext]
    mov     ecx, seq_pext_end - seq_pext
    call    emit_bytes
    mov     dword [last_expr_ty], TY_U64
    mov     eax, 1
    ret

.do_crc32c:
    call    .eval_2_args_rcx_rax    ; rcx = acc, rax = val64
    lea     rsi, [seq_crc32c]
    mov     ecx, seq_crc32c_end - seq_crc32c
    call    emit_bytes
    mov     dword [last_expr_ty], TY_U64
    mov     eax, 1
    ret

.do_prefetch:
    call    .eval_1_arg
    lea     rsi, [seq_prefetch]
    mov     ecx, seq_prefetch_end - seq_prefetch
    call    emit_bytes
    mov     dword [last_expr_ty], TY_U64
    mov     eax, 1
    ret

.do_clflush:
    call    .eval_1_arg
    lea     rsi, [seq_clflush]
    mov     ecx, seq_clflush_end - seq_clflush
    call    emit_bytes
    mov     dword [last_expr_ty], TY_U64
    mov     eax, 1
    ret

.do_lfence:
    mov     al, TOK_LPAREN
    call    expect_tok_kind
    mov     al, TOK_RPAREN
    call    expect_tok_kind
    lea     rsi, [seq_lfence]
    mov     ecx, seq_lfence_end - seq_lfence
    call    emit_bytes
    mov     dword [last_expr_ty], TY_U64
    mov     eax, 1
    ret

.do_sfence:
    mov     al, TOK_LPAREN
    call    expect_tok_kind
    mov     al, TOK_RPAREN
    call    expect_tok_kind
    lea     rsi, [seq_sfence]
    mov     ecx, seq_sfence_end - seq_sfence
    call    emit_bytes
    mov     dword [last_expr_ty], TY_U64
    mov     eax, 1
    ret

.do_mfence:
    mov     al, TOK_LPAREN
    call    expect_tok_kind
    mov     al, TOK_RPAREN
    call    expect_tok_kind
    lea     rsi, [seq_mfence]
    mov     ecx, seq_mfence_end - seq_mfence
    call    emit_bytes
    mov     dword [last_expr_ty], TY_U64
    mov     eax, 1
    ret

; --- NASM-Encoded Native Instruction Templates ---
seq_popcnt:
    popcnt  rax, rax
seq_popcnt_end:

seq_lzcnt:
    lzcnt   rax, rax
seq_lzcnt_end:

seq_tzcnt:
    tzcnt   rax, rax
seq_tzcnt_end:

seq_pdep:
    pdep    rax, rcx, rax
seq_pdep_end:

seq_pext:
    pext    rax, rcx, rax
seq_pext_end:

seq_crc32c:
    xchg    rax, rcx                ; rax = acc, rcx = val64
    crc32   rax, rcx
seq_crc32c_end:

seq_prefetch:
    prefetcht0 [rax]
seq_prefetch_end:

seq_clflush:
    clflushopt [rax]
seq_clflush_end:

seq_lfence:
    lfence
seq_lfence_end:

seq_sfence:
    sfence
seq_sfence_end:

seq_mfence:
    mfence
seq_mfence_end:
