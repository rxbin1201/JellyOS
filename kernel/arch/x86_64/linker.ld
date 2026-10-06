/*
 * JellyOS kernel linker script (x86_64).
 *
 * The kernel lives in the top 2 GiB (-mcmodel=kernel). Each PT_LOAD segment
 * starts on its own page so the boot manager can map it with exact rights:
 * text R-X, rodata R--, data RW-. See docs/architecture/memory-layout.md.
 */

OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

KERNEL_VMA = 0xFFFFFFFF80000000;

PHDRS
{
    text   PT_LOAD FLAGS(5);  /* R X */
    rodata PT_LOAD FLAGS(4);  /* R   */
    data   PT_LOAD FLAGS(6);  /* R W */
    note   PT_NOTE FLAGS(4);
}

SECTIONS
{
    . = KERNEL_VMA;
    __kernel_start = .;

    .text : {
        *(.text.entry)
        *(.text .text.*)
    } :text

    . = ALIGN(4096);
    .rodata : {
        *(.rodata .rodata.*)
    } :rodata
    .note.jellyos : {
        KEEP(*(.note.jellyos))
    } :rodata :note

    . = ALIGN(4096);
    .data : {
        *(.data .data.*)
    } :data
    .bss : {
        *(COMMON)
        *(.bss .bss.*)
    } :data

    . = ALIGN(4096);
    __kernel_end = .;

    /DISCARD/ : {
        *(.eh_frame*)
        *(.comment)
        *(.note.GNU-stack)
        *(.note.gnu.*)
    }
}
