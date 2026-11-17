; =============================================================================
; Zero Stage-0 Bootstrap — Hardware-Lens Loop Engine (`bootstrap/loops.inc`)
; =============================================================================
; Lowers all `for` loop forms directly to native x86-64 machine code:
;   1. `for bit in mask <bits>`               -> BMI1 `tzcnt` + VEX `blsr`
;   2. `for i in lo..hi [<rev | stride: S | unroll: K>]`
;   3. `for [idx, ] elem in span_or_slab [<rev | stride: S | prefetch: D>]`
; Rejects any unlowered `<lens>` (`<tile>`, `<simd>`) with `msg_unimpl_lens`.
; =============================================================================

%define FLENS_REV       1
%define FLENS_PREFETCH  2
%define FLENS_TILE      4

; Stack frame layout inside `parse_stmt_for_ext` (`sub rsp, 96`):
;   [rsp + 0]  u32  has_rec
;   [rsp + 4]  u32  saved_local_cnt
;   [rsp + 8]  u32  var1_sym
;   [rsp + 12] u32  var1_line
;   [rsp + 16] u32  var2_sym        (0 if 1-var loop)
;   [rsp + 20] u32  lens_flags      (FLENS_*)
;   [rsp + 24] u64  stride_val      (default 1)
;   [rsp + 32] u32  unroll_val      (default 1: 1, 2, 4, or 8)
;   [rsp + 36] u32  prefetch_dist   (u32 byte offset)
;   [rsp + 40] i32  idx_disp        (rbp_disp of induction index)
;   [rsp + 44] i32  bound_disp      (rbp_disp of hi/len bound)
;   [rsp + 48] i32  base_disp       (rbp_disp of span/slab base ptr)
;   [rsp + 52] i32  elem_disp       (rbp_disp of bound element var)
;   [rsp + 56] u32  elem_ty
;   [rsp + 60] u32  elem_size
;   [rsp + 64] u32  body_tok_start
;   [rsp + 68] u32  body_tok_after

parse_stmt_for_ext:
    push    rbx
    push    r12
    push    r13
    push    r14
    push    r15
    sub     rsp, 96

    mov     eax, [local_cnt]
    mov     [rsp + 4], eax
    inc     dword [scope_depth]

    mov     dword [rsp + 16], 0     ; var2_sym = 0
    mov     dword [rsp + 20], 0     ; lens_flags = 0
    mov     qword [rsp + 24], 1     ; stride = 1
    mov     dword [rsp + 32], 1     ; unroll = 1
    mov     dword [rsp + 36], 0     ; prefetch_dist = 0

    inc     dword [tok_idx]         ; consume `for`
    call    next_tok_ptr
    cmp     byte [rax], TOK_IDENT
    jne     parse_err_at_tok
    mov     ecx, [rax + 8]
    mov     edx, [rax + 4]
    mov     [rsp + 8],  ecx         ; var1_sym
    mov     [rsp + 12], edx         ; var1_line

    call    peek_tok_ptr
    cmp     byte [rax], TOK_COMMA
    jne     .expect_in
    inc     dword [tok_idx]         ; consume `,`
    call    next_tok_ptr
    cmp     byte [rax], TOK_IDENT
    jne     parse_err_at_tok
    mov     ecx, [rax + 8]
    mov     [rsp + 16], ecx         ; var2_sym (`for idx, elem in ...`)

