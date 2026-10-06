/*
 * Program entry. The kernel starts the main thread here with RSP = 8 mod 16
 * (as after a call) and the startup arguments in RDI, RSI, RDX.
 */

#include <jelly/os.h>

int main(uint64_t arg0, uint64_t arg1, uint64_t arg2);

__attribute__((noreturn, used)) void _start(uint64_t arg0, uint64_t arg1, uint64_t arg2)
{
    jelly_process_exit(main(arg0, arg1, arg2));
}
