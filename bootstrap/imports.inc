; =============================================================================
; Zero Stage-0 Bootstrap — Multi-File Module Loader (`bootstrap/imports.inc`)
; =============================================================================
; Implements `use "rel/path.zr"` top-level module imports:
;   - Resolves paths relative to the importing `.zr` file's directory
;   - Deduplicates canonicalized paths via hardware `crc32` (`sym_intern`)
;     so diamond imports (`A -> B, C` and `B, C -> D`) include `D` once
;   - Preserves per-file line numbers and order-independent top-level decls
; =============================================================================

expand_module_imports:
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r14
    push    r15

    ; Fast scan: if root token stream has no `use` identifier, return immediately
    mov     rsi, [space_tok]
    mov     ecx, [tok_count]
    mov     edx, [sym_use_id]
    xor     eax, eax
.fast_chk:
    cmp     eax, ecx
    jae     .no_imports
    mov     edi, eax
    shl     rdi, 4
    cmp     byte [rsi + rdi], TOK_IDENT
    jne     .fast_next
    cmp     [rsi + rdi + 8], rdx
    je      .has_imports
.fast_next:
    inc     eax
    jmp     .fast_chk

.has_imports:
    mov     dword [import_mod_cnt], 0
    mov     dword [import_final_cnt], 0
    mov     qword [import_scratch_off], 0

    mov     rax, [src_len]
    add     rax, 64
    and     rax, ~15
    mov     [import_src_off], rax

    ; Canonicalize & register root `[in_path]` into permanent `space_src` tail
    mov     rsi, [in_path]
    mov     rdi, [space_src]
    add     rdi, [import_src_off]
    mov     r14, rdi                ; r14 = permanent canonical root path cstr
    call    canonicalize_rel_path
    mov     r13d, eax               ; r13d = canonical root path len
    lea     rax, [r13 + 16]
    and     rax, ~7
    add     [import_src_off], rax

    mov     rdi, r14
    mov     esi, r13d
    call    sym_intern
    mov     rcx, [space_ast]
    mov     [rcx + AST_IMPORT_META_OFF], eax
    mov     dword [import_mod_cnt], 1

    ; Copy root tokens (excluding trailing `TOK_EOF`) into `AST_IMPORT_SCRATCH`
    mov     ecx, [tok_count]
    test    ecx, ecx
    jz      .no_imports
    dec     ecx                     ; exclude TOK_EOF
    mov     r15d, ecx               ; r15d = root_tok_cnt

    mov     rdi, [space_ast]
    add     rdi, AST_IMPORT_SCRATCH
    mov     r12, rdi                ; r12 = root scratch slice
    mov     rsi, [space_tok]
    mov     ecx, r15d
    shl     rcx, 1                  ; 2 qwords (16B) per token
    rep     movsq

    mov     eax, r15d
    shl     rax, 4
    mov     [import_scratch_off], rax

    ; Recursively expand `use "..."` starting from root module
    mov     rdi, r14                ; cur_file_path_cstr
    mov     rsi, r12                ; tok_slice_ptr
    mov     edx, r15d               ; tok_cnt
    call    process_module_tokens

    ; Copy expanded token stream + `TOK_EOF` back into `space_tok`
    mov     ecx, [import_final_cnt]
    mov     rsi, [space_ast]
    add     rsi, AST_IMPORT_FINAL
    mov     rdi, [space_tok]
    mov     eax, ecx
    shl     rcx, 1                  ; qwords
    rep     movsq

    ; Append clean 16-byte `TOK_EOF`
    xor     edx, edx
    mov     [rdi],     rdx
    mov     [rdi + 8], rdx
    inc     eax
    mov     [tok_count], eax

.no_imports:
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbp
    pop     rbx
    ret

; -----------------------------------------------------------------------------
; process_module_tokens(rdi = cur_path_cstr, rsi = tok_slice, edx = tok_cnt)
; -----------------------------------------------------------------------------
process_module_tokens:
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r14
    push    r15
    sub     rsp, 544                ; [rsp + 0..511] = path scratch

    mov     r12, rdi                ; r12 = cur_path_cstr
    mov     r13, rsi                ; r13 = tok_slice
    mov     r14d, edx               ; r14d = tok_cnt
    xor     ebx, ebx                ; ebx = token index `i`

