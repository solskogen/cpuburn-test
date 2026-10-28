; 32-bit bootstrap RAM check, also assembled into the native regression fixture.
; EAX = exclusive kernel end, EBX = bootinfo; EAX returns 1 for owned RAM.
; A usable E820 chain must cover [1 MiB, end); enabled reservations override it.
bits 32
boot_ram_available:
    push ebx
    push esi
    push edi
    push ebp
    mov ebp, eax
    cmp ebp, 0x100000
    jbe .bad
    cmp ebp, 0x1000000
    ja .bad
    mov ecx, [ebx + 20]
    test ecx, ecx
    jz .legacy
    cmp ecx, 32
    ja .bad
    mov edi, 0x100000
.pass:
    push edi
    lea esi, [ebx + 32]
    mov ecx, [ebx + 20]
.record:
    test dword [esi + 20], 1
    jz .next
    cmp dword [esi + 4], 0
    jne .next
    mov eax, [esi]
    mov edx, [esi + 8]
    add edx, eax
    jc .carry
    cmp dword [esi + 12], 0
    jne .saturate
    jmp .range
.carry:
    cmp dword [esi + 12], 0xffffffff
    jne .saturate
    pop edi
    jmp .bad
.saturate:
    mov edx, 0xffffffff
.range:
    cmp edx, eax
    jbe .next
    cmp dword [esi + 16], 1
    je .usable
    cmp eax, ebp
    jae .next
    cmp edx, 0x100000
    jbe .next
    pop edi
    jmp .bad
.usable:
    cmp eax, edi
    ja .next
    cmp edx, edi
    jbe .next
    mov edi, edx
.next:
    add esi, 24
    loop .record
    pop eax
    cmp edi, ebp
    jae .good
    cmp edi, eax
    ja .pass
    jmp .bad
.legacy:
    mov eax, [ebx + 12]
    test eax, eax
    jnz .size
    mov eax, [ebx + 8]
.size:
    ; Only the contiguous first 15 MiB is needed; do not rely on high blocks.
    cmp eax, 15360
    jbe .bounded
    mov eax, 15360
.bounded:
    shl eax, 10
    add eax, 0x100000
    cmp eax, ebp
    jae .good
.bad:
    xor eax, eax
    jmp .done
.good:
    mov eax, 1
.done:
    pop ebp
    pop edi
    pop esi
    pop ebx
    ret
