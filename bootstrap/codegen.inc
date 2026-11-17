; =============================================================================
; Zero Stage-0 Bootstrap — Native x86-64 Code Emitter (`bootstrap/codegen.inc`)
; =============================================================================
; Guarantees strict `0 mod 16` RSP alignment at every `call` site by allocating
; expression & call temporaries inside the fixed 16B-aligned function frame
; rather than mutating RSP with dynamic push/pop.
; =============================================================================

emit_u8:
    mov     rdx, [space_out]
    mov     edi, [code_off]
    mov     [rdx + rdi], al
    inc     edi
    mov     [code_off], edi
    ret

emit_u16:
    mov     rdx, [space_out]
    mov     edi, [code_off]
    mov     [rdx + rdi], ax
    add     edi, 2
    mov     [code_off], edi
    ret

emit_u32:
    mov     rdx, [space_out]
    mov     edi, [code_off]
    mov     [rdx + rdi], eax
    add     edi, 4
    mov     [code_off], edi
    ret

emit_u64:
    mov     rdx, [space_out]
    mov     edi, [code_off]
    mov     [rdx + rdi], rax
    add     edi, 8
    mov     [code_off], edi
    ret

; rsi = byte ptr, ecx = byte len
emit_bytes:
    test    ecx, ecx
    jz      .done
    mov     rdi, [space_out]
    mov     eax, [code_off]
    add     rdi, rax
    add     [code_off], ecx
    rep     movsb
.done:
    ret

; emit_mov_rax_imm64(rax = 64-bit constant)
emit_mov_rax_imm64:
    test    rax, rax
    jnz     .chk_u32
    mov     ax, 0xC031              ; 31 C0 = xor eax, eax
    jmp     emit_u16
.chk_u32:
    mov     rcx, rax
    shr     rcx, 32
    jnz     .full64
    push    rax
    mov     al, 0xB8                ; B8 <imm32> = mov eax, imm32
    call    emit_u8
    pop     rax
    jmp     emit_u32
.full64:
    push    rax
    mov     ax, 0xB848              ; 48 B8 <imm64> = movabs rax, imm64
    call    emit_u16
    pop     rax
    jmp     emit_u64

; emit_mov_rcx_imm64(rax = 64-bit constant)
emit_mov_rcx_imm64:
    push    rax
    mov     ax, 0xB948              ; 48 B9 <imm64> = movabs rcx, imm64
    call    emit_u16
    pop     rax
    jmp     emit_u64

; -----------------------------------------------------------------------------
; Target-Aware Fault Word & Reactor ABI Arg3 Emitters
;   - Host `ELF64`: `qword [0x10000000]`, Arg3 = `rcx` (`abi-host`)
;   - Bare-Metal `EFI`: per-core `qword [gs:0]` (`ReactorLocal.fault`), Arg3 = `r10`
; -----------------------------------------------------------------------------
emit_fault_store_imm32:             ; edi = fault_code (1, 2, 3, ...)
    cmp     byte [in_isr_mode], 0
    jne     err_isr_ban_cur
    push    rdi
    cmp     byte [mode_efi], 1
    je      .efi
    mov     al, 0xBA                ; mov edx, FAULT_WORD_VADDR
    call    emit_u8
    mov     eax, FAULT_WORD_VADDR
    call    emit_u32
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0x02C7              ; mov qword [rdx], imm32
    call    emit_u16
    pop     rax
    jmp     emit_u32
.efi:
    lea     rsi, [seq_gs0_store_imm_pre]
    mov     ecx, seq_gs0_store_imm_pre_end - seq_gs0_store_imm_pre
    call    emit_bytes
    pop     rax
    jmp     emit_u32

emit_fault_store_rax:               ; guest `rax` = non-zero fault_code
    cmp     byte [in_isr_mode], 0
    jne     err_isr_ban_cur
    cmp     byte [mode_efi], 1
    je      .efi
    mov     al, 0xB9                ; mov ecx, FAULT_WORD_VADDR
    call    emit_u8
    mov     eax, FAULT_WORD_VADDR
    call    emit_u32
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0x0189              ; mov [rcx], rax
    jmp     emit_u16
.efi:
    lea     rsi, [seq_gs0_store_rax]
    mov     ecx, seq_gs0_store_rax_end - seq_gs0_store_rax
    jmp     emit_bytes