.tok_loop:
    cmp     ebx, r14d
    jae     .mod_done

    mov     eax, ebx
    shl     rax, 4
    lea     r15, [r13 + rax]        ; r15 = &tok[i]

    cmp     byte [r15], TOK_IDENT
    jne     .copy_normal
    mov     eax, [sym_use_id]
    cmp     [r15 + 8], rax
    jne     .copy_normal

    ; Match `use "path.zr"` (`TOK_IDENT(use)` followed by `TOK_STR`)
    lea     eax, [ebx + 1]
    cmp     eax, r14d
    jae     .copy_normal
    cmp     byte [r15 + 16], TOK_STR
    jne     .copy_normal

    movzx   ecx, word [r15 + 16 + 2] ; sub_len
    test    ecx, ecx
    jz      .err_use_syn
    mov     rsi, [space_rodata]
    add     rsi, [r15 + 16 + 8]     ; sub_ptr

    ; Strip leading `./` on relative import string
    cmp     ecx, 2
    jb      .chk_abs
    cmp     byte [rsi], '.'
    jne     .chk_abs
    cmp     byte [rsi + 1], '/'
    jne     .chk_abs
    add     rsi, 2
    sub     ecx, 2

.chk_abs:
    lea     rdi, [rsp]              ; build resolved path in `[rsp]`
    cmp     byte [rsi], '/'
    je      .copy_sub

    ; Find last `/` in `r12` (`cur_path_cstr`)
    xor     edx, edx                ; len
    mov     r8d, -1                 ; last_slash_idx
.find_sl:
    movzx   eax, byte [r12 + rdx]
    test    al, al
    jz      .got_sl
    cmp     al, '/'
    jne     .nx_sl
    mov     r8d, edx
.nx_sl:
    inc     edx
    jmp     .find_sl
.got_sl:
    cmp     r8d, -1
    je      .copy_sub
    push    rsi
    push    rcx
    mov     rsi, r12
    lea     ecx, [r8d + 1]          ; include trailing `/`
    rep     movsb
    pop     rcx
    pop     rsi

.copy_sub:
    rep     movsb
    mov     byte [rdi], 0

    ; Canonicalize `/./` and `<seg>/..` in `[rsp]` so `sub/../x.zr` == `x.zr`!
    lea     rsi, [rsp]
    lea     rdi, [rsp + 256]
    call    canonicalize_rel_path
    lea     rsi, [rsp + 256]
.comb_len:
    mov     rdx, rax                ; rdx = canonical byte length
    mov     [rsp + 520], rdx

    ; Copy permanent path into `space_src` tail for `sym_intern` & `SYS_OPEN`
    mov     rdi, [space_src]
    add     rdi, [import_src_off]
    mov     rbp, rdi                ; rbp = permanent path cstr
    mov     rcx, rdx
    rep     movsb
    mov     byte [rdi], 0
    mov     rax, [rsp + 520]
    add     rax, 16
    and     rax, ~7
    add     [import_src_off], rax

    ; Deduplicate via `sym_intern` against `AST_IMPORT_META_OFF`
    mov     rdi, rbp
    mov     esi, [rsp + 520]
    call    sym_intern              ; eax = path_sym_id

    mov     rcx, [space_ast]
    add     rcx, AST_IMPORT_META_OFF
    mov     edx, [import_mod_cnt]
    xor     r8d, r8d
.dedup_lp:
    cmp     r8d, edx
    jae     .register_mod
    cmp     [rcx + r8 * 4], eax
    je      .skip_already_imported
    inc     r8d
    jmp     .dedup_lp

.register_mod:
    cmp     edx, 256
    jae     err_open_in
    mov     [rcx + rdx * 4], eax
    inc     dword [import_mod_cnt]

    ; Read imported `.zr` file into `space_src + [import_src_off]`
    mov     edi, AT_FDCWD
    mov     rsi, rbp
    xor     edx, edx                ; O_RDONLY
    xor     r10d, r10d
    mov     eax, SYS_OPENAT
    syscall
    test    rax, rax
    js      err_open_in
    mov     [rsp + 528], rax        ; fd

    mov     rdi, rax
    xor     esi, esi
    mov     edx, SEEK_END
    mov     eax, SYS_LSEEK
    syscall
    test    rax, rax
    js      err_open_in
    mov     [rsp + 536], rax        ; file_len

    mov     rdi, [rsp + 528]
    xor     esi, esi
    xor     edx, edx
    mov     eax, SYS_LSEEK
    syscall

    mov     rsi, [space_src]
    add     rsi, [import_src_off]
    mov     [rsp + 512], rsi        ; file_buf
    mov     rdi, [rsp + 528]
    mov     rdx, [rsp + 536]
    mov     eax, SYS_READ
    syscall
    cmp     rax, [rsp + 536]
    jne     err_open_in

    mov     rdi, [rsp + 528]
    mov     eax, SYS_CLOSE
    syscall

    ; Zero-pad file tail & advance `import_src_off`
    mov     rdi, [rsp + 512]
    mov     rcx, [rsp + 536]
    mov     qword [rdi + rcx], 0
    lea     rax, [rcx + 64]
    and     rax, ~15
    add     [import_src_off], rax

    ; Tokenize imported module with fresh `cur_line = 1`
    mov     dword [cur_line], 1
    mov     dword [tok_count], 0
    mov     rdi, [rsp + 512]
    mov     rsi, [rsp + 536]
    call    lexer_tokenize_range

    mov     edx, [tok_count]
    test    edx, edx
    jz      .skip_already_imported
    dec     edx                     ; exclude trailing TOK_EOF
    mov     [rsp + 520], edx        ; sub_tok_cnt

    mov     rdi, [space_ast]
    add     rdi, AST_IMPORT_SCRATCH
    add     rdi, [import_scratch_off]
    mov     [rsp + 528], rdi        ; sub_tok_slice
    mov     rsi, [space_tok]
    mov     ecx, edx
    shl     rcx, 1
    rep     movsq

    mov     eax, [rsp + 520]
    shl     rax, 4
    add     [import_scratch_off], rax

    ; Recursively expand nested imports inside this module
    mov     rdi, rbp
    mov     rsi, [rsp + 528]
    mov     edx, [rsp + 520]
    call    process_module_tokens

