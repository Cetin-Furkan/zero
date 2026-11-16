; =============================================================================
; Zero Stage-0 Bootstrap — UEFI Pre-Exit Handoff (`bootstrap/efi_boot.inc`)
; =============================================================================
; Implements `PEER_MASTER_PLAN.md` §8.1–§8.3 UEFI entry & BootHandoff builder:
;   - Enters in Microsoft x64 ABI (`rcx = ImageHandle`, `rdx = SystemTable`).
;   - Queries `EFI_GRAPHICS_OUTPUT_PROTOCOL` via `LocateProtocol` (returns to
;     firmware cleanly if absent, never guessing a framebuffer address).
;   - Locates ACPI 2.0 / 1.0 `RSDP` from `SystemTable->ConfigurationTable`.
;   - Queries `GetMemoryMap`, populates `BootHandoff` + `MemRange[]` inside
;     loader-allocated `.bss`, and calls `ExitBootServices`.
;   - Switches `RSP` to a 64 KiB `EfiLoaderData` stack and calls `.zr` `main`.
;   - Emits 256 16-byte exception gate stubs writing the vector to `.bss+0x1C000`.
; =============================================================================

%define EFI_SYS_BOOT_SERVICES       0x60
%define EFI_SYS_NR_CFG_TABLES       0x68
%define EFI_SYS_CFG_TABLES          0x70

%define EFI_BS_GET_MEMORY_MAP       0x38
%define EFI_BS_EXIT_BOOT_SERVICES   0xE8
%define EFI_BS_LOCATE_PROTOCOL      0x140

%define EFI_BSS_HANDOFF_OFF         0x00040
%define EFI_BSS_MEMMAP_OFF          0x00100
%define EFI_BSS_RAW_MMAP_OFF        0x04000
%define EFI_BSS_RAW_MMAP_CAP        0x08000
%define EFI_BSS_STACK_TOP_OFF       0x1C000
%define EFI_BSS_ISR_LINE_OFF        0x1C040
%define EFI_BSS_USER_START          0x20000

%define PE_HDR_FILE_SIZE            0x00200
%define PE_TEXT_RVA                 0x01000

; Emits the complete UEFI entry stub + `call main(&BootHandoff)` + 256 ISR stubs
emit_efi_target_runtime:
    push    rbx
    push    r12

    ; 1. Save MS x64 non-volatile GPRs so failure before ExitBootServices returns cleanly
    lea     rsi, [efi_save_nonvol]
    mov     ecx, efi_save_nonvol_end - efi_save_nonvol
    call    emit_bytes

    ; 2. `lea r15, [rip + disp32]` -> `r15` = runtime physical base of `.bss` (`bss_off = 0`)
    mov     al, 0x4C
    call    emit_u8
    mov     ax, 0x3D8D              ; 4C 8D 3D <disp32> = lea r15, [rip + disp32]
    call    emit_u16
    xor     edi, edi                ; bss_offset = 0
    call    emit_efi_bss_disp32

    ; 2b. `lea r10, [rip + disp32]` -> physical base of `.text` (`PE_HDR_FILE_SIZE` -> RVA 0x1000)
    ;     `mov r11d, imm32`        -> aligned `.text` span (patched in `finalize_efi_image`)
    mov     al, 0x4C
    call    emit_u8
    mov     ax, 0x158D              ; 4C 8D 15 <disp32> = lea r10, [rip + disp32]
    call    emit_u16
    mov     eax, PE_HDR_FILE_SIZE
    mov     ecx, [code_off]
    add     ecx, 4
    sub     eax, ecx
    call    emit_u32

    mov     ax, 0xBB41              ; 41 BB <imm32> = mov r11d, imm32
    call    emit_u16
    mov     eax, [code_off]
    mov     [efi_code_span_patch_off], eax
    xor     eax, eax
    call    emit_u32

    ; 3. Copy position-independent UEFI harvest + ExitBootServices stub
    lea     rsi, [efi_harvest_stub]
    mov     ecx, efi_harvest_stub_end - efi_harvest_stub
    call    emit_bytes

    ; 4. Call `.zr` `main(rdi = &BootHandoff)`
    mov     edi, [sym_main_id]
    call    find_or_add_fn
    mov     edi, eax
    call    emit_call_fn_idx

    ; 5. Post-main halt loop (`cli; .h: hlt; jmp .h`)
    mov     eax, 0xFDEBF4FA         ; FA (cli) F4 (hlt) EB FD (jmp -3)
    call    emit_u32

    ; 6. Align `.text` to 16 bytes and emit 256 x 16-byte exception gate stubs
    mov     eax, [code_off]
    add     eax, 15
    and     eax, ~15
    mov     [code_off], eax
    mov     [efi_isr_stubs_off], eax

    xor     ebx, ebx                ; vector 0..255