emit_fault_cmp_zero:
    cmp     byte [mode_efi], 1
    je      .efi
    mov     al, 0xB9                ; mov ecx, FAULT_WORD_VADDR
    call    emit_u8
    mov     eax, FAULT_WORD_VADDR
    call    emit_u32
    mov     eax, 0x00398348         ; cmp qword [rcx], 0
    jmp     emit_u32
.efi:
    lea     rsi, [seq_gs0_cmp_zero]
    mov     ecx, seq_gs0_cmp_zero_end - seq_gs0_cmp_zero
    jmp     emit_bytes

emit_fault_take_rax:                ; loads fault word into `rax` and clears it to `0`
    cmp     byte [mode_efi], 1
    je      .efi
    mov     al, 0xB9                ; mov ecx, FAULT_WORD_VADDR
    call    emit_u8
    mov     eax, FAULT_WORD_VADDR
    call    emit_u32
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0x018B              ; mov rax, [rcx]
    call    emit_u16
    mov     ax, 0xD231              ; xor edx, edx
    call    emit_u16
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0x1189              ; mov [rcx], rdx
    jmp     emit_u16
.efi:
    lea     rsi, [seq_gs0_take_rax]
    mov     ecx, seq_gs0_take_rax_end - seq_gs0_take_rax
    jmp     emit_bytes

emit_move_rax_to_arg3:
    cmp     byte [mode_efi], 1
    je      .efi
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC189              ; 48 89 C1 = mov rcx, rax (`abi-host`)
    jmp     emit_u16
.efi:
    mov     al, 0x49
    call    emit_u8
    mov     ax, 0xC289              ; 49 89 C2 = mov r10, rax (`Reactor ABI` §6.1)
    jmp     emit_u16

seq_gs0_store_imm_pre:
    db 0x65, 0x48, 0xC7, 0x04, 0x25, 0x00, 0x00, 0x00, 0x00 ; mov qword [gs:0], <imm32>
seq_gs0_store_imm_pre_end:

seq_gs0_store_rax:
    mov     [gs:0], rax
seq_gs0_store_rax_end:

seq_gs0_cmp_zero:
    cmp     qword [gs:0], 0
seq_gs0_cmp_zero_end:

seq_gs0_take_rax:
    mov     rax, [gs:0]
    mov     qword [gs:0], 0
seq_gs0_take_rax_end:

; Host `rax` = `SpaceEntry.vaddr_base`; emits load into guest `rcx` (guest `rax` preserved!)
emit_load_space_base_rcx:
    cmp     byte [mode_efi], 1
    je      .efi
    jmp     emit_mov_rcx_imm64
.efi:
    sub     rax, DATA_VADDR_BASE
    push    rax
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0x0D8D              ; 48 8D 0D <disp32> = lea rcx, [rip + disp32]
    call    emit_u16
    pop     rdi
    jmp     emit_efi_bss_disp32

; Host `rax` = `SpaceEntry.vaddr_base`; emits load into guest `rax`
emit_load_space_base_rax:
    cmp     byte [mode_efi], 1
    je      .efi
    jmp     emit_mov_rax_imm64
.efi:
    sub     rax, DATA_VADDR_BASE
    push    rax
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0x058D              ; 48 8D 05 <disp32> = lea rax, [rip + disp32]
    call    emit_u16
    pop     rdi
    jmp     emit_efi_bss_disp32

; emit_load_local(edi = signed rbp disp) -> `mov rax, [rbp + disp]` (`disp8` if `-128..127`)
emit_load_local:
    push    rdi
    cmp     edi, -128
    jl      .d32
    cmp     edi, 127
    jg      .d32
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0x458B              ; 48 8B 45 <disp8>
    call    emit_u16
    pop     rax
    jmp     emit_u8
.d32:
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0x858B              ; 48 8B 85 <disp32>
    call    emit_u16
    pop     rax
    jmp     emit_u32

; emit_load_local_rcx(edi = signed rbp disp) -> `mov rcx, [rbp + disp]`
emit_load_local_rcx:
    push    rdi
    cmp     edi, -128
    jl      .d32
    cmp     edi, 127
    jg      .d32
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0x4D8B              ; 48 8B 4D <disp8>
    call    emit_u16
    pop     rax
    jmp     emit_u8
