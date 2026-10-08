/*
 * Kernel panic and assertions (README sections 45 and 51).
 */

#ifndef CORE_PANIC_H
#define CORE_PANIC_H

struct arch_interrupt_frame;

/* Stop the system with a reason, register state and stack trace. Never returns. */
__attribute__((noreturn, format(printf, 1, 2)))
void panic(const char *fmt, ...);

/* Panic caused by a CPU exception; frame holds the interrupted state. */
__attribute__((noreturn))
void panic_with_frame(const struct arch_interrupt_frame *frame, const char *reason);

/*
 * Whoever has the screen (the display layer) says how a panic gets onto it: `prepare` runs before the panic's
 * text is written, `show` after all of it has gone to the serial port. Both run with interrupts off on a
 * kernel that is about to halt: no locks, no sleeping, no memory allocation.
 */
void panic_set_screen(void (*prepare)(void), void (*show)(void));

#define ASSERT(cond)                                                                 \
    do {                                                                             \
        if (!(cond))                                                                 \
            panic("assertion failed: %s (%s:%d)", #cond, __FILE__, __LINE__);         \
    } while (0)

#endif
