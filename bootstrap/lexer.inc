; =============================================================================
; Zero Stage-0 Bootstrap — Hardware CRC32C Lexer (`bootstrap/lexer.inc`)
; =============================================================================
; Uses Intel SSE4.2 / Willow Cove hardware `crc32` instructions to hash and
; intern all identifiers & keywords into a 65,536-slot open-addressed table,
; emitting a flat array of 16-byte `Token` structs into `space_tok`.
; =============================================================================

; -----------------------------------------------------------------------------
; sym_intern(rdi = byte_ptr, esi = byte_len) -> eax = canonical sym_id (1..N)
; Preserves: rbx, rbp, r12, r13, r14, r15
; -----------------------------------------------------------------------------
sym_intern:
    push    rbx
    push    r12
    push    r13

    mov     r8,  rdi                ; r8  = str_ptr
    mov     r9d, esi                ; r9d = str_len

    ; --- Hardware Castagnoli CRC32C (`crc32q` + `crc32b`) ---
    mov     eax, 0xFFFFFFFF
    mov     rcx, rdi
    mov     edx, esi
.crc_qword:
    cmp     edx, 8
    jb      .crc_byte
    crc32   rax, qword [rcx]
    add     rcx, 8
    sub     edx, 8
    jmp     .crc_qword
.crc_byte:
    test    edx, edx
    jz      .crc_done
    crc32   eax, byte [rcx]
    inc     rcx
    dec     edx
    jmp     .crc_byte
.crc_done:
    not     eax
    test    eax, eax
    jnz     .hash_nonzero
    mov     eax, 1                  ; 0 is reserved for empty hash slot
.hash_nonzero:
    mov     r10d, eax               ; r10d = 32-bit hardware CRC32C hash
    mov     r11d, eax
    and     r11d, SYM_HASH_MASK     ; r11d = slot index (0..65535)
    mov     r12,  [space_sym]       ; r12  = symbol space base

.probe_loop:
    mov     ebx, r11d
    shl     ebx, 4                  ; slot * 16
    mov     edx, [r12 + rbx]        ; stored_hash
    test    edx, edx
    jz      .insert_new

    cmp     edx, r10d
    jne     .next_slot
    cmp     dword [r12 + rbx + 8], r9d  ; stored_len == str_len?
    jne     .next_slot

    ; Hash + length matched -> verify exact bytes against SymMeta[sym_id].ptr
    mov     r13d, [r12 + rbx + 4]   ; candidate sym_id
    mov     eax, r13d
    shl     eax, 4
    mov     rsi, [r12 + SYM_META_OFF + rax] ; candidate ptr
    mov     rdi, r8
    mov     ecx, r9d
    repe    cmpsb
    je      .found_existing

.next_slot:
    inc     r11d
    and     r11d, SYM_HASH_MASK
    jmp     .probe_loop

.insert_new:
    mov     eax, [sym_count]
    inc     eax
    mov     [sym_count], eax        ; new sym_id (1-based)

    ; Populate 16B Hash Slot: [hash:u32, sym_id:u32, len:u32, pad:u32]
    mov     [r12 + rbx],     r10d
    mov     [r12 + rbx + 4], eax
    mov     [r12 + rbx + 8], r9d

    ; Populate 16B SymMeta[sym_id]: [ptr:u64, len:u32, hash:u32]
    mov     edx, eax
    shl     edx, 4
    mov     [r12 + SYM_META_OFF + rdx],      r8
    mov     [r12 + SYM_META_OFF + rdx + 8],  r9d
    mov     [r12 + SYM_META_OFF + rdx + 12], r10d
    jmp     .done

.found_existing:
    mov     eax, r13d

.done:
    pop     r13
    pop     r12
    pop     rbx
    ret

; -----------------------------------------------------------------------------
; lexer_init — Pre-seeds all built-in `Zero` keywords into sym_id 1..KW_COUNT
; -----------------------------------------------------------------------------
lexer_init:
    push    rbx
    mov     dword [sym_count], 0
    lea     rbx, [kw_seed_table]
.seed_loop:
    movzx   esi, byte [rbx]
    test    esi, esi
    jz      .seed_done
    lea     rdi, [rbx + 1]
    call    sym_intern
    movzx   ecx, byte [rbx]
    lea     rbx, [rbx + rcx + 1]
    jmp     .seed_loop
