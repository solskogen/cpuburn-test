/* BIOS stages the file at 0x11000; rm.asm copies it to extended RAM. */
OUTPUT_FORMAT(elf32-i386)
ENTRY(_start)
PHDRS
{
    code PT_LOAD FLAGS(5);
    data PT_LOAD FLAGS(6);
}
/* Firmware/loader storage, not allocated as part of the kernel image. */
bios_data_area = 0x400;
bootinfo = 0x600;
SECTIONS
{
    . = 0x100000;
    _kernel_start = .;
    .text : {
        *(.text.start)
        *(.text .text.*)
    } :code
    .rodata : { *(.rodata .rodata.*) } :code
    . = ALIGN(4096);
    .data : { *(.data .data.*) } :data
    _kernel_file_end = .;
    . = ALIGN(4);
    __bss_start = .;
    .bss : { *(.bss .bss.*) *(COMMON) } :data
    . = ALIGN(4096);
    _end = .;
    ASSERT(_end <= 0x1000000, "Kernel exceeds the identity-mapped RAM window")
    ASSERT(0x11000 + ((_kernel_file_end - _kernel_start + 511) / 512) * 512 <= 0x70000,
           "Kernel exceeds low-memory loader staging area")
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
