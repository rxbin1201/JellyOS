/*
 * The kernel log as a file (drivers/console/kmsg.c).
 */

#ifndef DRIVERS_CONSOLE_KMSG_H
#define DRIVERS_CONSOLE_KMSG_H

/*
 * With "logfile=PATH" on the kernel command line: write the log to that file now and return when it is there.
 * For code that is about to do something after which nothing may be readable any more (thread context only).
 * Without the option it does nothing.
 */
void kmsg_logfile_write(void);

#endif