.seed_done:
    pop     rbx
    ret

; -----------------------------------------------------------------------------
; emit_tok(al = kind, ah = subkind, ecx = len, rdx = payload)
; Appends 16-byte Token at `space_tok + tok_count * 16`
; -----------------------------------------------------------------------------
emit_tok:
    mov     r8d, [tok_count]
    shl     r8,  4
    add     r8,  [space_tok]
    mov     [r8],     ax            ; writes kind (al) at +0 and subkind (ah) at +1
    mov     [r8 + 2], cx
    mov     r9d, [cur_line]
    mov     [r8 + 4], r9d
    mov     [r8 + 8], rdx
    inc     dword [tok_count]
    ret

; -----------------------------------------------------------------------------
; lexer_tokenize — Converts `[space_src .. +src_len)` into 16B Token Stream
; -----------------------------------------------------------------------------
lexer_tokenize:
    mov     dword [cur_line], 1
    mov     dword [tok_count], 0
    mov     rdi, [space_src]
    mov     rsi, [src_len]
    ; fallthrough into lexer_tokenize_range

; -----------------------------------------------------------------------------
; lexer_tokenize_range(rdi = buf_ptr, rsi = buf_len)
; Appends tokens for `[rdi .. rdi + rsi)` + TOK_EOF starting at `[tok_count]`.
; Assumes `[rdi + rsi]` and `[rdi + rsi + 1]` are zero-padded.
; -----------------------------------------------------------------------------
lexer_tokenize_range:
    push    rbx
    push    rbp
    push    r12
    push    r14

    mov     rbp, rdi                ; rbp = current source byte ptr
    mov     rbx, rdi
    add     rbx, rsi                ; rbx = source end ptr (zero-padded!)
    mov     r14, [space_rodata]     ; r14 = string literal pool

.next_token:
    cmp     rbp, rbx
    jae     .emit_eof

    movzx   eax, byte [rbp]

    ; --- Fast whitespace skip ---
    cmp     al, ' '
    je      .skip_1
    cmp     al, 9                   ; '\t'
    je      .skip_1
    cmp     al, 13                  ; '\r'
    je      .skip_1
    cmp     al, 10                  ; '\n'
    jne     .check_comment
    inc     dword [cur_line]
.skip_1:
    inc     rbp
    jmp     .next_token

.check_comment:
    cmp     al, '/'
    jne     .check_ident
    movzx   ecx, byte [rbp + 1]
    cmp     cl, '/'
    je      .line_comment
    cmp     cl, '*'
    je      .block_comment
    jmp     .check_ops

.line_comment:
    add     rbp, 2
.lc_loop:
    cmp     rbp, rbx
    jae     .emit_eof
    cmp     byte [rbp], 10
    je      .next_token             ; let main loop count '\n'
    inc     rbp
    jmp     .lc_loop

.block_comment:
    add     rbp, 2
.bc_loop:
    cmp     rbp, rbx
    jae     lex_err_unclosed_comment
    movzx   eax, byte [rbp]
    cmp     al, 10
    jne     .bc_chk_end
    inc     dword [cur_line]
    inc     rbp
    jmp     .bc_loop
.bc_chk_end:
    cmp     al, '*'
    jne     .bc_adv
    cmp     byte [rbp + 1], '/'
    jne     .bc_adv
    add     rbp, 2
    jmp     .next_token
.bc_adv:
    inc     rbp
    jmp     .bc_loop

    ; --- Identifiers & Keywords: [A-Za-z_][A-Za-z0-9_]* ---
.check_ident:
    cmp     al, '_'
    je      .scan_ident
    mov     ecx, eax
    or      cl, 0x20
    cmp     cl, 'a'
    jb      .check_number
    cmp     cl, 'z'
    ja      .check_number

.scan_ident:
    mov     r12, rbp                ; r12 = ident start
    inc     rbp
.id_loop:
    cmp     rbp, rbx
    jae     .id_done
    movzx   eax, byte [rbp]
    cmp     al, '_'
    je      .id_adv
    cmp     al, '0'
    jb      .id_done
    cmp     al, '9'
    jbe     .id_adv
    or      al, 0x20
    cmp     al, 'a'
    jb      .id_done
    cmp     al, 'z'
    ja      .id_done
