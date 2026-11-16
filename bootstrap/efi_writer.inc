; =============================================================================
; Zero Stage-0 Bootstrap — UEFI Application Container (`bootstrap/efi_writer.inc`)
; =============================================================================
; Finalizes an x86-64 UEFI Application image (`Subsystem = 10`), patches all
; RIP-relative `.bss` displacements, and appends the minimal 12-byte `.reloc`
; directory required by EDK2 `PeCoffLoaderLoadImage`.
; =============================================================================

finalize_efi_image:
    push    rbx
    push    r12
    push    r13
    push    r14
    push    r15

    mov     r15, [space_out]

    ; 1. Compute `.text`, `.reloc`, and `.bss` geometry
    mov     eax, [code_off]
    sub     eax, PE_HDR_FILE_SIZE   ; eax = text_virt_size
    mov     r12d, eax

    lea     ebx, [rax + 0x1FF]
    and     ebx, ~0x1FF             ; ebx = text_raw_size (512B aligned)

    ; Zero-pad `.text` tail up to `PE_HDR_FILE_SIZE + ebx`
    mov     edi, [code_off]
    lea     edx, [rbx + PE_HDR_FILE_SIZE]
.pad_text:
    cmp     edi, edx
    jae     .geom_rva
    mov     byte [r15 + rdi], 0
    inc     edi
    jmp     .pad_text

.geom_rva:
    lea     ecx, [r12 + 0xFFF]
    and     ecx, ~0xFFF             ; text_rva_span (4KiB aligned)
    mov     edx, [efi_code_span_patch_off]
    mov     [r15 + rdx], ecx        ; patch BootHandoff.code_bytes!
    lea     r13d, [rcx + PE_TEXT_RVA] ; r13d = reloc_rva
    lea     r14d, [r13 + 0x1000]    ; r14d = bss_rva!

    ; 2. Patch all RIP-relative `.bss` displacements (`target_rva - next_rip_rva`)
    mov     rsi, [space_fixup]
    xor     ecx, ecx
    mov     edx, [fixup_cnt]
.patch_bss:
    cmp     ecx, edx
    jae     .emit_reloc_sec
    mov     edi, [rsi + rcx * 8]        ; patch_file_off
    movsxd  rax, dword [rsi + rcx * 8 + 4] ; signed bss_off
    add     eax, r14d                   ; target_rva = bss_rva + bss_off
    lea     r8d, [rdi + 4 - PE_HDR_FILE_SIZE + PE_TEXT_RVA] ; next_rip_rva
    sub     eax, r8d
    mov     [r15 + rdi], eax
    inc     ecx
    jmp     .patch_bss

    ; 3. Write 512-byte `.reloc` section at `PE_HDR_FILE_SIZE + text_raw_size`