.isr_loop:
    cmp     ebx, 256
    jae     .done
    ; 16-byte stub:
    ;   0: FA                       cli                         (1B)
    ;   1: C7 05 <disp32> <imm32>   mov dword [rip+isr_line], v (10B)
    ;  11: F4                       hlt                         (1B)
    ;  12: EB FD                    jmp -3 (back to hlt)        (2B)
    ;  14: 66 90                    nop                         (2B)
    mov     al, 0xFA
    call    emit_u8
    mov     ax, 0x05C7
    call    emit_u16
    mov     edi, EFI_BSS_ISR_LINE_OFF
    ; Note: in `C7 05 <disp32> <imm32>`, next RIP is `disp_pos + 8` (not +4!)
    ; So record with `bss_off - 4` to compensate for the trailing 4-byte imm32!
    sub     edi, 4
    call    emit_efi_bss_disp32
    mov     eax, ebx
    call    emit_u32
    mov     al, 0xF4
    call    emit_u8
    mov     ax, 0xFDEB
    call    emit_u16
    mov     ax, 0x9066
    call    emit_u16

    inc     ebx
    jmp     .isr_loop

.done:
    pop     r12
    pop     rbx
    ret

; Records a 4-byte RIP-relative `.bss` displacement at current `[code_off]`
emit_efi_bss_disp32:                ; edi = target offset inside `.bss`
    mov     edx, [code_off]
    mov     eax, [fixup_cnt]
    mov     rcx, [space_fixup]
    mov     [rcx + rax * 8],     edx
    mov     [rcx + rax * 8 + 4], edi
    inc     dword [fixup_cnt]
    xor     eax, eax
    jmp     emit_u32

efi_save_nonvol:
    push    rbx
    push    rbp
    push    rdi
    push    rsi
    push    r12
    push    r13
    push    r14
    push    r15
efi_save_nonvol_end:

; -----------------------------------------------------------------------------
; Position-Independent UEFI Pre-Exit Harvest Stub
; Entry: `rcx = ImageHandle`, `rdx = SystemTable`, `r15 = physical .bss base`
; -----------------------------------------------------------------------------
efi_harvest_stub:
    mov     rbp, rsp
    and     rsp, -16
    sub     rsp, 96                 ; 32B MS shadow space + local scratch slots

    mov     r12, rcx                ; r12 = ImageHandle
    mov     r13, rdx                ; r13 = SystemTable
    mov     r14, [rdx + EFI_SYS_BOOT_SERVICES] ; r14 = BootServices*

    ; Save physical `.text` span BEFORE volatile MS x64 firmware calls clobber `r10`/`r11`!
    mov     [r15 + EFI_BSS_HANDOFF_OFF + 96],  r10 ; BootHandoff.code_base
    mov     [r15 + EFI_BSS_HANDOFF_OFF + 104], r11 ; BootHandoff.code_bytes

    ; --- 1. Locate GOP (`LocateProtocol(&gop_guid, NULL, &gop_ptr)`) ---
    lea     rcx, [rel .gop_guid]
    xor     edx, edx
    lea     r8,  [rsp + 32]         ; &gop_ptr
    mov     rax, [r14 + EFI_BS_LOCATE_PROTOCOL]
    call    rax
    test    rax, rax
    jnz     .fail_return_firmware   ; GOP missing -> return cleanly to UEFI (§8.5)

    mov     rbx, [rsp + 32]         ; rbx = EFI_GRAPHICS_OUTPUT_PROTOCOL*
    mov     rbx, [rbx + 0x18]       ; rbx = GOP->Mode
    mov     rsi, [rbx + 0x08]       ; rsi = GOP->Mode->Info

    ; Populate `BootHandoff` at `r15 + EFI_BSS_HANDOFF_OFF`
    lea     rdi, [r15 + EFI_BSS_HANDOFF_OFF]
    mov     rax, 0x5A45524F48414E44 ; 'ZEROHAND' magic (§8.2)
    mov     [rdi + 0],  rax
    mov     dword [rdi + 8],  1     ; version = 1
    mov     dword [rdi + 12], 1     ; core_count = 1 (Gate 1a BSP-only)

    mov     rax, [rbx + 0x18]       ; FrameBufferBase (physical)
    mov     [rdi + 16], rax
    mov     rcx, [rbx + 0x20]       ; FrameBufferSize (bytes)
    mov     [rsp + 40], rcx         ; save fb_size

    mov     eax, [rsi + 0x04]       ; HorizontalResolution
    mov     [rdi + 24], eax
    mov     eax, [rsi + 0x08]       ; VerticalResolution
    mov     [rdi + 28], eax
    mov     eax, [rsi + 0x20]       ; PixelsPerScanLine
    shl     eax, 2                  ; gop_stride in bytes (`* 4`)
    mov     [rdi + 32], eax

    ; Translate UEFI PixelFormat:
    ;   1 (BGRX8888) -> 0, 0 (RGBX8888) -> 1, 2 (PixelBitMask RGB/BGR 32b) -> 0/1, else 0xFF
    mov     eax, [rsi + 0x0C]
    mov     edx, 0xFF
    cmp     eax, 1
    je      .fmt_bgrx
    test    eax, eax
    je      .fmt_rgbx
    cmp     eax, 2
    jne     .set_fmt
    cmp     dword [rsi + 0x10], 0x00FF0000
    jne     .chk_bm_rgbx
    cmp     dword [rsi + 0x18], 0x000000FF
    je      .fmt_bgrx
    jmp     .set_fmt