.id_adv:
    inc     rbp
    jmp     .id_loop
.id_done:
    mov     rdi, r12
    mov     rsi, rbp
    sub     rsi, r12                ; esi = identifier length
    push    rsi
    call    sym_intern              ; eax = canonical sym_id
    pop     rcx                     ; ecx = length
    mov     edx, eax                ; rdx = sym_id
    cmp     eax, KW_COUNT
    ja      .is_user_ident
    mov     ah, al                  ; subkind = KW_*
    mov     al, TOK_KW
    call    emit_tok
    jmp     .next_token
.is_user_ident:
    mov     ax, TOK_IDENT           ; al = TOK_IDENT, ah = 0
    call    emit_tok
    jmp     .next_token

    ; --- Numeric Literals: Decimal (+ B/KB/MB/GB), 0x Hex, 0b Binary ---
.check_number:
    cmp     al, '0'
    jb      .check_string
    cmp     al, '9'
    ja      .check_string

    mov     r12, rbp                ; r12 = literal start
    xor     rdx, rdx                ; rdx = accumulated u64 value
    cmp     al, '0'
    jne     .dec_loop
    movzx   ecx, byte [rbp + 1]
    or      cl, 0x20
    cmp     cl, 'x'
    je      .hex_start
    cmp     cl, 'b'
    je      .bin_start
    jmp     .dec_loop

.hex_start:
    add     rbp, 2
.hex_loop:
    movzx   eax, byte [rbp]
    cmp     al, '_'
    je      .hex_skip_us
    cmp     al, '0'
    jb      .num_emit
    cmp     al, '9'
    jbe     .hex_09
    mov     ecx, eax
    or      cl, 0x20
    cmp     cl, 'a'
    jb      .num_emit
    cmp     cl, 'f'
    ja      .num_emit
    sub     cl, 'a' - 10
    shl     rdx, 4
    movzx   ecx, cl
    or      rdx, rcx
    inc     rbp
    jmp     .hex_loop
.hex_09:
    sub     al, '0'
    shl     rdx, 4
    or      rdx, rax
    inc     rbp
    jmp     .hex_loop
.hex_skip_us:
    inc     rbp
    jmp     .hex_loop

.bin_start:
    add     rbp, 2
.bin_loop:
    movzx   eax, byte [rbp]
    cmp     al, '_'
    je      .bin_skip_us
    cmp     al, '0'
    jb      .num_emit
    cmp     al, '1'
    ja      .num_emit
    sub     al, '0'
    shl     rdx, 1
    or      rdx, rax
    inc     rbp
    jmp     .bin_loop
.bin_skip_us:
    inc     rbp
    jmp     .bin_loop

.dec_loop:
    movzx   eax, byte [rbp]
    cmp     al, '_'
    je      .dec_skip_us
    cmp     al, '0'
    jb      .dec_check_unit
    cmp     al, '9'
    ja      .dec_check_unit
    sub     al, '0'
    imul    rdx, rdx, 10
    add     rdx, rax
    inc     rbp
    jmp     .dec_loop
.dec_skip_us:
    inc     rbp
    jmp     .dec_loop

    ; Base-10 Byte-Unit Suffixes (`64B`, `4KB`, `32KB`, `16MB`, `4GB`)
.dec_check_unit:
    movzx   eax, byte [rbp]
    cmp     al, 'B'
    je      .unit_b
    cmp     al, 'K'
    je      .unit_kb
    cmp     al, 'M'
    je      .unit_mb
    cmp     al, 'G'
    je      .unit_gb
    jmp     .num_emit

.unit_b:
    inc     rbp
    jmp     .num_emit
.unit_kb:
    cmp     byte [rbp + 1], 'B'
    jne     .num_emit
    shl     rdx, 10
    add     rbp, 2
    jmp     .num_emit
.unit_mb:
    cmp     byte [rbp + 1], 'B'
    jne     .num_emit
    shl     rdx, 20
    add     rbp, 2
    jmp     .num_emit