.d32:
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0x8D8B              ; 48 8B 8D <disp32>
    call    emit_u16
    pop     rax
    jmp     emit_u32

; emit_store_local(edi = signed rbp disp) -> `mov [rbp + disp], rax`
emit_store_local:
    push    rdi
    cmp     edi, -128
    jl      .d32
    cmp     edi, 127
    jg      .d32
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0x4589              ; 48 89 45 <disp8>
    call    emit_u16
    pop     rax
    jmp     emit_u8
.d32:
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0x8589              ; 48 89 85 <disp32>
    call    emit_u16
    pop     rax
    jmp     emit_u32

; emit_lea_local(edi = signed rbp disp) -> `lea rax, [rbp + disp]`
emit_lea_local:
    push    rdi
    cmp     edi, -128
    jl      .d32
    cmp     edi, 127
    jg      .d32
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0x458D              ; 48 8D 45 <disp8>
    call    emit_u16
    pop     rax
    jmp     emit_u8
.d32:
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0x858D              ; 48 8D 85 <disp32>
    call    emit_u16
    pop     rax
    jmp     emit_u32

; emit_lea_local_rcx(edi = signed rbp disp) -> `lea rcx, [rbp + disp]`
emit_lea_local_rcx:
    push    rdi
    cmp     edi, -128
    jl      .d32
    cmp     edi, 127
    jg      .d32
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0x4D8D              ; 48 8D 4D <disp8>
    call    emit_u16
    pop     rax
    jmp     emit_u8
.d32:
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0x8D8D              ; 48 8D 8D <disp32>
    call    emit_u16
    pop     rax
    jmp     emit_u32

; -----------------------------------------------------------------------------
; Frame-Allocated Reusable Temporary Slots (`0 mod 16` RSP Preserving!)
; Named locals grow `frame_qwords` permanently during a function; expression &
; call temporaries allocate above `frame_qwords + temp_depth`, tracking peak
; `max_frame_qwords` patched into prologue `sub rsp, imm32` at function end.
; -----------------------------------------------------------------------------
alloc_temp_qword:
    inc     dword [temp_depth]
    mov     eax, [frame_qwords]
    add     eax, [temp_depth]
    cmp     eax, [max_frame_qwords]
    jbe     .calc
    mov     [max_frame_qwords], eax
.calc:
    shl     eax, 3
    neg     eax                     ; negative rbp_disp32
    ret

free_temp_qwords:                   ; edi = count to release
    sub     [temp_depth], edi
    ret

; -----------------------------------------------------------------------------
; Width-Aware Memory Load (`rax = *(T*)rax`) & Store (`*(T*)rcx = rax`)
; edi = TY_* type tag
; -----------------------------------------------------------------------------
emit_load_mem_typed:
    test    edi, TY_FLAG_VIEW_ONE | TY_FLAG_ATOMIC
    jnz     .u64
    and     edi, TY_BASE_MASK
    cmp     edi, TY_CUSTOM_PTR_BASE
    jae     .u64
    cmp     edi, TY_BITPACK_BASE
    jae     .check_bp
    cmp     edi, TY_U8
    je      .u8
    cmp     edi, TY_I8
    je      .i8
    cmp     edi, TY_U16
    je      .u16
    cmp     edi, TY_I16
    je      .i16
    cmp     edi, TY_U32
    je      .u32
    cmp     edi, TY_I32
    je      .i32
.u64:
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0x008B              ; 48 8B 00 = mov rax, [rax]
    jmp     emit_u16
.u8:
    mov     al, 0x0F
    call    emit_u8
    mov     ax, 0x00B6              ; 0F B6 00 = movzx eax, byte [rax]
    jmp     emit_u16
.i8:
    mov     eax, 0x00BE0F48         ; 48 0F BE 00 = movsx rax, byte [rax]
    jmp     emit_u32
.u16:
    mov     al, 0x0F
    call    emit_u8
    mov     ax, 0x00B7              ; 0F B7 00 = movzx eax, word [rax]
    jmp     emit_u16
.i16:
    mov     eax, 0x00BF0F48         ; 48 0F BF 00 = movsx rax, word [rax]
    jmp     emit_u32
.u32:
    mov     ax, 0x008B              ; 8B 00 = mov eax, [rax]
    jmp     emit_u16
