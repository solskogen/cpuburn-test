; Real-mode stub (flat binary, org 0, runs at 0x1000:0000, linear 0x10000). Padded to RM_SECTORS.
; Gathers BIOS info, enables A20 and validates RAM for the whole runtime (file + BSS),
; then reads the runtime from the boot disk one track per INT 13h call into a low
; bounce buffer and copies each chunk to its final address at 1 MiB from a brief
; 16-bit protected-mode window. Only the floppy capacity and RAM bound its size.
bits 16
org 0
%ifndef KERNEL_BYTES
%error KERNEL_BYTES must describe the linked runtime file
%endif
%ifndef KERNEL_END
%error KERNEL_END must include the linked runtime BSS
%endif

%define RM_SECTORS 8            ; size of this file; boot.asm loads it after the boot sector
%define KERNEL_LBA (1 + RM_SECTORS)
%define KERNEL_SECTORS ((KERNEL_BYTES + 511) / 512)
%define KERNEL_BASE 0x100000
%if KERNEL_BYTES < 1 || KERNEL_LBA + KERNEL_SECTORS > 2880
%error runtime file does not fit the 1.44 MB floppy
%endif
%define BOUNCE_SEG 0x1100       ; linear 0x11000; even a 63-sector track ends below the
                                ; 0x20000 DMA boundary
%define BPB_SPT 0x7C18          ; geometry boot.asm left in its BPB (queried for HDD-style boot)
%define BPB_HEADS 0x7C1A
%define BOOTINFO 0x600          ; linear: filled for the kernel
; BOOTINFO layout:
;  +0  dword magic 'CBI1'
;  +4  dword boot drive
;  +8  dword int15 88h KB (above 1M)
;  +12 dword int15 e801 KB (1M-16M)   (0 if n/a)
;  +16 dword int15 e801 64K blocks above 16M
;  +20 dword e820 entry count
;  +24 dword a20 enabled flag (after enable attempt)
;  +32 e820 entries: 24 bytes each (base64,len64,type32,acpi32), max 32 entries

entry:
    cli
    mov ax, cs
    mov ds, ax
    xor ax, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7000
    cld
    mov [drive], dl
    mov ax, [es:BPB_SPT]
    mov [spt], ax
    mov ax, [es:BPB_HEADS]
    mov [heads], ax
    push dx
    mov di, BOOTINFO
    mov cx, 800 / 2
    rep stosw
    pop dx
    movzx eax, dl
    mov [es:BOOTINFO+4], eax
    mov dword [es:BOOTINFO], 0x31494243

    int 0x12
    cmp ax, 576
    jae .enough_lowmem
    mov si, lowmem_error
    jmp entry.errchar
.enough_lowmem:
    ; known text mode for VGA output
    mov ax, 0x0003
    int 0x10

    ; memory: INT 15h AX=E801 / AH=88 / E820 (all best-effort)
    sti
    xor eax, eax
    mov ah, 0x88
    int 0x15
    jc .n88
    movzx eax, ax
    mov [es:BOOTINFO+8], eax
.n88:
    mov ax, 0xE801
    xor cx, cx
    xor dx, dx
    int 0x15
    jc .ne801
    test cx, cx
    jz .use_ax
    mov ax, cx
    mov bx, dx
.use_ax:
    movzx eax, ax
    movzx ebx, bx
    mov [es:BOOTINFO+12], eax
    mov [es:BOOTINFO+16], ebx
.ne801:
    ; E820
    xor ebx, ebx
    mov di, BOOTINFO+32
    xor si, si                      ; count
.e820:
    mov eax, 0xE820
    mov edx, 0x534D4150
    mov ecx, 24
    mov dword [es:di+20], 1
    int 0x15
    jc .e820done
    cmp eax, 0x534D4150
    jne .e820done
    cmp cl, 20
    jb .skip
    inc si
    add di, 24
    cmp si, 32
    jae .e820done