.unit_gb:
    cmp     byte [rbp + 1], 'B'
    jne     .num_emit
    shl     rdx, 30
    add     rbp, 2

.num_emit:
    mov     rcx, rbp
    sub     rcx, r12                ; token byte length in source
    mov     ax, TOK_INT
    call    emit_tok
    jmp     .next_token

    ; --- String Literals: "..." ---
.check_string:
    cmp     al, '"'
    jne     .check_char
    inc     rbp                     ; skip opening '"'
    mov     edi, [rodata_off]       ; edi = start offset in space_rodata

.str_scan:
    cmp     rbp, rbx
    jae     lex_err_unclosed_str
    movzx   eax, byte [rbp]
    inc     rbp
    cmp     al, '"'
    je      .str_finish
    cmp     al, 10
    je      lex_err_unclosed_str
    cmp     al, '\'
    jne     .str_put

    cmp     rbp, rbx
    jae     lex_err_unclosed_str
    movzx   eax, byte [rbp]
    inc     rbp
    cmp     al, 'n'
    jne     .s_esc_t
    mov     al, 10
    jmp     .str_put
.s_esc_t:
    cmp     al, 't'
    jne     .s_esc_r
    mov     al, 9
    jmp     .str_put
.s_esc_r:
    cmp     al, 'r'
    jne     .s_esc_0
    mov     al, 13
    jmp     .str_put
.s_esc_0:
    cmp     al, '0'
    jne     .s_esc_x
    xor     eax, eax
    jmp     .str_put
.s_esc_x:
    cmp     al, 'x'
    jne     .str_put
    lea     rcx, [rbp + 2]
    cmp     rcx, rbx
    ja      lex_err_unclosed_str
    movzx   eax, byte [rbp]
    call    hex_nibble
    shl     eax, 4
    mov     ecx, eax
    movzx   eax, byte [rbp + 1]
    call    hex_nibble
    or      eax, ecx
    add     rbp, 2

.str_put:
    mov     ecx, [rodata_off]
    mov     [r14 + rcx], al
    inc     dword [rodata_off]
    jmp     .str_scan

.str_finish:
    mov     ecx, [rodata_off]
    sub     ecx, edi                ; ecx = decoded string byte length
    mov     edx, edi                ; rdx = offset in space_rodata
    mov     ax, TOK_STR
    call    emit_tok
    jmp     .next_token

    ; --- Character Literals: '...' ---
.check_char:
    cmp     al, 39                  ; '\''
    jne     .check_ops
    mov     r12, rbp
    inc     rbp
    cmp     rbp, rbx
    jae     lex_err_invalid
    movzx   edx, byte [rbp]
    inc     rbp
    cmp     dl, '\'
    jne     .char_close
    movzx   edx, byte [rbp]
    inc     rbp
    cmp     dl, 'n'
    jne     .c_esc_t
    mov     edx, 10
    jmp     .char_close
.c_esc_t:
    cmp     dl, 't'
    jne     .c_esc_r
    mov     edx, 9
    jmp     .char_close
.c_esc_r:
    cmp     dl, 'r'
    jne     .c_esc_0
    mov     edx, 13
    jmp     .char_close
.c_esc_0:
    cmp     dl, '0'
    jne     .c_esc_x
    xor     edx, edx
    jmp     .char_close
.c_esc_x:
    cmp     dl, 'x'
    jne     .char_close
    lea     rcx, [rbp + 2]
    cmp     rcx, rbx
    ja      lex_err_invalid
    movzx   eax, byte [rbp]
    call    hex_nibble
    shl     eax, 4
    mov     edx, eax
    movzx   eax, byte [rbp + 1]
    call    hex_nibble
    or      edx, eax
    add     rbp, 2
.char_close:
    cmp     byte [rbp], 39
    jne     lex_err_invalid
    inc     rbp
    mov     rcx, rbp
    sub     rcx, r12
    mov     ax, TOK_CHAR
    call    emit_tok
    jmp     .next_token

    ; --- Multi-Char & Single-Char Operators / Sigils ---