.i32:
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0x0063              ; 48 63 00 = movsxd rax, dword [rax]
    jmp     emit_u16
.check_bp:
    cmp     edi, TY_STRUCT_BASE
    jae     .done                   ; structs leave address in `rax`
    sub     edi, TY_BITPACK_BASE
    shl     edi, 5
    mov     rcx, [space_ast]
    mov     eax, [rcx + AST_TYPE_OFF + rdi + 8] ; byte_size (1,2,4,8)
    cmp     eax, 1
    je      .u8
    cmp     eax, 2
    je      .u16
    cmp     eax, 4
    je      .u32
    jmp     .u64
.done:
    ret

; Target `rcx` = dst addr, `rax` = value (or src struct ptr), `edi` = TY_* tag
emit_store_mem_typed:
    test    edi, TY_FLAG_VIEW_ONE | TY_FLAG_ATOMIC
    jnz     .b8
    test    edi, TY_FLAG_STREAM
    jnz     .stream_nt
    and     edi, TY_BASE_MASK
    cmp     edi, TY_CUSTOM_PTR_BASE
    jae     .b8
    cmp     edi, TY_STRUCT_BASE
    jae     .struct_copy
    cmp     edi, TY_BITPACK_BASE
    jae     .bp_store
    cmp     edi, TY_U8
    je      .b1
    cmp     edi, TY_I8
    je      .b1
    cmp     edi, TY_U16
    je      .b2
    cmp     edi, TY_I16
    je      .b2
    cmp     edi, TY_U32
    je      .b4
    cmp     edi, TY_I32
    je      .b4
.b8:
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0x0189              ; 48 89 01 = mov [rcx], rax
    jmp     emit_u16
.stream_nt:
    and     edi, TY_BASE_MASK
    cmp     edi, TY_U32
    je      .nt4
    cmp     edi, TY_I32
    je      .nt4
    mov     al, 0x48
    call    emit_u8
.nt4:
    mov     ax, 0xC30F              ; [48] 0F C3 01 = movnti [rcx], eax/rax
    call    emit_u16
    mov     al, 0x01
    jmp     emit_u8
.b1:
    mov     ax, 0x0188              ; 88 01 = mov [rcx], al
    jmp     emit_u16
.b2:
    mov     al, 0x66
    call    emit_u8
    mov     ax, 0x0189              ; 66 89 01 = mov [rcx], ax
    jmp     emit_u16
.b4:
    mov     ax, 0x0189              ; 89 01 = mov [rcx], eax
    jmp     emit_u16
.bp_store:
    sub     edi, TY_BITPACK_BASE
    shl     edi, 5
    mov     rdx, [space_ast]
    mov     eax, [rdx + AST_TYPE_OFF + rdi + 8]
    cmp     eax, 1
    je      .b1
    cmp     eax, 2
    je      .b2
    cmp     eax, 4
    je      .b4
    jmp     .b8

.struct_copy:
    sub     edi, TY_STRUCT_BASE
    shl     edi, 5
    mov     rdx, [space_ast]
    mov     eax, [rdx + AST_TYPE_OFF + rdi + 8]
    push    rax
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC689              ; 48 89 C6 = mov rsi, rax (src)
    call    emit_u16
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xCF89              ; 48 89 CF = mov rdi, rcx (dst)
    call    emit_u16
    mov     al, 0xB9                ; B9 <imm32> = mov ecx, byte_size
    call    emit_u8
    pop     rax
    call    emit_u32
    mov     ax, 0xA4F3              ; F3 A4 = rep movsb
    jmp     emit_u16

; -----------------------------------------------------------------------------
; Span Bounds (`fail 2`), Null View (`fail 3`), & Immediate-RHS Binary Emitter
; -----------------------------------------------------------------------------
; Checks guest `rax` (index) < span length `[rbp + edi + 8]`, else `fail 2`.
; Preserves guest `rax`.
emit_span_bounds_check_rax:
    add     edi, 8
    call    emit_load_local_rcx     ; rcx = span.len
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC839              ; 48 39 C8 = cmp rax, rcx (unsigned)
    call    emit_u16
    mov     ax, 0x820F              ; 0F 82 <rel32> = jb .in_bounds
    call    emit_u16
    mov     eax, [code_off]
    push    rax
    xor     eax, eax
    call    emit_u32
    mov     edi, 2
    call    emit_raise_fault_imm    ; fail 2
    pop     rdi
    jmp     patch_rel32_here

