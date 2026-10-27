; Real-mode stub (flat binary, org 0, runs at 0x1000:0000, linear 0x10000). Padded to 4 KiB.
; Gathers BIOS info, enables A20, silences floppy motor, switches to 32-bit flat protected mode and jumps to kernel at 0x11000.
bits 16
org 0

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
    ; stop floppy motor (BIOS timer ISR will no longer run)
    mov dx, 0x3F2
    mov al, 0x0C
    out dx, al

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
    jnz .enter_pm
    mov si, a20_error
.errchar:
    lodsb
    test al, al
    jz .stop
    mov ah, 0x0E
    mov bx, 7
    int 0x10
    jmp .errchar
.stop:
    cli
    hlt
    jmp .stop

    ; --- protected mode ---
.enter_pm:
    cli
    lgdt [gdtr]
    mov eax, cr0
    or al, 1
    mov cr0, eax
    jmp dword 0x08:pm32 + 0x10000

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
gdtr:
    dw 3*8-1
    dd gdt + 0x10000

bits 32
pm32:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x90000
    jmp 0x08:0x11000

times 4096-($-$$) db 0