.check_ops:
    movzx   ecx, byte [rbp + 1]     ; 1-byte lookahead (0-padded safe)
    xor     edx, edx

    cmp     al, ':'
    je      .op_colon
    cmp     al, '.'
    je      .op_dot
    cmp     al, '-'
    je      .op_minus
    cmp     al, '+'
    je      .op_plus
    cmp     al, '*'
    je      .op_star
    cmp     al, '/'
    je      .op_slash
    cmp     al, '='
    je      .op_eq
    cmp     al, '!'
    je      .op_bang
    cmp     al, '<'
    je      .op_lt
    cmp     al, '>'
    je      .op_gt
    cmp     al, '|'
    je      .op_pipe
    cmp     al, '&'
    je      .op_amp
    cmp     al, '^'
    je      .op_caret
    cmp     al, '%'
    je      .op_percent

    ; Pure single-char tokens
    cmp     al, '('
    mov     ah, TOK_LPAREN
    je      .emit_1
    cmp     al, ')'
    mov     ah, TOK_RPAREN
    je      .emit_1
    cmp     al, '{'
    mov     ah, TOK_LBRACE
    je      .emit_1
    cmp     al, '}'
    mov     ah, TOK_RBRACE
    je      .emit_1
    cmp     al, '['
    mov     ah, TOK_LBRACK
    je      .emit_1
    cmp     al, ']'
    mov     ah, TOK_RBRACK
    je      .emit_1
    cmp     al, '@'
    mov     ah, TOK_AT
    je      .emit_1
    cmp     al, ','
    mov     ah, TOK_COMMA
    je      .emit_1
    cmp     al, '~'
    mov     ah, TOK_TILDE
    je      .emit_1
    cmp     al, '?'
    mov     ah, TOK_QUEST
    je      .emit_1
    jmp     lex_err_invalid

.op_colon:
    cmp     cl, ':'
    mov     ah, TOK_COLON2
    je      .emit_2
    cmp     cl, '='
    mov     ah, TOK_WALRUS
    je      .emit_2
    mov     ah, TOK_COLON
    jmp     .emit_1

.op_dot:
    cmp     cl, '.'
    mov     ah, TOK_DOT2
    je      .emit_2
    mov     ah, TOK_DOT
    jmp     .emit_1

.op_minus:
    cmp     cl, '>'
    mov     ah, TOK_ARROW
    je      .emit_2
    cmp     cl, '='
    mov     ah, TOK_MINUS_EQ
    je      .emit_2
    mov     ah, TOK_MINUS
    jmp     .emit_1

.op_plus:
    cmp     cl, '='
    mov     ah, TOK_PLUS_EQ
    je      .emit_2
    mov     ah, TOK_PLUS
    jmp     .emit_1

.op_star:
    cmp     cl, '='
    mov     ah, TOK_STAR_EQ
    je      .emit_2
    mov     ah, TOK_STAR
    jmp     .emit_1

.op_slash:
    cmp     cl, '='
    mov     ah, TOK_SLASH_EQ
    je      .emit_2
    mov     ah, TOK_SLASH
    jmp     .emit_1

.op_eq:
    cmp     cl, '='
    mov     ah, TOK_EQ
    je      .emit_2
    cmp     cl, '>'
    mov     ah, TOK_FAT_ARROW
    je      .emit_2
    mov     ah, TOK_ASSIGN
    jmp     .emit_1

.op_bang:
    cmp     cl, '='
    mov     ah, TOK_NE
    je      .emit_2
    mov     ah, TOK_BANG
    jmp     .emit_1

.op_lt:
    cmp     cl, '='
    mov     ah, TOK_LE
    je      .emit_2
    cmp     cl, '<'
    jne     .lt_one
    cmp     byte [rbp + 2], '<'
    je      .lt_three
    cmp     byte [rbp + 2], '='
    mov     ah, TOK_SHL_EQ
    je      .emit_3
    mov     ah, TOK_SHL
    jmp     .emit_2
.lt_three:
    cmp     byte [rbp + 3], '='
    mov     ah, TOK_ROL_EQ
    je      .emit_4
    mov     ah, TOK_ROL
    jmp     .emit_3
.lt_one:
    mov     ah, TOK_LT
    jmp     .emit_1