.skip:
    test ebx, ebx
    jnz .e820
.e820done:
    movzx esi, si
    mov [es:BOOTINFO+20], esi
    cli

    ; --- enable A20: BIOS, fast (0x92, never touching bit0), KBC; verify each step ---
    call a20_check
    jnz .a20ok
    mov ax, 0x2401
    int 0x15
    call a20_check
    jnz .a20ok
    in al, 0x92
    cmp al, 0xFF
    je .kbc
    test al, 2
    jnz .kbc
    and al, 0xFE
    or al, 2
    out 0x92, al
    call a20_check
    jnz .a20ok
.kbc:
    call kbc_wait
    jc .a20failed
    mov al, 0xD0
    out 0x64, al
    mov cx, 0xFFFF
.output_wait:
    in al, 0x64
    test al, 1
    jnz .output_ready
    loop .output_wait
    jmp .a20failed
.output_ready:
    in al, 0x60
    or al, 3                   ; A20 on; never assert active-low reset
    mov bl, al
    call kbc_wait
    jc .a20failed
    mov al, 0xD1
    out 0x64, al
    call kbc_wait
    jc .a20failed
    mov al, bl
    out 0x60, al
    call kbc_wait
.a20failed:
    call a20_check
.a20ok:
    mov eax, 0
    jz .a20no
    mov eax, 1
.a20no:
    mov [es:BOOTINFO+24], eax
    test eax, eax
    jnz .check_ram
    mov si, a20_error
.errchar:
    lodsb
    test al, al
    jz .stop
    out 0xe9, al
    mov ah, 0x0E
    mov bx, 7
    int 0x10
    jmp .errchar
.stop:
    cli
    hlt
    jmp .stop

    ; --- validate RAM for file + BSS in 32-bit protected mode before any copy ---
.check_ram:
    cli
    lgdt [gdtr]
    mov eax, cr0
    or al, 1
    mov cr0, eax
    jmp dword 0x08:pm_check + 0x10000

    ; --- back in real mode: load the runtime track by track ---
load_kernel:
    mov ax, cs
    mov ds, ax
    xor ax, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7000
    sti
.next:
    mov ax, [left]
    test ax, ax
    jz .done
    mov ax, [lba]                   ; LBA -> CHS
    xor dx, dx
    div word [spt]
    inc dx
    mov [sec], dl
    xor dx, dx
    div word [heads]
    mov [head], dl
    mov [cyl], ax
    mov ax, [spt]                   ; never cross a track: min(left, spt - sec + 1)
    movzx bx, byte [sec]
    sub ax, bx
    inc ax
    cmp ax, [left]
    jbe .count
    mov ax, [left]
.count:
    mov [cnt], ax
    mov byte [tries], 5
.retry:
    mov ax, BOUNCE_SEG
    mov es, ax
    xor bx, bx
    mov ax, [cyl]
    mov ch, al
    mov cl, ah
    shl cl, 6
    or cl, [sec]
    mov dh, [head]
    mov dl, [drive]
    mov al, [cnt]
    mov ah, 0x02
    int 0x13
    jnc .read
    xor ah, ah
    mov dl, [drive]
    int 0x13
    dec byte [tries]
    jnz .retry
    mov si, disk_error
    jmp entry.errchar
.read:
    xor ax, ax
    mov es, ax
    call a20_check                  ; a BIOS call must not have left A20 off
    jnz .copy
    mov si, a20_error
    jmp entry.errchar
.copy:
    call copy_chunk
    mov ax, [cnt]
    add [lba], ax
    sub [left], ax
    jmp .next
.done:
    cli
    ; stop floppy motor (BIOS timer ISR will no longer run)
    mov dx, 0x3F2
    mov al, 0x0C
    out dx, al
    mov eax, cr0
    or al, 1
    mov cr0, eax
    jmp dword 0x08:pm32 + 0x10000

