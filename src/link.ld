/* rm.asm reads the file from the floppy track by track and copies it to 1 MiB. */
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
    /* At 1 MiB the runtime cannot overlap the loader, stacks or low-memory fixtures. */
    ASSERT(_kernel_start == 0x100000, "Kernel must be linked at 1 MiB")
    ASSERT(_end <= 0x1000000, "Kernel exceeds the identity-mapped RAM window")
    /* RAM bound: keep the REQ_MEM8M arena window 0x400000-0x7FFFFF available
       (the arena's 64 KiB header follows _end). */
    ASSERT(_end <= 0x3F0000, "Kernel end + BSS exceeds the 0x3F0000 RAM bound")
    /* 2880 floppy sectors minus the boot sector and the 8-sector rm.asm loader. */
    ASSERT((_kernel_file_end - _kernel_start + 511) / 512 <= 2871,
           "Kernel file exceeds the 1.44 MB floppy")
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