.op_gt:
    cmp     cl, '='
    mov     ah, TOK_GE
    je      .emit_2
    cmp     cl, '>'
    jne     .gt_one
    cmp     byte [rbp + 2], '>'
    je      .gt_three
    cmp     byte [rbp + 2], '='
    mov     ah, TOK_SHR_EQ
    je      .emit_3
    mov     ah, TOK_SHR
    jmp     .emit_2
.gt_three:
    cmp     byte [rbp + 3], '='
    mov     ah, TOK_ROR_EQ
    je      .emit_4
    mov     ah, TOK_ROR
    jmp     .emit_3
.gt_one:
    mov     ah, TOK_GT
    jmp     .emit_1

.op_percent:
    cmp     cl, '='
    mov     ah, TOK_PERCENT_EQ
    je      .emit_2
    mov     ah, TOK_PERCENT
    jmp     .emit_1

.op_pipe:
    cmp     cl, '|'
    mov     ah, TOK_OR2
    je      .emit_2
    cmp     cl, '='
    mov     ah, TOK_PIPE_EQ
    je      .emit_2
    cmp     cl, '>'
    mov     ah, TOK_PIPE_GT
    je      .emit_2
    mov     ah, TOK_PIPE
    jmp     .emit_1

.op_amp:
    cmp     cl, '&'
    mov     ah, TOK_AND2
    je      .emit_2
    cmp     cl, '='
    mov     ah, TOK_AMP_EQ
    je      .emit_2
    mov     ah, TOK_AMP
    jmp     .emit_1

.op_caret:
    cmp     cl, '='
    mov     ah, TOK_CARET_EQ
    je      .emit_2
    mov     ah, TOK_CARET
    jmp     .emit_1

.emit_4:
    mov     al, ah
    xor     ah, ah
    mov     ecx, 4
    add     rbp, 4
    call    emit_tok
    jmp     .next_token

.emit_3:
    mov     al, ah
    xor     ah, ah
    mov     ecx, 3
    add     rbp, 3
    call    emit_tok
    jmp     .next_token

.emit_2:
    mov     al, ah
    xor     ah, ah
    mov     ecx, 2
    add     rbp, 2
    call    emit_tok
    jmp     .next_token

.emit_1:
    mov     al, ah
    xor     ah, ah
    mov     ecx, 1
    inc     rbp
    call    emit_tok
    jmp     .next_token

.emit_eof:
    xor     eax, eax                ; TOK_EOF = 0
    xor     ecx, ecx
    xor     edx, edx
    call    emit_tok

    pop     r14
    pop     r12
    pop     rbp
    pop     rbx
    ret

; -----------------------------------------------------------------------------
; Lexer Diagnostics & `--lex` Token Stream Inspector
; -----------------------------------------------------------------------------
lex_err_invalid:
    lea     rsi, [msg_lex_chr]
    mov     edx, msg_lex_chr_len
    jmp     lex_die_with_line
lex_err_unclosed_str:
    lea     rsi, [msg_lex_str]
    mov     edx, msg_lex_str_len
    jmp     lex_die_with_line
lex_err_unclosed_comment:
    lea     rsi, [msg_lex_cmt]
    mov     edx, msg_lex_cmt_len

lex_die_with_line:
    mov     edi, 2
    mov     eax, SYS_WRITE
    syscall
    mov     edi, 2
    mov     esi, [cur_line]
    call    write_u64_fd
    mov     edi, 2
    lea     rsi, [nl_byte]
    mov     edx, 1
    mov     eax, SYS_WRITE
    syscall
    mov     edi, 1
    mov     eax, SYS_EXIT
    syscall

; Decodes ASCII hex char in `al` -> `al` (0..15), or dies via `lex_err_invalid`
hex_nibble:
    cmp     al, '0'
    jb      lex_err_invalid
    cmp     al, '9'
    jbe     .digit
    or      al, 0x20
    cmp     al, 'a'
    jb      lex_err_invalid
    cmp     al, 'f'
    ja      lex_err_invalid
    sub     al, 'a' - 10
    ret
.digit:
    sub     al, '0'
    ret

; write_u64_fd(edi = fd, rsi = u64 value)
write_u64_fd:
    sub     rsp, 32
    lea     r8, [rsp + 31]
    mov     rax, rsi
    mov     r9d, 10
    xor     ecx, ecx