.emit_reloc_sec:
    lea     edi, [rbx + PE_HDR_FILE_SIZE] ; reloc_file_off
    push    rdi
    add     rdi, r15
    xor     eax, eax
    mov     ecx, 64                 ; zero 512 bytes (64 qwords)
    rep     stosq
    pop     rdi

    ; 12-byte empty relocation block: `{ PageRVA = 0x1000, BlockSize = 12, Pad = 0 }`
    mov     dword [r15 + rdi + 0], PE_TEXT_RVA
    mov     dword [r15 + rdi + 4], 12
    mov     dword [r15 + rdi + 8], 0

    lea     eax, [rdi + 0x200]
    mov     [total_out], eax        ; total `.efi` file size on disk!

    ; 4. Stamp 512-byte UEFI Application Header (`0x000 .. 0x1FF`)
    mov     rdi, r15
    xor     eax, eax
    mov     ecx, 64                 ; zero first 512 bytes
    rep     stosq

    ; Header pointer offset (`0x00` signature + `0x3C` header offset = `0x40`)
    mov     word  [r15 + 0x00], 0x5A4D
    mov     dword [r15 + 0x3C], 0x40

    ; COFF File Header (`0x40 .. 0x57`)
    mov     dword [r15 + 0x40], 0x00004550  ; Signature
    mov     word  [r15 + 0x44], 0x8664      ; Machine = x86_64
    mov     word  [r15 + 0x46], 3           ; NumberOfSections = 3 (.text, .reloc, .bss)
    mov     word  [r15 + 0x54], 240         ; SizeOfOptionalHeader
    mov     word  [r15 + 0x56], 0x0022      ; EXECUTABLE_IMAGE | LARGE_ADDRESS_AWARE

    ; Optional Header (`0x58 .. 0x147`)
    mov     rax, [data_bss_off]
    add     rax, 0xFFF
    and     rax, ~0xFFF             ; rax = aligned bss_virt_size

    mov     word  [r15 + 0x58 + 0x00], 0x020B       ; PE32+ Magic
    mov     byte  [r15 + 0x58 + 0x02], 1
    mov     dword [r15 + 0x58 + 0x04], ebx          ; SizeOfCode
    mov     dword [r15 + 0x58 + 0x08], 0x200        ; SizeOfInitializedData
    mov     dword [r15 + 0x58 + 0x0C], eax          ; SizeOfUninitializedData
    mov     dword [r15 + 0x58 + 0x10], PE_TEXT_RVA  ; AddressOfEntryPoint = 0x1000
    mov     dword [r15 + 0x58 + 0x14], PE_TEXT_RVA  ; BaseOfCode
    mov     qword [r15 + 0x58 + 0x18], 0x00400000   ; Preferred ImageBase
    mov     dword [r15 + 0x58 + 0x20], 0x1000       ; SectionAlignment
    mov     dword [r15 + 0x58 + 0x24], 0x0200       ; FileAlignment

    lea     edx, [r14 + rax]                        ; SizeOfImage = bss_rva + bss_virt_size
    mov     dword [r15 + 0x58 + 0x38], edx
    mov     dword [r15 + 0x58 + 0x3C], PE_HDR_FILE_SIZE ; SizeOfHeaders = 0x200
    mov     word  [r15 + 0x58 + 0x44], 10           ; Subsystem = EFI_APPLICATION (10)
    mov     qword [r15 + 0x58 + 0x48], 0x100000     ; SizeOfStackReserve
    mov     qword [r15 + 0x58 + 0x50], 0x10000      ; SizeOfStackCommit
    mov     qword [r15 + 0x58 + 0x58], 0x100000     ; SizeOfHeapReserve
    mov     qword [r15 + 0x58 + 0x60], 0x1000       ; SizeOfHeapCommit
    mov     dword [r15 + 0x58 + 0x6C], 16           ; NumberOfRvaAndSizes = 16

    ; DataDirectory[5] = Base Relocation Table (`0x58 + 0x70 + 40 = 0xF0`)
    mov     dword [r15 + 0xF0 + 0], r13d            ; VirtualAddress = reloc_rva
    mov     dword [r15 + 0xF0 + 4], 12              ; Size = 12 bytes

    ; Section 0 (`.text` at `0x148`, 40 bytes)
    mov     dword [r15 + 0x148 + 0],  0x7865742E    ; ".tex"
    mov     byte  [r15 + 0x148 + 4],  't'
    mov     dword [r15 + 0x148 + 8],  r12d          ; VirtualSize
    mov     dword [r15 + 0x148 + 12], PE_TEXT_RVA   ; VirtualAddress = 0x1000
    mov     dword [r15 + 0x148 + 16], ebx           ; SizeOfRawData
    mov     dword [r15 + 0x148 + 20], PE_HDR_FILE_SIZE ; PointerToRawData = 0x200
    mov     dword [r15 + 0x148 + 36], 0x60000020    ; CNT_CODE | MEM_EXECUTE | MEM_READ

    ; Section 1 (`.reloc` at `0x170`, 40 bytes)
    mov     dword [r15 + 0x170 + 0],  0x6C65722E    ; ".rel"
    mov     word  [r15 + 0x170 + 4],  0x636F        ; "oc"
    mov     dword [r15 + 0x170 + 8],  12            ; VirtualSize = 12
    mov     dword [r15 + 0x170 + 12], r13d          ; VirtualAddress = reloc_rva
    mov     dword [r15 + 0x170 + 16], 0x200         ; SizeOfRawData = 512
    lea     edx, [rbx + PE_HDR_FILE_SIZE]
    mov     dword [r15 + 0x170 + 20], edx           ; PointerToRawData
    mov     dword [r15 + 0x170 + 36], 0x42000040    ; INITIALIZED_DATA | DISCARDABLE | READ

    ; Section 2 (`.bss` at `0x198`, 40 bytes)
    mov     dword [r15 + 0x198 + 0],  0x7373622E    ; ".bss"
    mov     dword [r15 + 0x198 + 8],  eax           ; VirtualSize = aligned bss size
    mov     dword [r15 + 0x198 + 12], r14d          ; VirtualAddress = bss_rva
    mov     dword [r15 + 0x198 + 36], 0xC0000080    ; UNINITIALIZED_DATA | READ | WRITE

    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbx
    ret