; Checks guest `rax` (pointer) != 0, else `fail 3`. Preserves guest `rax`.
emit_null_view_check_rax:
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC085              ; 48 85 C0 = test rax, rax
    call    emit_u16
    mov     ax, 0x850F              ; 0F 85 <rel32> = jnz .non_null
    call    emit_u16
    mov     eax, [code_off]
    push    rax
    xor     eax, eax
    call    emit_u32
    mov     edi, 3
    call    emit_raise_fault_imm    ; fail 3
    pop     rdi
    jmp     patch_rel32_here

; Precedence-safe immediate-RHS instruction selector
; Inputs: `r12d = op_tok`, `ecx = op_prec`, `r14d = LHS type tag`
; Returns: `eax = 1` if handled (consuming RHS token & updating `[last_expr_ty]`), else `0`.
try_emit_imm_rhs_binop:
    push    rbx
    push    r13
    mov     ebx, ecx                ; op_prec

    call    peek_tok_ptr
    movzx   edx, byte [rax]
    cmp     dl, TOK_INT
    je      .imm_from_tok
    cmp     dl, TOK_CHAR
    je      .imm_from_tok
    cmp     dl, TOK_IDENT
    jne     .fallback

    ; Must NOT be followed by postfix call/index/member/descriptor `<`/`{`
    movzx   ecx, byte [rax + 16]
    cmp     cl, TOK_LPAREN
    je      .fallback
    cmp     cl, TOK_LBRACK
    je      .fallback
    cmp     cl, TOK_DOT
    je      .fallback
    cmp     cl, TOK_LT
    je      .fallback
    cmp     cl, TOK_LBRACE
    je      .fallback

    ; Respect local shadowing: locals/spaces must NOT resolve as constants
    push    rax
    mov     edi, [rax + 8]
    call    find_local_entry
    test    eax, eax
    pop     rax
    jnz     .fallback

    push    rax
    mov     edi, [rax + 8]
    call    find_space
    test    rax, rax
    pop     rax
    jnz     .fallback

    push    rax
    mov     edi, [rax + 8]
    call    find_const
    test    eax, eax
    pop     rax
    jz      .fallback
    mov     r13, rdx
    jmp     .chk_prec

.imm_from_tok:
    mov     r13, [rax + 8]

.chk_prec:
    ; Precedence check against token AFTER `t0` (`[rax + 16]`)
    inc     dword [tok_idx]
    movzx   eax, byte [rax + 16]
    call    binop_precedence
    dec     dword [tok_idx]
    cmp     ecx, ebx
    ja      .fallback

    cmp     r12d, TOK_PLUS
    je      .do_add
    cmp     r12d, TOK_MINUS
    je      .do_sub
    cmp     r12d, TOK_SHL
    je      .do_shl
    cmp     r12d, TOK_SHR
    je      .do_shr
    cmp     r12d, TOK_AMP
    je      .do_and
    cmp     r12d, TOK_STAR
    je      .do_mul_pow2
    jmp     .fallback

.do_add:
    movsxd  rcx, r13d
    cmp     rcx, r13
    jne     .fallback
    mov     dword [last_expr_ty], TY_I64
    test    r13, r13
    jz      .consumed
    cmp     r13, -128
    jl      .add_32
    cmp     r13, 127
    jg      .add_32
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC083              ; add rax, imm8
    call    emit_u16
    mov     eax, r13d
    call    emit_u8
    jmp     .consumed
.add_32:
    mov     al, 0x48
    call    emit_u8
    mov     al, 0x05                ; add rax, imm32
    call    emit_u8
    mov     eax, r13d
    call    emit_u32
    jmp     .consumed

.do_sub:
    movsxd  rcx, r13d
    cmp     rcx, r13
    jne     .fallback
    mov     dword [last_expr_ty], TY_I64
    test    r13, r13
    jz      .consumed
    cmp     r13, -128
    jl      .sub_32
    cmp     r13, 127
    jg      .sub_32
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xE883              ; sub rax, imm8
    call    emit_u16
    mov     eax, r13d
    call    emit_u8
    jmp     .consumed
.sub_32:
    mov     al, 0x48
    call    emit_u8
    mov     al, 0x2D                ; sub rax, imm32
    call    emit_u8
    mov     eax, r13d
    call    emit_u32
    jmp     .consumed