.u64_loop:
    xor     edx, edx
    div     r9
    add     dl, '0'
    dec     r8
    mov     [r8], dl
    inc     ecx
    test    rax, rax
    jnz     .u64_loop
    mov     rsi, r8
    mov     edx, ecx
    mov     eax, SYS_WRITE
    syscall
    add     rsp, 32
    ret

; lexer_dump — Prints every 16B token for `./zero0 --lex <file.zr>`
lexer_dump:
    push    rbx
    push    r12
    push    r13

    xor     ebx, ebx                ; token index
    mov     r12, [space_tok]
    mov     r13, [space_sym]

.dump_loop:
    cmp     ebx, [tok_count]
    jae     .dump_summary

    mov     eax, ebx
    shl     rax, 4
    add     rax, r12                ; rax = &Token[ebx]
    push    rax

    ; Print `L<line>\t`
    mov     edi, 1
    lea     rsi, [lbl_line_pfx]
    mov     edx, 1
    mov     eax, SYS_WRITE
    syscall

    mov     rax, [rsp]
    mov     edi, 1
    mov     esi, [rax + 4]          ; line
    call    write_u64_fd

    mov     edi, 1
    lea     rsi, [lbl_tab_kind]
    mov     edx, lbl_tab_kind_len
    mov     eax, SYS_WRITE
    syscall

    mov     rax, [rsp]
    mov     edi, 1
    movzx   esi, byte [rax]         ; kind
    call    write_u64_fd

    mov     rax, [rsp]
    movzx   ecx, byte [rax]
    cmp     cl, TOK_IDENT
    je      .dump_sym
    cmp     cl, TOK_KW
    je      .dump_sym
    cmp     cl, TOK_INT
    je      .dump_int
    cmp     cl, TOK_CHAR
    je      .dump_int
    cmp     cl, TOK_STR
    je      .dump_str
    jmp     .dump_nl

.dump_sym:
    mov     edi, 1
    lea     rsi, [lbl_sym]
    mov     edx, lbl_sym_len
    mov     eax, SYS_WRITE
    syscall

    mov     rax, [rsp]
    mov     r8,  [rax + 8]          ; sym_id
    mov     edi, 1
    mov     rsi, r8
    call    write_u64_fd

    mov     edi, 1
    lea     rsi, [lbl_sp]
    mov     edx, 2
    mov     eax, SYS_WRITE
    syscall

    mov     rax, [rsp]
    mov     ecx, [rax + 8]          ; sym_id
    shl     ecx, 4
    mov     rsi, [r13 + SYM_META_OFF + rcx]
    mov     edx, [r13 + SYM_META_OFF + rcx + 8]
    mov     edi, 1
    mov     eax, SYS_WRITE
    syscall
    jmp     .dump_nl

.dump_int:
    mov     edi, 1
    lea     rsi, [lbl_val]
    mov     edx, lbl_val_len
    mov     eax, SYS_WRITE
    syscall
    mov     rax, [rsp]
    mov     edi, 1
    mov     rsi, [rax + 8]
    call    write_u64_fd
    jmp     .dump_nl

.dump_str:
    mov     edi, 1
    lea     rsi, [lbl_str]
    mov     edx, lbl_str_len
    mov     eax, SYS_WRITE
    syscall
    mov     rax, [rsp]
    mov     rsi, [space_rodata]
    add     rsi, [rax + 8]
    movzx   edx, word [rax + 2]
    mov     edi, 1
    mov     eax, SYS_WRITE
    syscall
    mov     edi, 1
    lea     rsi, [lbl_quote]
    mov     edx, 1
    mov     eax, SYS_WRITE
    syscall

.dump_nl:
    pop     rax
    mov     edi, 1
    lea     rsi, [nl_byte]
    mov     edx, 1
    mov     eax, SYS_WRITE
    syscall

    inc     ebx
    jmp     .dump_loop

.dump_summary:
    mov     edi, 1
    lea     rsi, [msg_lex_sum1]
    mov     edx, msg_lex_sum1_len
    mov     eax, SYS_WRITE
    syscall

    mov     edi, 1
    mov     esi, [tok_count]
    call    write_u64_fd

    mov     edi, 1
    lea     rsi, [msg_lex_sum2]
    mov     edx, msg_lex_sum2_len
    mov     eax, SYS_WRITE
    syscall

    mov     edi, 1
    mov     esi, [sym_count]
    call    write_u64_fd

    mov     edi, 1
    lea     rsi, [msg_lex_sum3]
    mov     edx, msg_lex_sum3_len
    mov     eax, SYS_WRITE
    syscall

    pop     r13
    pop     r12
    pop     rbx
    ret