.expect_in:
    mov     al, TOK_KW
    mov     ah, KW_IN
    call    expect_tok_kw

    ; Check if target after `in` is a bare `@slab` / `@arena` identifier!
    call    peek_tok_ptr
    cmp     byte [rax], TOK_IDENT
    jne     .eval_first_expr
    cmp     byte [rax + 16], TOK_LBRACK
    je      .eval_first_expr
    cmp     byte [rax + 16], TOK_DOT
    je      .eval_first_expr
    cmp     byte [rax + 16], TOK_LPAREN
    je      .eval_first_expr

    mov     edi, [rax + 8]
    call    find_space
    test    rax, rax
    jz      .eval_first_expr
    cmp     dword [rax + 4], SKIND_PORT
    je      parse_err_cur_tok

    ; --- Direct `@slab` / `@arena` Iteration! ---
    mov     r14, rax                ; &SpaceEntry
    inc     dword [tok_idx]         ; consume space identifier

    call    alloc_anon_local
    mov     [rsp + 48], eax         ; base_disp
    mov     rax, [r14 + 8]
    call    emit_load_space_base_rax
    mov     edi, [rsp + 48]
    call    emit_store_local

    call    alloc_anon_local
    mov     [rsp + 44], eax         ; bound_disp (len = capacity)
    mov     rax, [r14 + 24]
    call    emit_mov_rax_imm64
    mov     edi, [rsp + 44]
    call    emit_store_local

    mov     eax, [r14 + 20]
    mov     [rsp + 56], eax         ; elem_ty
    mov     eax, [r14 + 16]
    mov     [rsp + 60], eax         ; elem_size
    jmp     .span_loop_common

.eval_first_expr:
    ; Evaluate `lo` / `mask` / `span_expr` BEFORE binding loop vars!
    mov     edi, 1
    call    parse_expr
    mov     r15d, [last_expr_ty]

    test    r15d, TY_FLAG_SPAN
    jnz     .from_span_expr

    ; Must be 1-var loop (`..` range or `<bits>`)
    cmp     dword [rsp + 16], 0
    jne     parse_err_cur_tok

    call    peek_tok_ptr
    cmp     byte [rax], TOK_DOT2
    je      .range_for

    ; --- `<bits>` Loop (`tzcnt` + `blsr`) ---
    mov     edi, [rsp + 8]
    mov     esi, TY_I64
    mov     edx, [rsp + 12]
    call    alloc_local_scoped
    mov     r12d, eax

    call    alloc_anon_local
    mov     r13d, eax
    mov     edi, r13d
    call    emit_store_local

    call    peek_tok_ptr
    mov     esi, [rax + 4]
    cmp     byte [rax], TOK_LT
    jne     .reject_lens
    inc     dword [tok_idx]

    call    next_tok_ptr
    mov     esi, [rax + 4]
    cmp     byte [rax], TOK_IDENT
    jne     .reject_lens
    mov     edx, [sym_bits_id]
    cmp     [rax + 8], edx
    jne     .reject_lens

    call    peek_tok_ptr
    mov     esi, [rax + 4]
    cmp     byte [rax], TOK_GT
    jne     .reject_lens
    inc     dword [tok_idx]

    call    block_has_trailing_recover
    mov     [rsp], eax
    test    eax, eax
    jz      .bits_go
    call    rec_push_scope
.bits_go:
    call    loop_push_scope
    mov     r14d, [code_off]

    mov     edi, r13d
    call    emit_load_local
    call    emit_jz_rel32
    mov     r15d, eax

    mov     al, 0xF3
    call    emit_u8
    mov     eax, 0xC0BC0F48         ; tzcnt rax, rax
    call    emit_u32
    mov     edi, r12d
    call    emit_store_local

    call    parse_block_no_scope_push

    call    loop_patch_continues_here
    mov     edi, r13d
    call    emit_load_local
    mov     al, 0xC4
    call    emit_u8
    mov     eax, 0xC8F3F8E2         ; blsr rax, rax
    call    emit_u32
    mov     edi, r13d
    call    emit_store_local

    mov     edi, r14d
    call    emit_jmp_to
    mov     edi, r15d
    call    patch_rel32_here
    call    loop_pop_and_patch_breaks_here
    jmp     .rewind_for_scope

; -----------------------------------------------------------------------------
; Span / Slice Iteration (`for [idx, ] elem in span [<rev | stride: S | prefetch: D>]`)
; -----------------------------------------------------------------------------
.from_span_expr:
    ; `rax` points to `{ base, len }`
    mov     al, 0x49
    call    emit_u8
    mov     ax, 0xC089              ; mov r8, rax
    call    emit_u16

    call    alloc_anon_local
    mov     [rsp + 48], eax         ; base_disp
    mov     al, 0x49
    call    emit_u8
    mov     ax, 0x008B              ; mov rax, [r8]
    call    emit_u16
    mov     edi, [rsp + 48]
    call    emit_store_local

    call    alloc_anon_local
    mov     [rsp + 44], eax         ; bound_disp (`len`)
    mov     eax, 0x08408B49         ; mov rax, [r8 + 8]
    call    emit_u32
    mov     edi, [rsp + 44]
    call    emit_store_local

    mov     edi, r15d
    and     edi, TY_BASE_MASK
    mov     [rsp + 56], edi         ; elem_ty
    call    type_size_align
    mov     [rsp + 60], eax         ; elem_size

