#ifndef ARCH_X86_64_PIC_H
#define ARCH_X86_64_PIC_H

/* Remap the legacy 8259 PICs to VECTOR_PIC_BASE and mask all lines. */
void pic_disable(void);

#endif
