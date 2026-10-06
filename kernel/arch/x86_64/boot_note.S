/*
 * JellyOS boot protocol note (see boot/protocol/jelly/boot_info.h).
 *
 * Tells the boot manager which boot_info_t version this kernel requires.
 * Layout: elf64_note_t header, name "JellyOS", boot_note_protocol_t.
 */

#define BOOT_NOTE_TYPE_PROTOCOL 1
#define REQUIRED_BOOT_VERSION   1

    .section .note.jellyos, "a", @note
    .balign 4
    .long   2f - 1f                 /* namesz */
    .long   4f - 3f                 /* descsz */
    .long   BOOT_NOTE_TYPE_PROTOCOL /* type */
1:  .asciz  "JellyOS"
2:  .balign 4
3:  .long   REQUIRED_BOOT_VERSION   /* required_version */
    .long   0                       /* reserved */
4:  .balign 4

    .section .note.GNU-stack, "", @progbits