.do_shl:
    cmp     r13, 63
    ja      .fallback
    mov     edi, r14d
    call    is_ty_unsigned
    mov     edx, TY_I64
    test    eax, eax
    jz      .shl_ty
    mov     edx, TY_U64
.shl_ty:
    mov     [last_expr_ty], edx
    test    r13, r13
    jz      .consumed
    cmp     r13, 1
    jne     .shl_n
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xE0D1              ; shl rax, 1
    call    emit_u16
    jmp     .consumed
.shl_n:
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xE0C1              ; shl rax, imm8
    call    emit_u16
    mov     eax, r13d
    call    emit_u8
    jmp     .consumed

.do_shr:
    cmp     r13, 63
    ja      .fallback
    mov     edi, r14d
    call    is_ty_unsigned
    test    eax, eax
    jz      .shr_signed
    mov     dword [last_expr_ty], TY_U64
    test    r13, r13
    jz      .consumed
    cmp     r13, 1
    jne     .shr_u_n
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xE8D1              ; shr rax, 1
    call    emit_u16
    jmp     .consumed
.shr_u_n:
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xE8C1              ; shr rax, imm8
    call    emit_u16
    mov     eax, r13d
    call    emit_u8
    jmp     .consumed
.shr_signed:
    mov     dword [last_expr_ty], TY_I64
    test    r13, r13
    jz      .consumed
    cmp     r13, 1
    jne     .shr_s_n
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xF8D1              ; sar rax, 1
    call    emit_u16
    jmp     .consumed
.shr_s_n:
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xF8C1              ; sar rax, imm8
    call    emit_u16
    mov     eax, r13d
    call    emit_u8
    jmp     .consumed

.do_and:
    cmp     r13, 0x7FFFFFFF
    ja      .fallback
    mov     dword [last_expr_ty], TY_I64
    cmp     r13, 127
    ja      .and_32
    mov     ax, 0xE083              ; 83 E0 <imm8> = and eax, imm8 (0..127 -> 64b zero-ext)
    call    emit_u16
    mov     eax, r13d
    call    emit_u8
    jmp     .consumed
.and_32:
    mov     al, 0x25                ; 25 <imm32> = and eax, imm32 (0..0x7FFFFFFF -> 64b zero-ext)
    call    emit_u8
    mov     eax, r13d
    call    emit_u32
    jmp     .consumed

.do_mul_pow2:
    test    r13, r13
    jle     .fallback
    lea     rax, [r13 - 1]
    test    r13, rax
    jnz     .fallback
    bsf     r13, r13                ; r13 = shift count 0..62
    mov     dword [last_expr_ty], TY_I64
    test    r13, r13
    jz      .consumed
    cmp     r13, 1
    jne     .mul_shl_n
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xE0D1              ; shl rax, 1
    call    emit_u16
    jmp     .consumed
.mul_shl_n:
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xE0C1              ; shl rax, imm8
    call    emit_u16
    mov     eax, r13d
    call    emit_u8

.consumed:
    inc     dword [tok_idx]
    mov     eax, 1
    pop     r13
    pop     rbx
    ret

.fallback:
    xor     eax, eax
    pop     r13
    pop     rbx
    ret

; -----------------------------------------------------------------------------
; Branch & Patch Emitters
; -----------------------------------------------------------------------------
emit_jmp_rel32:
    mov     al, 0xE9
    call    emit_u8
    mov     eax, [code_off]
    push    rax
    xor     eax, eax
    call    emit_u32
    pop     rax
    ret

emit_jz_rel32:
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC085              ; test rax, rax
    call    emit_u16
    mov     ax, 0x840F              ; jz rel32
    call    emit_u16
    mov     eax, [code_off]
    push    rax
    xor     eax, eax
    call    emit_u32
    pop     rax
    ret

emit_jnz_rel32:
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC085              ; test rax, rax
    call    emit_u16
    mov     ax, 0x850F              ; jnz rel32
    call    emit_u16
    mov     eax, [code_off]
    push    rax
    xor     eax, eax
    call    emit_u32
    pop     rax
    ret

patch_rel32_here:
    mov     eax, [code_off]
    lea     ecx, [rdi + 4]
    sub     eax, ecx
    mov     rdx, [space_out]
    mov     [rdx + rdi], eax
    ret

