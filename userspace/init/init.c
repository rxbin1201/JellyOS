/*
 * init: the first user process (README section 29).
 *
 * The kernel starts /init from the initramfs with /dev/console as stdin,
 * stdout and stderr and marks it critical: if init ever exits, the kernel
 * panics. init
 *   1. checks the file system skeleton (/dev is mounted by the kernel, /tmp
 *      is part of the root ramfs) and creates missing directories,
 *   2. reads the kernel command line from JELLY_CMDLINE,
 *   3. starts the service manager, which starts everything else, and
 *      restarts it if it dies,
 *   4. in recovery mode (recovery=1) skips all services and keeps a rescue
 *      shell running on the console.
 * safe_mode=1 is handed to the service manager, which then only starts
 * services marked essential.
 */

#include <errno.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jelly/os.h>

#define SERVICE_MANAGER     "/sbin/servicemanager"
#define RESCUE_SHELL        "/bin/sh"
#define RESTART_WINDOW_NS   (10ULL * 1000000000ULL)
#define RESTART_LIMIT       5

/* Is `option` (e.g. "recovery=1") one of the words of the command line? */
static int cmdline_has(const char *cmdline, const char *option)
{
    size_t length = strlen(option);
    for (const char *p = cmdline; (p = strstr(p, option)); p += length) {
        int starts = p == cmdline || p[-1] == ' ';
        int ends = p[length] == '\0' || p[length] == ' ';
        if (starts && ends)
            return 1;
    }
    return 0;
}

static void ensure_directory(const char *path, unsigned mode)
{
    status_t status = jelly_mkdir(path, mode);
    if (status == STATUS_SUCCESS)
        printf("init: created %s\n", path);
    else if (status != STATUS_ALREADY_EXISTS)
        printf("init: cannot create %s: %s\n", path, strerror((int)status));
}

static void prepare_file_systems(void)
{
    jelly_stat_t stat;

    ensure_directory("/tmp", 0777);
    ensure_directory("/etc", 0755);
    if (STATUS_IS_ERROR(jelly_stat("/dev/console", 0, &stat)))
        printf("init: warning: /dev/console is missing (devfs not mounted?)\n");
}

/* Keep a rescue shell on the console forever. */
static __attribute__((noreturn)) void rescue_shell(void)
{
    char *argv[] = { "sh", NULL };
    for (;;) {
        printf("init: starting rescue shell\n");
        int code = process_run(RESCUE_SHELL, argv, NULL);
        if (code < 0) {
            printf("init: cannot start %s: %s\n", RESCUE_SHELL, strerror(errno));
            jelly_thread_sleep(5000000000ULL);
        } else {
            printf("init: rescue shell exited with code %d\n", code);
        }
    }
}

int main(int argc, char **argv, char **envp)
{
    (void)argc;
    (void)argv;
    (void)envp;

    const char *cmdline = getenv("JELLY_CMDLINE");
    if (!cmdline)
        cmdline = "";
    int recovery = cmdline_has(cmdline, "recovery=1");
    int safe_mode = cmdline_has(cmdline, "safe_mode=1");

    printf("init: JellyOS init (process environment ready, ABI %u)\n", jelly_abi_version());
    prepare_file_systems();
    setenv("PATH", "/bin:/sbin", 1);

    if (recovery) {
        printf("init: recovery mode, no services are started\n");
        rescue_shell();
    }

    char *manager_argv[] = { "servicemanager", safe_mode ? "--safe-mode" : NULL, NULL };
    uint64_t window_start = jelly_clock_ns();
    int restarts = 0;

    for (;;) {
        jelly_handle_t manager;
        int code;

        if (process_spawn(SERVICE_MANAGER, manager_argv, NULL, &manager)) {
            printf("init: cannot start %s: %s\n", SERVICE_MANAGER, strerror(errno));
            rescue_shell();
        }
        if (process_wait(manager, &code))
            code = -1;
        jelly_handle_close(manager);
        printf("init: service manager exited with code %d\n", code);

        /* Too many failures in a short time: give the operator a shell. */
        uint64_t now = jelly_clock_ns();
        if (now - window_start > RESTART_WINDOW_NS) {
            window_start = now;
            restarts = 0;
        }
        if (++restarts >= RESTART_LIMIT) {
            printf("init: service manager keeps failing, falling back to a rescue shell\n");
            rescue_shell();
        }
        jelly_thread_sleep(500000000ULL);
        printf("init: restarting the service manager\n");
    }
}