.skip_already_imported:
    add     ebx, 2                  ; consume `use` + `"..."`
    jmp     .tok_loop

.copy_normal:
    mov     eax, [import_final_cnt]
    inc     dword [import_final_cnt]
    shl     rax, 4
    mov     rdi, [space_ast]
    add     rdi, AST_IMPORT_FINAL
    add     rdi, rax
    mov     rcx, [r15]
    mov     rdx, [r15 + 8]
    mov     [rdi],     rcx
    mov     [rdi + 8], rdx
    inc     ebx
    jmp     .tok_loop

.err_use_syn:
    mov     esi, [r15 + 4]
    lea     rdi, [msg_syn_line]
    mov     edx, msg_syn_line_len
    jmp     err_msg_at_line

.mod_done:
    add     rsp, 544
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbp
    pop     rbx
    ret

; -----------------------------------------------------------------------------
; canonicalize_rel_path(rsi = in_cstr, rdi = out_buf) -> rax = out_len
; Collapses `/./`, leading `./`, duplicate `//`, and `<seg>/..` segments.
; -----------------------------------------------------------------------------
canonicalize_rel_path:
    push    rbx
    push    r12
    mov     r12, rdi                ; r12 = out_base
    cmp     byte [rsi], '/'
    jne     .seg_loop
    mov     byte [rdi], '/'
    inc     rdi
    inc     rsi
.seg_loop:
    cmp     byte [rsi], '/'
    jne     .chk_end
    inc     rsi
    jmp     .seg_loop
.chk_end:
    cmp     byte [rsi], 0
    je      .finish
    ; Measure segment `[rsi .. rsi + rcx)`
    xor     ecx, ecx
.meas:
    movzx   eax, byte [rsi + rcx]
    test    al, al
    jz      .got_seg
    cmp     al, '/'
    je      .got_seg
    inc     ecx
    jmp     .meas
.got_seg:
    ; Skip `.` segment
    cmp     ecx, 1
    jne     .chk_dot2
    cmp     byte [rsi], '.'
    jne     .emit_seg
    add     rsi, 1
    jmp     .seg_loop
.chk_dot2:
    cmp     ecx, 2
    jne     .emit_seg
    cmp     word [rsi], 0x2E2E      ; ".."
    jne     .emit_seg
    ; Pop previous non-`..` segment from `rdi` if one exists
    cmp     rdi, r12
    je      .emit_seg
    lea     rbx, [rdi - 1]
    cmp     rbx, r12
    je      .emit_seg
.pop_sl:
    cmp     rbx, r12
    je      .chk_prev_not_dot2
    cmp     byte [rbx - 1], '/'
    je      .chk_prev_not_dot2
    dec     rbx
    jmp     .pop_sl
.chk_prev_not_dot2:
    mov     rax, rdi
    sub     rax, rbx
    cmp     rax, 2
    jne     .do_pop
    cmp     word [rbx], 0x2E2E
    je      .emit_seg               ; keep `../..` if already at root
.do_pop:
    mov     rdi, rbx
    cmp     rdi, r12
    je      .pop_done
    lea     rax, [r12 + 1]
    cmp     rdi, rax
    jne     .trim_sl
    cmp     byte [r12], '/'
    je      .pop_done
.trim_sl:
    dec     rdi
.pop_done:
    add     rsi, 2
    jmp     .seg_loop
.emit_seg:
    cmp     rdi, r12
    je      .copy_s
    cmp     byte [rdi - 1], '/'
    je      .copy_s
    mov     byte [rdi], '/'
    inc     rdi
.copy_s:
    rep     movsb
    jmp     .seg_loop
.finish:
    mov     byte [rdi], 0
    mov     rax, rdi
    sub     rax, r12
    pop     r12
    pop     rbx
    ret