.span_loop_common:
    ; Parse optional `<rev | stride: S | prefetch: D>` lenses (`edi = 1` -> allow prefetch, no unroll)
    mov     edi, 1
    call    .parse_opt_for_lenses

    ; Bind `idx` (named if `var2_sym != 0`, else anonymous) & `elem`!
    cmp     dword [rsp + 16], 0
    je      .sp_one_var
    mov     edi, [rsp + 8]          ; idx = var1
    mov     esi, TY_U64
    mov     edx, [rsp + 12]
    call    alloc_local_scoped
    mov     [rsp + 40], eax         ; idx_disp

    mov     edi, [rsp + 16]         ; elem = var2
    mov     esi, [rsp + 56]
    mov     edx, [rsp + 12]
    call    alloc_local_scoped
    mov     [rsp + 52], eax         ; elem_disp
    jmp     .sp_init_idx

.sp_one_var:
    call    alloc_anon_local
    mov     [rsp + 40], eax         ; anonymous idx_disp
    mov     edi, [rsp + 8]          ; elem = var1
    mov     esi, [rsp + 56]
    mov     edx, [rsp + 12]
    call    alloc_local_scoped
    mov     [rsp + 52], eax         ; elem_disp

.sp_init_idx:
    call    block_has_trailing_recover
    mov     [rsp], eax
    test    eax, eax
    jz      .sp_setup
    call    rec_push_scope
.sp_setup:
    test    dword [rsp + 20], FLENS_TILE
    jz      .sp_no_tile
    mov     esi, [rsp + 12]
    call    emit_tile_enter
.sp_no_tile:
    call    loop_push_scope

    test    dword [rsp + 20], FLENS_REV
    jnz     .sp_rev_loop

    ; Forward span loop: `idx = 0; while (u64)idx < len { elem = base[idx]; body; idx += stride }`
    xor     eax, eax
    call    emit_mov_rax_imm64
    mov     edi, [rsp + 40]
    call    emit_store_local

    mov     r14d, [code_off]        ; loop_top
    mov     edi, [rsp + 40]
    call    emit_load_local
    mov     edi, [rsp + 44]
    call    emit_load_local_rcx     ; rcx = len
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC839              ; cmp rax, rcx
    call    emit_u16
    mov     ax, 0x830F              ; jae .exit (unsigned!)
    call    emit_u16
    mov     r15d, [code_off]
    xor     eax, eax
    call    emit_u32

    call    .emit_load_span_elem_into_slot
    call    parse_block_no_scope_push

    call    loop_patch_continues_here
    mov     edi, [rsp + 40]
    call    emit_load_local
    mov     rax, [rsp + 24]         ; stride
    call    emit_mov_rcx_imm64
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC801              ; add rax, rcx
    call    emit_u16
    mov     edi, [rsp + 40]
    call    emit_store_local

    mov     edi, r14d
    call    emit_jmp_to
    mov     edi, r15d
    call    patch_rel32_here
    call    loop_pop_and_patch_breaks_here
    jmp     .rewind_for_scope

