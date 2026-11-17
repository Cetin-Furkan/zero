; =============================================================================
; Zero Stage-0 Bootstrap — Immutable `@capsule` Container (`bootstrap/capsule_writer.inc`)
; =============================================================================
; Emits a position-independent 64B-header `.zcap` image (`ZERO_OS_ARCHITECTURE.md` §3.2):
;   +0   u64  magic        = 0x5041435A4F52455A ('ZEROCAP' + 'P')
;   +8   u32  version      = 1
;   +12  u32  entry_off    = 64
;   +16  u32  code_bytes   = aligned8 payload byte length
;   +20  u32  crc32c       = hardware SSE4.2 CRC32C of payload qwords [64 .. 64+code_bytes)
;   +24  u8[40] reserved   = 0
;   +64  ...  RX Reactor-ABI machine code (`jmp rel32 -> main` at +64)
; =============================================================================

%define ZCAP_HDR_SIZE       64
%define ZCAP_MAGIC_LO       0x4F52455A  ; "ZERO"
%define ZCAP_MAGIC_HI       0x5041435A  ; "ZCAP"

emit_capsule_target_runtime:
    ; At offset +64: `E9 <rel32>` jumping directly into `.zr` `fn main`!
    mov     al, 0xE9
    call    emit_u8
    mov     edi, [sym_main_id]
    call    find_or_add_fn
    mov     ebx, eax
    mov     edx, [code_off]
    mov     rcx, [space_ast]
    add     rcx, AST_CALLFIX_OFF
    mov     eax, [call_fix_cnt]
    mov     [rcx + rax * 8],     edx
    mov     [rcx + rax * 8 + 4], ebx
    inc     dword [call_fix_cnt]
    xor     eax, eax
    jmp     emit_u32

finalize_capsule_image:
    push    rbx
    push    r12
    push    r15

    mov     r15, [space_out]

    ; 1. Pad payload `[64 .. code_off)` with `0x90` / `0x00` up to 8-byte boundary
    mov     edi, [code_off]
    lea     ebx, [rdi + 7]
    and     ebx, ~7                 ; ebx = total file size (multiple of 8)
.pad_lp:
    cmp     edi, ebx
    jae     .pad_dn
    mov     byte [r15 + rdi], 0
    inc     edi
    jmp     .pad_lp
.pad_dn:
    mov     [total_out], ebx

    ; 2. Compute hardware `crc32` (`CRC32C` Castagnoli) over qwords in `[64 .. ebx)`
    mov     r12d, ebx
    sub     r12d, ZCAP_HDR_SIZE     ; r12d = payload code_bytes
    mov     eax, 0xFFFFFFFF         ; initial seed
    lea     rsi, [r15 + ZCAP_HDR_SIZE]
    mov     ecx, r12d
    shr     ecx, 3                  ; qword count
.crc_lp:
    test    ecx, ecx
    jz      .crc_dn
    crc32   rax, qword [rsi]
    add     rsi, 8
    dec     ecx
    jmp     .crc_lp
.crc_dn:
    not     eax                     ; standard finalized CRC32C (`~acc`)
    mov     edx, eax

    ; 3. Zero 64-byte header and stamp fields
    mov     rdi, r15
    xor     eax, eax
    mov     ecx, 8
    rep     stosq

    mov     dword [r15 + 0],  ZCAP_MAGIC_LO
    mov     dword [r15 + 4],  ZCAP_MAGIC_HI
    mov     dword [r15 + 8],  1             ; version = 1
    mov     dword [r15 + 12], ZCAP_HDR_SIZE ; entry_off = 64
    mov     dword [r15 + 16], r12d          ; code_bytes
    mov     dword [r15 + 20], edx           ; crc32c

    pop     r15
    pop     r12
    pop     rbx
    ret
