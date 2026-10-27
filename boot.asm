; Boot sector: loads STAGE2_SECTORS sectors from the boot drive to 0x1000:0000 (linear 0x10000), jumps there.
; Read-only use of the disk. Works for floppy and for BIOSes that present the image as a floppy/HDD (DL passed through).
bits 16
org 0x7C00

%ifndef STAGE2_SECTORS
%define STAGE2_SECTORS 64
%endif

start:
    jmp short entry
    nop
    db 'CPUBURN '            ; OEM
    dw 512                   ; bytes/sector
    db 1                     ; sectors/cluster
    dw 1                     ; reserved
    db 2                     ; FATs
    dw 224                   ; root entries
    dw 2880                  ; total sectors
    db 0xF0                  ; media
    dw 9                     ; sectors/FAT
spt:    dw 18                ; sectors/track
heads:  dw 2
    dd 0                     ; hidden
    dd 0
    db 0, 0, 0x29
    dd 0x42424242
    db 'CPUBURN    '
    db 'FAT12   '

entry:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti
    cld
    mov [drive], dl

    ; fetch geometry (ignore failures, keep BPB defaults)
    mov ah, 0x08
    xor di, di
    int 0x13
    jc .nogeo
    mov al, cl
    and al, 0x3F
    jz .nogeo
    xor ah, ah
    mov [spt], ax
    mov al, dh
    inc al
    xor ah, ah
    mov [heads], ax
.nogeo:
    mov dl, [drive]
    xor ah, ah
    int 0x13

    mov word [lba], 1                ; next LBA to read (sector 0 is us)
    mov word [left], STAGE2_SECTORS
    mov word [lseg], 0x1000

.next:
    mov ax, [left]
    test ax, ax
    jz .done

    ; LBA -> CHS
    mov ax, [lba]
    xor dx, dx
    div word [spt]                   ; ax = lba / spt, dx = lba % spt
    inc dx                           ; sector (1-based)
    mov [sec], dl
    xor dx, dx
    div word [heads]                 ; ax = cyl, dx = head
    mov [head], dl
    mov [cyl], ax

    ; count = min(left, spt - sec + 1, room until 64K boundary)
    mov ax, [spt]
    movzx bx, byte [sec]
    sub ax, bx
    inc ax
    mov cx, [left]
    cmp ax, cx
    jbe .c1
    mov ax, cx
.c1:
    mov bx, [lseg]
    shl bx, 4                        ; linear offset within 64K (segment multiple of 0x20 keeps this exact)
    mov cx, 0
    sub cx, bx                       ; bytes to boundary (0 => full 64K)
    jnz .c2
    mov cx, 0xFFFF
.c2:
    shr cx, 9                        ; sectors that fit
    jnz .c3
    mov cx, 1
.c3:
    cmp ax, cx
    jbe .c4
    mov ax, cx
.c4:
    mov [cnt], ax

    mov byte [tries], 5
.retry:
    mov ax, [lseg]
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
    jnc .ok
    xor ah, ah
    mov dl, [drive]
    int 0x13
    dec byte [tries]
    jnz .retry
    mov si, msg_err
    call print
.hang:
    cli
    hlt
    jmp .hang
.ok:
    mov ax, [cnt]
    add [lba], ax
    sub [left], ax
    shl ax, 5                        ; sectors * 32 paragraphs
    add [lseg], ax
    jmp .next

.done:
    xor ax, ax
    mov es, ax
    mov dl, [drive]
    jmp 0x1000:0x0000

print:
    lodsb
    test al, al
    jz .r
    mov ah, 0x0E
    mov bx, 7
    int 0x10
    jmp print
.r: ret

msg_err: db 'cpuburn: disk read error', 13, 10, 0
drive: db 0
sec:   db 0
head:  db 0
tries: db 0
cyl:   dw 0
lba:   dw 0
left:  dw 0
cnt:   dw 0
lseg:  dw 0

times 510-($-$$) db 0
dw 0xAA55
