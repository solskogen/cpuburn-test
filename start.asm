; Kernel low-level: entry, ISR stubs, guard_call/recover, ring3 entry, DF task.
bits 32

extern kmain, c_isr, __bss_start, _end, df_handler_c, tss_main_esp_init
global _start, guard_call, guard_recover, ring3_call_asm, isr_stub, g_guard, df_task_entry
global load_gdt_idt

section .text.start
_start:
    cli
    cld
    mov esp, 0x90000
    mov edi, __bss_start
    mov ecx, _end
    sub ecx, edi
    shr ecx, 2
    xor eax, eax
    rep stosd
    call kmain
.halt:
    cli
    hlt
    jmp .halt

section .text

; int guard_call(void (*fn)(void))
guard_call:
    push ebp
    push ebx
    push esi
    push edi
    pushfd
    push dword [g_guard]
    mov [g_guard], esp
    mov eax, [esp + 28]
    call eax
    xor eax, eax
guard_ret:
    pop dword [g_guard]
    popfd
    pop edi
    pop esi
    pop ebx
    pop ebp
    ret

; void guard_recover(u32 code)  -- never returns
guard_recover:
    mov eax, [esp + 4]
    mov cx, 0x10
    mov ds, cx
    mov es, cx
    mov fs, cx
    mov gs, cx
    mov ss, cx
    mov edx, [g_guard]
    test edx, edx
    jz .nog
    mov esp, edx
    cld
    jmp guard_ret
.nog:
    cli
    hlt
    jmp .nog

; int ring3_call_asm(entry, stack_top) : called inside a guard frame by C wrapper (see ring3_tramp)
global ring3_tramp
extern g_r3_entry, g_r3_stack
ring3_tramp:
    mov ax, 0x23
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    push dword 0x23           ; ss
    push dword [g_r3_stack]   ; esp
    push dword 0x2            ; eflags (IF=0, IOPL=0)
    push dword 0x1B           ; cs
    push dword [g_r3_entry]   ; eip
    iretd
ring3_call_asm: ret

; ---- ISR stubs: one per vector 0..255 ----
%macro STUB 1
isr_%1:
%if (%1 == 8) || (%1 == 10) || (%1 == 11) || (%1 == 12) || (%1 == 13) || (%1 == 14) || (%1 == 17) || (%1 == 21) || (%1 == 29) || (%1 == 30)
%else
    push dword 0
%endif
    push dword %1
    jmp isr_common
%endmacro

%assign i 0
%rep 256
STUB i
%assign i i+1
%endrep

isr_common:
    pushad
    push ds
    push es
    push fs
    push gs
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    cld
    mov eax, esp
    push eax
    call c_isr
    add esp, 4
    pop gs
    pop fs
    pop es
    pop ds
    popad
    add esp, 8
    iretd

section .data
align 4
isr_stub:
%assign i 0
%rep 256
    dd isr_ %+ i
%assign i i+1
%endrep

section .text
; Double-fault task entry (runs on its own TSS/stack): print and halt.
df_task_entry:
    cld
    call df_handler_c
.h: cli
    hlt
    jmp .h

section .bss
g_guard: resd 1