.chk_bm_rgbx:
    cmp     dword [rsi + 0x10], 0x000000FF
    jne     .set_fmt
    cmp     dword [rsi + 0x18], 0x00FF0000
    jne     .set_fmt
.fmt_rgbx:
    mov     edx, 1
    jmp     .set_fmt
.fmt_bgrx:
    xor     edx, edx
.set_fmt:
    mov     [rdi + 36], edx

    ; --- 2. Scan `SystemTable->ConfigurationTable` for ACPI 2.0 / 1.0 RSDP & Walk XSDT/RSDT ---
    mov     rcx, [r13 + EFI_SYS_NR_CFG_TABLES]
    mov     rsi, [r13 + EFI_SYS_CFG_TABLES]
    xor     r8,  r8                 ; fallback ACPI 1.0 RSDP
.cfg_loop:
    test    rcx, rcx
    jz      .cfg_done
    mov     rax, [rsi]
    cmp     rax, [rel .acpi20_guid]
    jne     .chk_acpi1
    mov     rax, [rsi + 8]
    cmp     rax, [rel .acpi20_guid + 8]
    jne     .chk_acpi1
    mov     r8,  [rsi + 16]         ; exact ACPI 2.0 RSDP found!
    jmp     .cfg_done
.chk_acpi1:
    mov     rax, [rsi]
    cmp     rax, [rel .acpi10_guid]
    jne     .cfg_next
    mov     rax, [rsi + 8]
    cmp     rax, [rel .acpi10_guid + 8]
    jne     .cfg_next
    mov     r8,  [rsi + 16]
.cfg_next:
    add     rsi, 24
    dec     rcx
    jmp     .cfg_loop
.cfg_done:
    mov     [rdi + 40], r8          ; BootHandoff.rsdp
    mov     qword [rdi + 48], 0     ; BootHandoff.madt
    mov     qword [rdi + 56], 0     ; BootHandoff.mcfg

    test    r8, r8
    jz      .acpi_done
    mov     rax, 0x2052545020445352 ; "RSD PTR "
    cmp     [r8], rax
    jne     .acpi_done

    cmp     byte [r8 + 15], 2
    jb      .try_rsdt
    mov     rsi, [r8 + 24]          ; XsdtAddress
    test    rsi, rsi
    jz      .try_rsdt
    cmp     dword [rsi], 0x54445358 ; "XSDT"
    jne     .try_rsdt
    mov     eax, [rsi + 4]          ; Length
    cmp     eax, 36
    jb      .acpi_done
    lea     rcx, [rsi + 36]
    lea     rdx, [rsi + rax]
