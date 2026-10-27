/* Kernel image: loaded by rm.asm at 0x11000 (4 KiB after the real-mode stub). */
OUTPUT_FORMAT(elf32-i386)
ENTRY(_start)
SECTIONS
{
    . = 0x11000;
    .text : {
        *(.text.start)
        *(.text .text.*)
    }
    .rodata : { *(.rodata .rodata.*) }
    .data : { *(.data .data.*) }
    . = ALIGN(4);
    __bss_start = .;
    .bss : { *(.bss .bss.*) *(COMMON) }
    . = ALIGN(4096);
    _end = .;
    ASSERT(_end < 0x70000, "Kernel overlaps reserved V86 scratch / stack")
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
