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

#define ASSERT(cond)                                                                 \
    do {                                                                             \
        if (!(cond))                                                                 \
            panic("assertion failed: %s (%s:%d)", #cond, __FILE__, __LINE__);         \
    } while (0)

#endif