; Copy [cnt] sectors from the bounce buffer to linear [dest] and advance [dest].
; Real mode in and out (IF=1 on return); the copy runs in 16-bit protected mode.
copy_chunk:
    movzx ecx, word [cnt]
    shl ecx, 7                      ; dwords
    mov edi, [dest]
    lea eax, [edi + ecx * 4]
    mov [dest], eax
    mov esi, BOUNCE_SEG * 16
    cld
    cli
    mov eax, cr0
    or al, 1
    mov cr0, eax
    jmp 0x18:.pm
.pm:
    mov ax, 0x10                    ; flat 4 GiB data
    mov ds, ax
    mov es, ax
    a32 rep movsd
    mov ax, 0x20                    ; 64 KiB limits again before real mode
    mov ds, ax
    mov es, ax
    mov eax, cr0
    and al, 0xFE
    mov cr0, eax
    jmp 0x1000:.rm
.rm:
    mov ax, cs
    mov ds, ax
    xor ax, ax
    mov es, ax
    sti
    ret

kbc_wait:
    mov cx, 0xFFFF
.w: in al, 0x64
    test al, 2
    jz .d
    loop .w
    stc
    ret
.d: clc
    ret

a20_error: db 'CPUBURN: A20 unavailable; halted safely.', 0
lowmem_error: db 'CPUBURN: 576 KiB conventional RAM required.', 0
disk_error: db 'CPUBURN: disk read error loading the runtime; halted safely.', 0

drive: db 0
sec:   db 0
head:  db 0
tries: db 0
spt:   dw 0
heads: dw 0
cyl:   dw 0
cnt:   dw 0
lba:   dw KERNEL_LBA
left:  dw KERNEL_SECTORS
dest:  dd KERNEL_BASE

; ZF=0 if A20 enabled (memory at 0:0x500 differs from FFFF:0x510)
a20_check:
    push ax
    push ds
    push es
    xor ax, ax
    mov ds, ax
    mov ax, 0xFFFF
    mov es, ax
    mov al, [ds:0x500]
    mov ah, [es:0x510]
    mov byte [ds:0x500], 0
    mov byte [es:0x510], 0xFF
    cmp byte [ds:0x500], 0xFF
    mov [es:0x510], ah
    mov [ds:0x500], al
    pop es
    pop ds
    pop ax
    ; equal => wrapped => A20 off => return ZF=1 ; not equal => ZF=0
    ret

align 8
gdt:
    dq 0
    dq 0x00CF9A000000FFFF   ; 0x08 code32 flat
    dq 0x00CF92000000FFFF   ; 0x10 data32 flat
    dq 0x00009A010000FFFF   ; 0x18 code16, base 0x10000 (this file)
    dq 0x000092000000FFFF   ; 0x20 data16, real-mode limits
gdtr:
    dw 5*8-1
    dd gdt + 0x10000

bits 32
pm_check:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov esp, 0x7000
    mov eax, KERNEL_END
    mov ebx, BOOTINFO
    call boot_ram_available
    test eax, eax
    jz ram_error
    jmp 0x18:to_real

pm32:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x90000
    jmp 0x08:KERNEL_BASE

ram_error:
    mov esi, ram_message + 0x10000
    mov edi, 0xb8000
    cmp word [0x463], 0x3b4
    jne .print
    mov edi, 0xb0000
.print:
    lodsb
    test al, al
    jz .halt
    out 0xe9, al
    mov ah, 0x4f
    stosw
    jmp .print
.halt:
    cli
    hlt
    jmp .halt

ram_message: db 'CPUBURN: runtime extended RAM unavailable; halted safely.', 0
%include "boot_memory.asm"

bits 16
to_real:                            ; 16-bit protected mode, then real mode
    mov ax, 0x20
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov eax, cr0
    and al, 0xFE
    mov cr0, eax
    jmp 0x1000:load_kernel

times RM_SECTORS*512-($-$$) db 0