.sp_rev_loop:
    ; Reverse span loop (`len == 0` skips cleanly without unsigned underflow!):
    ; uses hidden `rem` counter (`bound_disp`) = `len`; while `rem > 0`, `idx = rem - 1`...
    mov     r14d, [code_off]        ; loop_top
    mov     edi, [rsp + 44]
    call    emit_load_local         ; rax = rem (`len` initially)
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC085              ; test rax, rax
    call    emit_u16
    mov     ax, 0x840F              ; jz .exit
    call    emit_u16
    mov     r15d, [code_off]
    xor     eax, eax
    call    emit_u32

    ; `idx = rem - 1`
    mov     eax, 0x01E88348         ; sub rax, 1
    call    emit_u32
    mov     edi, [rsp + 40]
    call    emit_store_local

    call    .emit_load_span_elem_into_slot
    call    parse_block_no_scope_push

    call    loop_patch_continues_here
    mov     edi, [rsp + 44]
    call    emit_load_local         ; rax = rem
    mov     rax, [rsp + 24]         ; stride
    call    emit_mov_rcx_imm64
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC839              ; cmp rax, rcx (rem vs stride)
    call    emit_u16
    mov     ax, 0x0577              ; ja .sub_s (5 bytes)
    call    emit_u16
    xor     eax, eax                ; mov eax, 0 -> rem = 0
    mov     al, 0x31
    call    emit_u8
    mov     al, 0xC0
    call    emit_u8
    mov     ax, 0x03EB              ; jmp .save_rem
    call    emit_u16
    ; .sub_s: `sub rax, rcx` (`48 29 C8` = 3 bytes!)
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC829
    call    emit_u16
    ; .save_rem:
    mov     edi, [rsp + 44]
    call    emit_store_local

    mov     edi, r14d
    call    emit_jmp_to
    mov     edi, r15d
    call    patch_rel32_here
    call    loop_pop_and_patch_breaks_here
    jmp     .rewind_for_scope

; -----------------------------------------------------------------------------
; Range Iteration (`for i in lo..hi [<rev | stride: S | unroll: K>]`)
; -----------------------------------------------------------------------------
.range_for:
    inc     dword [tok_idx]         ; consume `..`

    ; Bind `i` (`idx_disp`) and save `lo` temporarily in `base_disp`
    mov     edi, [rsp + 8]
    mov     esi, TY_I64
    mov     edx, [rsp + 12]
    call    alloc_local_scoped
    mov     [rsp + 40], eax         ; idx_disp (`i`)
    mov     edi, eax
    call    emit_store_local        ; store `lo` in `i`

    call    alloc_anon_local
    mov     [rsp + 48], eax         ; save `lo` copy for `<rev>`!
    mov     edi, [rsp + 40]
    call    emit_load_local
    mov     edi, [rsp + 48]
    call    emit_store_local

    mov     edi, 1
    call    parse_expr              ; `hi` -> `rax`
    call    alloc_anon_local
    mov     [rsp + 44], eax         ; bound_disp (`hi`)
    mov     edi, eax
    call    emit_store_local

    xor     edi, edi                ; `edi = 0` -> range lenses (`rev`, `stride`, `unroll`)
    call    .parse_opt_for_lenses

    call    block_has_trailing_recover
    mov     [rsp], eax
    test    eax, eax
    jz      .rg_go
    call    rec_push_scope
.rg_go:
    test    dword [rsp + 20], FLENS_TILE
    jz      .rg_no_tile
    mov     esi, [rsp + 12]
    call    emit_tile_enter
.rg_no_tile:
    call    loop_push_scope

    test    dword [rsp + 20], FLENS_REV
    jnz     .rg_rev

    cmp     dword [rsp + 32], 1
    ja      .rg_unrolled

    ; Standard / Strided forward `for i in lo..hi`:
    mov     r14d, [code_off]
    mov     edi, [rsp + 40]
    call    emit_load_local         ; rax = i
    mov     edi, [rsp + 44]
    call    emit_load_local_rcx     ; rcx = hi
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC839              ; cmp rax, rcx
    call    emit_u16
    mov     ax, 0x8D0F              ; jge .exit (signed, matching Phase-0 `0..n`)
    call    emit_u16
    mov     r15d, [code_off]
    xor     eax, eax
    call    emit_u32

    call    parse_block_no_scope_push

    call    loop_patch_continues_here
    mov     edi, [rsp + 40]
    call    emit_load_local
    mov     rax, [rsp + 24]         ; stride
    call    emit_mov_rcx_imm64
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC801              ; add rax, rcx
    call    emit_u16
    mov     edi, [rsp + 40]
    call    emit_store_local

    mov     edi, r14d
    call    emit_jmp_to
    mov     edi, r15d
    call    patch_rel32_here
    call    loop_pop_and_patch_breaks_here
    jmp     .rewind_for_scope