; -----------------------------------------------------------------------------
; Pre-Seeded Keyword Table (Exact Order 1 .. KW_COUNT = 39)
; -----------------------------------------------------------------------------
kw_seed_table:
    db 2, "fn"              ; 1  KW_FN
    db 6, "return"          ; 2  KW_RETURN
    db 2, "if"              ; 3  KW_IF
    db 4, "else"            ; 4  KW_ELSE
    db 3, "for"             ; 5  KW_FOR
    db 2, "in"              ; 6  KW_IN
    db 4, "loop"            ; 7  KW_LOOP
    db 5, "break"           ; 8  KW_BREAK
    db 8, "continue"        ; 9  KW_CONTINUE
    db 7, "recover"         ; 10 KW_RECOVER
    db 5, "print"           ; 11 KW_PRINT
    db 7, "syscall"         ; 12 KW_SYSCALL
    db 4, "true"            ; 13 KW_TRUE
    db 5, "false"           ; 14 KW_FALSE
    db 3, "bit"             ; 15 KW_BIT
    db 4, "bool"            ; 16 KW_BOOL
    db 2, "u8"              ; 17 KW_U8
    db 3, "u16"             ; 18 KW_U16
    db 3, "u32"             ; 19 KW_U32
    db 3, "u64"             ; 20 KW_U64
    db 2, "i8"              ; 21 KW_I8
    db 3, "i16"             ; 22 KW_I16
    db 3, "i32"             ; 23 KW_I32
    db 3, "i64"             ; 24 KW_I64
    db 3, "f32"             ; 25 KW_F32
    db 3, "f64"             ; 26 KW_F64
    db 3, "str"             ; 27 KW_STR
    db 5, "bytes"           ; 28 KW_BYTES
    db 6, "mask64"          ; 29 KW_MASK64
    db 4, "ring"            ; 30 KW_RING
    db 4, "slab"            ; 31 KW_SLAB
    db 6, "shared"          ; 32 KW_SHARED
    db 5, "arena"           ; 33 KW_ARENA
    db 4, "mmio"            ; 34 KW_MMIO
    db 3, "rel"             ; 35 KW_REL
    db 6, "cell64"          ; 36 KW_CELL64
    db 4, "cold"            ; 37 KW_COLD
    db 6, "system"          ; 38 KW_SYSTEM
    db 5, "stage"           ; 39 KW_STAGE
    db 4, "fail"            ; 40 KW_FAIL
    db 0                    ; End sentinel

lbl_line_pfx:       db "L"
lbl_tab_kind:       db 9, "kind="
lbl_tab_kind_len    equ $ - lbl_tab_kind
lbl_sym:            db 9, "sym#"
lbl_sym_len         equ $ - lbl_sym
lbl_sp:             db ": "
lbl_val:            db 9, "val="
lbl_val_len         equ $ - lbl_val
lbl_str:            db 9, "str=", '"'
lbl_str_len         equ $ - lbl_str
lbl_quote:          db '"'
nl_byte:            db 10

msg_lex_chr:        db "[zero0] lex fault: unexpected character at line "
msg_lex_chr_len     equ $ - msg_lex_chr
msg_lex_str:        db "[zero0] lex fault: unclosed string literal at line "
msg_lex_str_len     equ $ - msg_lex_str
msg_lex_cmt:        db "[zero0] lex fault: unclosed /* comment at line "
msg_lex_cmt_len     equ $ - msg_lex_cmt

msg_lex_sum1:       db "[zero0] lex ok: "
msg_lex_sum1_len    equ $ - msg_lex_sum1
msg_lex_sum2:       db " tokens, "
msg_lex_sum2_len    equ $ - msg_lex_sum2
msg_lex_sum3:       db " interned symbols (39 built-in keywords)", 10
msg_lex_sum3_len    equ $ - msg_lex_sum3
