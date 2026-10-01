[org 0x7C00]
[bits 16]

; The BIOS boot sector executes at 0x7C00. Do NOT load the kernel over it.
; Load the kernel at physical 0x10000 instead.
KERNEL_PHYS      equ 0x10000
KERNEL_SECTORS   equ 64

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    mov [boot_drive], dl

    ; Use INT 13h Extensions (LBA). QEMU/SeaBIOS supports this path.
    ; Read LBA 1..32 -> physical 0x10000
    ; Read LBA 33..64 -> physical 0x14000
    mov dl, [boot_drive]

    mov word [dap_sector_count], 32
    mov word [dap_buffer_offset], 0
    mov word [dap_buffer_segment], 0x1000       ; 0x1000:0000 = 0x10000
    mov word [dap_lba_low], 1
    mov word [dap_lba_low + 2], 0
    mov word [dap_lba_low + 4], 0
    mov word [dap_lba_low + 6], 0
    mov word [dap_lba_high], 0
    mov word [dap_lba_high + 2], 0
    mov word [dap_lba_high + 4], 0
    mov word [dap_lba_high + 6], 0
    mov si, dap
    mov ah, 0x42
    int 0x13
    jc disk_error

    mov word [dap_buffer_segment], 0x1400       ; 0x1400:0000 = 0x14000
    mov word [dap_lba_low], 33
    mov si, dap
    mov ah, 0x42
    int 0x13
    jc disk_error

    ; Enter 32-bit protected mode.
    cli
    lgdt [gdt_descriptor]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp 0x08:protected_mode

disk_error:
    mov ax, 0x0003
    int 0x10
    mov si, disk_error_msg
.print:
    lodsb
    test al, al
    jz .hang
    mov ah, 0x0E
    int 0x10
    jmp .print
.hang:
    cli
    hlt
    jmp .hang

[bits 32]
protected_mode:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x90000

    ; The kernel is linked at 0x10000, so jump to its physical address.
    jmp KERNEL_PHYS

gdt:
    dq 0
    dw 0xFFFF
    dw 0
    db 0
    db 10011010b
    db 11001111b
    db 0
    dw 0xFFFF
    dw 0
    db 0
    db 10010010b
    db 11001111b
    db 0
gdt_end:

gdt_descriptor:
    dw gdt_end - gdt - 1
    dd gdt

align 4
dap:
    db 0x10
    db 0
dap_sector_count:
    dw 0
dap_buffer_offset:
    dw 0
dap_buffer_segment:
    dw 0
dap_lba_low:
    dd 0
dap_lba_high:
    dd 0

boot_drive db 0
disk_error_msg db 'JupiterOS: disk error', 0

times 510 - ($ - $$) db 0
dw 0xAA55