.rg_rev:
    ; Reverse `for i in lo..hi <rev [| stride: S]>`:
    ; If `hi <= lo`, skip immediately; else `i = hi - 1`, loop while `i >= lo`
    ; carefully avoiding signed/unsigned wrap when `lo == 0`!
    mov     edi, [rsp + 44]
    call    emit_load_local         ; rax = hi
    mov     edi, [rsp + 48]
    call    emit_load_local_rcx     ; rcx = lo
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC839              ; cmp rax, rcx (hi vs lo)
    call    emit_u16
    mov     ax, 0x8E0F              ; jle .exit_rev
    call    emit_u16
    mov     r15d, [code_off]
    xor     eax, eax
    call    emit_u32

    ; `i = hi - 1`
    mov     eax, 0x01E88348         ; sub rax, 1
    call    emit_u32
    mov     edi, [rsp + 40]
    call    emit_store_local

    mov     r14d, [code_off]        ; rev_body_top
    call    parse_block_no_scope_push

    call    loop_patch_continues_here
    ; Check if `i - lo < stride` -> if so, exit cleanly before subtracting!
    mov     edi, [rsp + 40]
    call    emit_load_local         ; rax = i
    mov     edi, [rsp + 48]
    call    emit_load_local_rcx     ; rcx = lo
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC829              ; sub rax, rcx (rem = i - lo >= 0)
    call    emit_u16
    mov     rax, [rsp + 24]         ; stride
    call    emit_mov_rcx_imm64
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC839              ; cmp rax, rcx (rem vs stride)
    call    emit_u16
    mov     ax, 0x820F              ; jb .exit_rev2
    call    emit_u16
    mov     ebx, [code_off]
    xor     eax, eax
    call    emit_u32

    ; `i -= stride` and jump to `rev_body_top`
    mov     edi, [rsp + 40]
    call    emit_load_local
    mov     rax, [rsp + 24]
    call    emit_mov_rcx_imm64
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC829              ; sub rax, rcx
    call    emit_u16
    mov     edi, [rsp + 40]
    call    emit_store_local

    mov     edi, r14d
    call    emit_jmp_to

    mov     edi, r15d
    call    patch_rel32_here
    mov     edi, ebx
    call    patch_rel32_here
    call    loop_pop_and_patch_breaks_here
    jmp     .rewind_for_scope

.rg_unrolled:
    ; Token-Replay Hardware Loop Unrolling (`<unroll: K>`, `K` in {2, 4, 8})!
    ; 1. Record `body_tok_start = [tok_idx]`
    mov     eax, [tok_idx]
    mov     [rsp + 64], eax

    ; Hidden base induction `cur` (`base_disp`) initialized to `lo` (`[rsp + 48]`)
    mov     r14d, [code_off]        ; fast_loop_top
    ; Check `cur + K * stride <= hi` -> i.e. `hi - cur >= K * stride` (and `cur < hi`)
    mov     edi, [rsp + 44]
    call    emit_load_local         ; rax = hi
    mov     edi, [rsp + 48]
    call    emit_load_local_rcx     ; rcx = cur
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC829              ; sub rax, rcx (hi - cur)
    call    emit_u16

    mov     eax, [rsp + 32]         ; K
    dec     eax                     ; (K - 1)
    imul    rax, [rsp + 24]         ; (K - 1) * stride
    inc     rax                     ; last unrolled step `cur + (K - 1)*stride < hi` <=> `hi - cur >= (K - 1)*stride + 1`
    call    emit_mov_rcx_imm64
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC839              ; cmp rax, rcx
    call    emit_u16
    mov     ax, 0x8C0F              ; jl .tail_loop (signed!)
    call    emit_u16
    mov     r15d, [code_off]
    xor     eax, eax
    call    emit_u32

    ; Emit `K` back-to-back copies of `{ ... }`!
    xor     ebx, ebx
.unr_copy_lp:
    cmp     ebx, [rsp + 32]
    jae     .unr_copies_done

    ; `i = cur + ebx * stride`
    mov     edi, [rsp + 48]
    call    emit_load_local
    test    ebx, ebx
    jz      .unr_set_i
    mov     eax, ebx
    imul    rax, [rsp + 24]
    call    emit_mov_rcx_imm64
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC801              ; add rax, rcx
    call    emit_u16