emit_jmp_to:
    push    rdi
    mov     al, 0xE9
    call    emit_u8
    pop     rdi
    mov     eax, [code_off]
    add     eax, 4
    sub     edi, eax
    mov     eax, edi
    jmp     emit_u32

emit_call_direct_off:
    push    rdi
    mov     al, 0xE8
    call    emit_u8
    pop     rdi
    mov     eax, [code_off]
    add     eax, 4
    sub     edi, eax
    mov     eax, edi
    jmp     emit_u32

; emit_write_rodata_slice(rsi = bytes_ptr, ecx = byte_len)
emit_write_rodata_slice:
    test    ecx, ecx
    jz      .skip
    push    r12
    push    r13

    mov     r12d, ecx
    mov     r13d, [tgt_ro_off]
    mov     rdi,  [space_tgt_ro]
    add     rdi,  r13
    rep     movsb
    add     [tgt_ro_off], r12d

    mov     al, 0xB8
    call    emit_u8
    mov     eax, 1
    call    emit_u32
    mov     al, 0xBF
    call    emit_u8
    mov     eax, 1
    call    emit_u32
    mov     al, 0xBE
    call    emit_u8
    mov     edx, [code_off]

    mov     eax, [fixup_cnt]
    mov     rcx, [space_fixup]
    mov     [rcx + rax * 8],     edx
    mov     [rcx + rax * 8 + 4], r13d
    inc     dword [fixup_cnt]

    xor     eax, eax
    call    emit_u32
    mov     al, 0xBA
    call    emit_u8
    mov     eax, r12d
    call    emit_u32
    mov     ax, 0x050F
    call    emit_u16

    pop     r13
    pop     r12
.skip:
    ret

; -----------------------------------------------------------------------------
; Target Binary `_start` Trampoline + `__zr_write_i64`
; -----------------------------------------------------------------------------
emit_target_runtime:
    ; Pass Linux `_start` `(argc, argv)` in `(rdi, rsi)` before aligning `rsp`!
    lea     rsi, [seq_elf_entry_args]
    mov     ecx, seq_elf_entry_args_end - seq_elf_entry_args
    call    emit_bytes

    ; `call main`
    mov     edi, [sym_main_id]
    call    find_or_add_fn
    mov     edi, eax
    call    emit_call_fn_idx

    ; Check unhandled scalar fault at `qword [FAULT_WORD_VADDR]`
    lea     rsi, [target_exit_tail]
    mov     ecx, target_exit_tail_end - target_exit_tail
    call    emit_bytes

    ; Emit `__zr_write_i64`
    mov     eax, [code_off]
    mov     [rt_write_i64_off], eax
    lea     rsi, [target_rt_write_i64]
    mov     ecx, target_rt_write_i64_end - target_rt_write_i64
    call    emit_bytes
    ret

seq_elf_entry_args:
    mov     rdi, [rsp]              ; arg0 = argc
    lea     rsi, [rsp + 8]          ; arg1 = &argv[0]
    and     rsp, -16
seq_elf_entry_args_end:

target_exit_tail:
    mov     ecx, FAULT_WORD_VADDR
    mov     rcx, [rcx]
    test    rcx, rcx
    jz      .clean
    mov     edi, 1                  ; exit(1) on unhandled scalar fault!
    jmp     .out
.clean:
    mov     rdi, rax
.out:
    mov     eax, 60
    syscall
target_exit_tail_end:

target_rt_write_i64:
    push    rbp
    mov     rbp, rsp
    sub     rsp, 32
    lea     rsi, [rbp - 1]
    mov     rax, rdi
    xor     r8d, r8d
    test    rax, rax
    jns     .pos
    neg     rax
    mov     r8d, 1
.pos:
    mov     ecx, 10
.digits:
    xor     edx, edx
    div     rcx
    add     dl, '0'
    mov     [rsi], dl
    dec     rsi
    test    rax, rax
    jnz     .digits
    test    r8d, r8d
    jz      .out
    mov     byte [rsi], '-'
    dec     rsi
.out:
    inc     rsi
    mov     rdx, rbp
    sub     rdx, rsi
    mov     edi, 1
    mov     eax, 1
    syscall
    leave
    ret
target_rt_write_i64_end:
