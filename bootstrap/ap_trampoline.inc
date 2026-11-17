; =============================================================================
; Zero Stage-0 Bootstrap — Gate-1b AP Startup Trampoline (`bootstrap/ap_trampoline.inc`)
; =============================================================================
; Isolated `< 1 MiB` hardware bridge copied before `ExitBootServices` into a
; UEFI-allocated `EfiLoaderData` region (`4` pages = `16 KiB` `< 0xA0000`):
;   - `tramp_phys + 0x0000`: Read-Only Executable (`PTE_RX` in `ZeroOS` `CR3`)
;   - `tramp_phys + 0x1000`: Transitional PML4 (`[0] -> +0x2000 | 0x03`)
;   - `tramp_phys + 0x2000`: Transitional PDPT (`[0] -> +0x3000 | 0x03`)
;   - `tramp_phys + 0x3000`: Transitional PD   (`[0] -> 0x000000 | 0x83`, 0..2MiB)
;
; Included inside `efi_harvest_stub` using `.ap_*` local labels so outer local
; label scope (`efi_harvest_stub.*`) remains unbroken.
; =============================================================================

AP_TRAMP_GDT_OFF        equ 0x0100
AP_TRAMP_GDTR_OFF       equ 0x0120
AP_TRAMP_LJMP32_OFF     equ 0x0128
AP_TRAMP_LJMP64_OFF     equ 0x0130
AP_TRAMP_CR3_OFF        equ 0x0138
AP_TRAMP_ENTRY64_OFF    equ 0x0140
AP_TRAMP_MBOX_OFF       equ 0x0148

bits 16
.ap_trampoline_blob:
    cli
    cld
    mov     ax, cs
    mov     ds, ax                  ; DS = CS = tramp_phys >> 4
    xor     ebx, ebx
    mov     bx, cs
    shl     ebx, 4                  ; EBX = tramp_phys (< 0xA0000, preserved into 64b)

    o32 lgdt [AP_TRAMP_GDTR_OFF]

    mov     eax, cr0
    or      eax, 1                  ; CR0.PE = 1
    mov     cr0, eax

    jmp     dword far [AP_TRAMP_LJMP32_OFF]

bits 32
align 4, db 0x90
.ap_tramp_pm32:
    mov     ax, 0x10
    mov     ds, ax
    mov     es, ax
    mov     ss, ax

    ; 1. Enable CR4.PAE (bit 5 = 0x20)
    mov     eax, cr4
    or      eax, 0x20
    mov     cr4, eax

    ; 2. Load transitional <1 MiB PML4 (`tramp_phys + 0x1000`) so `new_cr3` may reside > 4 GiB
    lea     eax, [ebx + 0x1000]
    mov     cr3, eax

    ; 3. Enable IA32_EFER.LME (0x100) | IA32_EFER.NXE (0x800) BEFORE loading W^X `new_cr3`
    mov     ecx, 0xC0000080
    rdmsr
    or      eax, 0x00000900
    wrmsr

    ; 4. Enable CR0.PG | CR0.WP | CR0.NE | CR0.MP | CR0.PE
    mov     eax, cr0
    or      eax, 0x80010023
    mov     cr0, eax

    ; 5. Enter 64-bit Long Mode (`0x0018 : tramp_phys + AP_TRAMP_LM64_REL`)
    jmp     far [ebx + AP_TRAMP_LJMP64_OFF]

bits 64
align 8, db 0x90
.ap_tramp_lm64:
    xor     eax, eax
    mov     ds, ax
    mov     es, ax
    mov     ss, ax
    mov     fs, ax
    mov     gs, ax

    ; Switch immediately to full 64-bit hardware W^X `new_cr3`
    mov     rax, [rbx + AP_TRAMP_CR3_OFF]
    mov     cr3, rax

    ; Load per-AP dedicated 16 KiB stack, Reactor ABI (`r12..r15` + `gs:0`), & core slot from `ap_mbox`
    mov     rsi, [rbx + AP_TRAMP_MBOX_OFF]
    mov     rsp, [rsi + 8]          ; 16-byte aligned stack top
    xor     ebp, ebp
    mov     r12, [rsi + 0]          ; r12 = immutable Reactor ID (`slot 1..15`)
    xor     r13d, r13d              ; r13 = 0 (inbox_base until bound)
    xor     r14d, r14d              ; r14 = 0 (scratch_base until bound)
    mov     r15, [rsi + 40]         ; r15 = &ReactorLocal[slot] (`mbox[5]`)

    ; Bind `IA32_GS_BASE` (`MSR 0xC0000101`) = `r15` so `[gs:0]` is per-AP from cycle 0!
    mov     rax, r15
    mov     rdx, r15
    shr     rdx, 32
    mov     ecx, 0xC0000101
    wrmsr

    mov     rdi, r12                ; arg0 (`rdi`) = core slot (`1..15`)
    mov     rax, [rbx + AP_TRAMP_ENTRY64_OFF]
    mov     qword [rsi + 16], 1     ; started = 1 (signals BSP not to send 2nd SIPI)
    xor     ebx, ebx
    call    rax                     ; enter `.zr` `fn ap_main(slot: u64)`
.ap_halt_forever:
    cli
    hlt
    jmp     .ap_halt_forever

    times AP_TRAMP_GDT_OFF - ($ - .ap_trampoline_blob) db 0x90

    ; +0x100: Temporary flat GDT (32 bytes)
    dq 0x0000000000000000           ; 0x00: null
    dq 0x00CF9A000000FFFF           ; 0x08: 32-bit flat code (RX)
    dq 0x00CF92000000FFFF           ; 0x10: 32-bit flat data (RW)
    dq 0x00AF9A000000FFFF           ; 0x18: 64-bit long-mode code (RX)

    ; +0x120: GDTR (6B + 2B pad)
    dw 31
    dd 0                            ; patched at copy time -> `tramp_phys + 0x100`
    dw 0

    ; +0x128: 32-bit far pointer (6B + 2B pad)
    dd 0                            ; patched at copy time -> `tramp_phys + AP_TRAMP_PM32_REL`
    dw 0x0008
    dw 0

    ; +0x130: 64-bit far pointer (6B + 2B pad)
    dd 0                            ; patched at copy time -> `tramp_phys + AP_TRAMP_LM64_REL`
    dw 0x0018
    dw 0

    ; +0x138..0x14F: 64-bit Ring-0 parameters written by `gate1_os.zr` pre-CR3
    dq 0                            ; +0x138: boot_cr3
    dq 0                            ; +0x140: boot_entry64
    dq 0                            ; +0x148: boot_mbox
.ap_trampoline_blob_end:

AP_TRAMP_PM32_REL       equ (.ap_tramp_pm32 - .ap_trampoline_blob)
AP_TRAMP_LM64_REL       equ (.ap_tramp_lm64 - .ap_trampoline_blob)
AP_TRAMP_BLOB_LEN       equ (.ap_trampoline_blob_end - .ap_trampoline_blob)