.xsdt_loop:
    lea     r9, [rcx + 8]
    cmp     r9, rdx
    ja      .acpi_done
    mov     rax, [rcx]
    test    rax, rax
    jz      .xsdt_next
    mov     r10d, [rax]
    cmp     r10d, 0x43495041        ; "APIC" (MADT)
    jne     .x_chk_mcfg
    mov     [rdi + 48], rax
    jmp     .xsdt_next
.x_chk_mcfg:
    cmp     r10d, 0x4746434D        ; "MCFG"
    jne     .xsdt_next
    mov     [rdi + 56], rax
.xsdt_next:
    add     rcx, 8
    jmp     .xsdt_loop

.try_rsdt:
    mov     esi, [r8 + 16]          ; RsdtAddress (zero-extended)
    test    rsi, rsi
    jz      .acpi_done
    cmp     dword [rsi], 0x54445352 ; "RSDT"
    jne     .acpi_done
    mov     eax, [rsi + 4]
    cmp     eax, 36
    jb      .acpi_done
    lea     rcx, [rsi + 36]
    lea     rdx, [rsi + rax]
.rsdt_loop:
    lea     r9, [rcx + 4]
    cmp     r9, rdx
    ja      .acpi_done
    mov     eax, [rcx]
    test    rax, rax
    jz      .rsdt_next
    mov     r10d, [rax]
    cmp     r10d, 0x43495041        ; "APIC" (MADT)
    jne     .r_chk_mcfg
    mov     [rdi + 48], rax
    jmp     .rsdt_next
.r_chk_mcfg:
    cmp     r10d, 0x4746434D        ; "MCFG"
    jne     .rsdt_next
    mov     [rdi + 56], rax
.rsdt_next:
    add     rcx, 4
    jmp     .rsdt_loop
.acpi_done:

    ; --- 3. Query UEFI Memory Map & ExitBootServices (with retry on key change) ---
.mmap_retry:
    mov     qword [rsp + 48], EFI_BSS_RAW_MMAP_CAP ; MemoryMapSize
    lea     rcx, [rsp + 48]         ; &MemoryMapSize
    lea     rdx, [r15 + EFI_BSS_RAW_MMAP_OFF] ; MemoryMap buffer
    lea     r8,  [rsp + 56]         ; &MapKey
    lea     r9,  [rsp + 64]         ; &DescriptorSize
    lea     rax, [rsp + 72]         ; &DescriptorVersion (5th arg at [rsp + 32])
    mov     [rsp + 32], rax
    mov     rax, [r14 + EFI_BS_GET_MEMORY_MAP]
    call    rax
    test    rax, rax
    jnz     .fail_return_firmware

    ; Convert raw UEFI descriptors into `MemRange[]` at `r15 + EFI_BSS_MEMMAP_OFF`
    lea     rsi, [r15 + EFI_BSS_RAW_MMAP_OFF]
    mov     rcx, rsi
    add     rcx, [rsp + 48]         ; end of raw buffer
    mov     r9,  [rsp + 64]         ; DescriptorSize
    lea     rdi, [r15 + EFI_BSS_MEMMAP_OFF]
    xor     ebx, ebx                ; map_count = 0

.desc_loop:
    cmp     rsi, rcx
    jae     .append_gop_range
    cmp     ebx, 510
    jae     .append_gop_range

    mov     eax, [rsi + 0]          ; Type (u32)
    mov     r10, [rsi + 8]          ; PhysicalStart (u64)
    mov     r11, [rsi + 24]         ; NumberOfPages (u64)
    shl     r11, 12                 ; bytes = pages * 4096
    test    r11, r11
    jz      .desc_next

    ; Classify:
    ;   Types 1,2,3,4,7 -> kind=1 (WB DRAM)
    ;   Types 6,9,10    -> kind=5 (ACPI / Firmware RO + NX)
    ;   Types 11,12     -> kind=3 (UC MMIO)
    ;   All others      -> kind=4 (Unmapped Reserved)
    mov     edx, 4
    cmp     eax, 7                  ; EfiConventionalMemory
    je      .is_wb
    cmp     eax, 1                  ; EfiLoaderCode..EfiBootServicesData (1..4)
    jb      .chk_acpi_ro
    cmp     eax, 4
    jbe     .is_wb
.chk_acpi_ro:
    cmp     eax, 6                  ; EfiRuntimeServicesData
    je      .is_acpi
    cmp     eax, 9                  ; EfiACPIReclaimMemory
    je      .is_acpi
    cmp     eax, 10                 ; EfiACPIMemoryNVS
    je      .is_acpi