.unr_set_i:
    mov     edi, [rsp + 40]
    call    emit_store_local

    mov     eax, [rsp + 64]
    mov     [tok_idx], eax          ; rewind token cursor to `{`!
    mov     r12d, [local_cnt]       ; snapshot header local_cnt
    call    parse_block_no_scope_push
    mov     [local_cnt], r12d       ; rewind body locals before next unrolled copy!
    mov     eax, [tok_idx]
    mov     [rsp + 68], eax         ; save `body_tok_after`

    call    loop_patch_continues_here ; `continue` jumps to next unrolled step!
    inc     ebx
    jmp     .unr_copy_lp

.unr_copies_done:
    ; `cur += K * stride`
    mov     edi, [rsp + 48]
    call    emit_load_local
    mov     eax, [rsp + 32]
    imul    rax, [rsp + 24]
    call    emit_mov_rcx_imm64
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC801
    call    emit_u16
    mov     edi, [rsp + 48]
    call    emit_store_local

    mov     edi, r14d
    call    emit_jmp_to

    ; .tail_loop: scalar cleanup for remaining `< K` iterations!
    mov     edi, r15d
    call    patch_rel32_here

    mov     edi, [rsp + 48]
    call    emit_load_local
    mov     edi, [rsp + 40]
    call    emit_store_local        ; i = cur

    mov     r14d, [code_off]        ; tail_top
    mov     edi, [rsp + 40]
    call    emit_load_local
    mov     edi, [rsp + 44]
    call    emit_load_local_rcx
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC839
    call    emit_u16
    mov     ax, 0x8D0F              ; jge .done_unroll
    call    emit_u16
    mov     r15d, [code_off]
    xor     eax, eax
    call    emit_u32

    mov     eax, [rsp + 64]
    mov     [tok_idx], eax
    call    parse_block_no_scope_push

    call    loop_patch_continues_here
    mov     edi, [rsp + 40]
    call    emit_load_local
    mov     rax, [rsp + 24]
    call    emit_mov_rcx_imm64
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC801
    call    emit_u16
    mov     edi, [rsp + 40]
    call    emit_store_local

    mov     edi, r14d
    call    emit_jmp_to

    mov     edi, r15d
    call    patch_rel32_here
    mov     eax, [rsp + 68]
    mov     [tok_idx], eax
    call    loop_pop_and_patch_breaks_here

.rewind_for_scope:
    test    dword [rsp + 20], FLENS_TILE
    jz      .no_tile_exit
    call    emit_tile_exit
.no_tile_exit:
    dec     dword [scope_depth]
    mov     eax, [rsp + 4]
    mov     [local_cnt], eax

    cmp     dword [rsp], 0
    je      .done
    call    parse_trailing_recover_body

.done:
    add     rsp, 96
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbx
    ret

.reject_lens:
    lea     rdi, [msg_unimpl_lens]
    mov     edx, msg_unimpl_lens_len
    jmp     err_msg_at_line

; --- Internal Helpers for `parse_stmt_for_ext` (Note: `rsp` is shifted by +8 inside `call`!) ---

; Loads `base[idx]` (with optional `prefetcht0 [addr + D]`) into `elem_disp`
.emit_load_span_elem_into_slot:
    mov     edi, [rsp + 40 + 8]     ; idx_disp
    call    emit_load_local
    mov     edx, [rsp + 60 + 8]     ; elem_size
    cmp     edx, 1
    jbe     .els_add_b
    mov     eax, edx
    call    emit_mov_rcx_imm64
    mov     eax, 0xC1AF0F48         ; imul rax, rcx
    call    emit_u32
.els_add_b:
    mov     edi, [rsp + 48 + 8]     ; base_disp
    call    emit_load_local_rcx
    mov     al, 0x48
    call    emit_u8
    mov     ax, 0xC801              ; add rax, rcx -> `rax` = &base[idx]
    call    emit_u16

    ; Optional hardware `prefetcht0 [rax + disp32]` (`0F 18 88 <disp32>`, `/1`)
    test    dword [rsp + 20 + 8], FLENS_PREFETCH
    jz      .els_load
    mov     ax, 0x180F
    call    emit_u16
    mov     al, 0x88
    call    emit_u8
    mov     eax, [rsp + 36 + 8]
    call    emit_u32

.els_load:
    mov     edx, [rsp + 56 + 8]     ; elem_ty
    cmp     edx, TY_STRUCT_BASE
    jae     .els_copy_struct
    mov     edi, edx
    call    emit_load_mem_typed
    mov     edi, [rsp + 52 + 8]     ; elem_disp
    call    emit_lea_local_rcx
    mov     edi, [rsp + 56 + 8]
    jmp     emit_store_mem_typed
.els_copy_struct:
    mov     edi, [rsp + 52 + 8]
    call    emit_lea_local_rcx
    mov     edi, [rsp + 56 + 8]     ; reload `elem_ty` (emit_* clobbers rdx!)
    jmp     emit_store_mem_typed

; Parses optional `<clause [| clause]*>` on range (`edi = 0`) or span (`edi = 1`) loops
.parse_opt_for_lenses:
    mov     r12d, edi               ; 0 = range, 1 = span
    call    peek_tok_ptr
    cmp     byte [rax], TOK_LT
    jne     .pfl_ret
    mov     r13d, [rax + 4]         ; `<` line for `.reject_lens`
    inc     dword [tok_idx]         ; consume `<`

.pfl_loop:
    call    next_tok_ptr
    mov     esi, [rax + 4]
    cmp     byte [rax], TOK_IDENT
    jne     .reject_lens
    mov     ecx, [rax + 8]

    cmp     ecx, [sym_rev_id]
    je      .pfl_rev
    cmp     ecx, [sym_stride_id]
    je      .pfl_stride
    cmp     ecx, [sym_unroll_id]
    je      .pfl_unroll
    cmp     ecx, [sym_prefetch_id]
    je      .pfl_prefetch
    cmp     ecx, [sym_tile_id]
    je      .pfl_tile
    jmp     .reject_lens

.pfl_tile:
    call    peek_tok_ptr
    cmp     byte [rax], TOK_COLON
    je      .pfl_bad                ; `<tile: 32KB>` cache-blocking stays rejected until Phase 4
    or      dword [rsp + 20 + 8], FLENS_TILE
    jmp     .pfl_sep

.pfl_rev:
    cmp     dword [rsp + 32 + 8], 1 ; disallow `<rev | unroll: K>`
    ja      .reject_lens
    or      dword [rsp + 20 + 8], FLENS_REV
    jmp     .pfl_sep

.pfl_stride:
    mov     al, TOK_COLON
    call    expect_tok_kind
    call    parse_const_lens_expr
    cmp     rax, 1
    jb      .pfl_bad
    mov     [rsp + 24 + 8], rax
    jmp     .pfl_sep

.pfl_unroll:
    test    r12d, r12d              ; range-only lens
    jnz     .pfl_bad
    test    dword [rsp + 20 + 8], FLENS_REV
    jnz     .pfl_bad
    mov     al, TOK_COLON
    call    expect_tok_kind
    call    parse_const_lens_expr
    cmp     rax, 2
    je      .unr_ok
    cmp     rax, 4
    je      .unr_ok
    cmp     rax, 8
    jne     .pfl_bad
.unr_ok:
    mov     [rsp + 32 + 8], eax
    jmp     .pfl_sep

.pfl_prefetch:
    test    r12d, r12d              ; span/slab lens
    jz      .pfl_bad
    mov     al, TOK_COLON
    call    expect_tok_kind
    call    parse_const_lens_expr
    cmp     rax, 0x7FFFFFFF
    ja      .pfl_bad
    mov     [rsp + 36 + 8], eax
    or      dword [rsp + 20 + 8], FLENS_PREFETCH
    jmp     .pfl_sep

.pfl_sep:
    call    peek_tok_ptr
    cmp     byte [rax], TOK_PIPE
    jne     .pfl_close
    inc     dword [tok_idx]
    jmp     .pfl_loop

.pfl_close:
    call    peek_tok_ptr
    mov     esi, [rax + 4]
    cmp     byte [rax], TOK_GT
    jne     .reject_lens
    inc     dword [tok_idx]
.pfl_ret:
    ret

.pfl_bad:
    mov     esi, r13d
    jmp     .reject_lens