.chk_mmio:
    cmp     eax, 11                 ; EfiMemoryMappedIO
    je      .is_mmio
    cmp     eax, 12                 ; EfiMemoryMappedIOPortSpace
    jne     .check_coalesce
.is_mmio:
    mov     edx, 3                  ; kind = 3 (UC MMIO)
    jmp     .check_coalesce
.is_acpi:
    mov     edx, 5                  ; kind = 5 (ACPI Read-Only)
    jmp     .check_coalesce
.is_wb:
    mov     edx, 1                  ; kind = 1 (WB DRAM)

.check_coalesce:
    ; Coalesce contiguous spans of identical `kind` so 2MiB/1GiB pages form cleanly!
    test    ebx, ebx
    jz      .emit_range
    lea     r8, [rbx - 1]
    imul    r8, r8, 24
    cmp     [rdi + r8 + 0], edx     ; same kind?
    jne     .emit_range
    mov     rax, [rdi + r8 + 8]     ; prev.phys
    add     rax, [rdi + r8 + 16]    ; prev.phys + prev.bytes
    cmp     rax, r10                ; contiguous?
    jne     .emit_range
    add     [rdi + r8 + 16], r11    ; extend prev.bytes!
    jmp     .desc_next

.emit_range:
    mov     r8d, ebx
    imul    r8, r8, 24
    mov     [rdi + r8 + 0],  edx    ; kind
    mov     dword [rdi + r8 + 4], 0 ; pad
    mov     [rdi + r8 + 8],  r10    ; phys
    mov     [rdi + r8 + 16], r11    ; bytes
    inc     ebx

.desc_next:
    add     rsi, r9
    jmp     .desc_loop

.append_gop_range:
    ; Append explicit `kind = 2` (`WC GOP`) range for `[gop_base, fb_size]` (§8.2)
    mov     r8d, ebx
    imul    r8, r8, 24
    mov     dword [rdi + r8 + 0], 2
    mov     dword [rdi + r8 + 4], 0
    mov     rax, [r15 + EFI_BSS_HANDOFF_OFF + 16] ; gop_base
    mov     [rdi + r8 + 8], rax
    mov     rax, [rsp + 40]         ; fb_size
    add     rax, 0xFFF
    and     rax, ~0xFFF
    mov     [rdi + r8 + 16], rax
    inc     ebx

    mov     [r15 + EFI_BSS_HANDOFF_OFF + 80], ebx ; map_count
    mov     [r15 + EFI_BSS_HANDOFF_OFF + 88], rdi ; map physical pointer

    ; --- 4. Call `ExitBootServices(ImageHandle, MapKey)` ---
    mov     rcx, r12                ; ImageHandle
    mov     rdx, [rsp + 56]         ; MapKey
    mov     rax, [r14 + EFI_BS_EXIT_BOOT_SERVICES]
    call    rax
    test    rax, rax
    jnz     .mmap_retry             ; If key changed, re-fetch map & retry!

    ; --- 5. Firmware Is Gone: Switch to 64 KiB Loader Stack & Enter `.zr` ---
    cli
    mov     dword [r15 + EFI_BSS_ISR_LINE_OFF], 0xFFFFFFFF ; sentinel: no exception
    lea     rsp, [r15 + EFI_BSS_STACK_TOP_OFF] ; 0 mod 16 aligned!
    lea     rdi, [r15 + EFI_BSS_HANDOFF_OFF]   ; arg0 = &BootHandoff
    jmp     .enter_zr_main

.fail_return_firmware:
    mov     rsp, rbp
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rsi
    pop     rdi
    pop     rbp
    pop     rbx
    mov     rax, 0x8000000000000003 ; EFI_UNSUPPORTED
    ret

align 8, db 0
.gop_guid:
    dd 0x9042A9DE
    dw 0x23DC, 0x4A38
    db 0x96, 0xFB, 0x7A, 0xDE, 0xD0, 0x80, 0x51, 0x6A
.acpi20_guid:
    dd 0x8868E871
    dw 0xE4F1, 0x11D3
    db 0xBC, 0x22, 0x00, 0x80, 0xC7, 0x3C, 0x88, 0x81
.acpi10_guid:
    dd 0xEB9D2D30
    dw 0x2D88, 0x11D3
    db 0x9A, 0x16, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D

.enter_zr_main:
efi_harvest_stub_end:
