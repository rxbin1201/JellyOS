/*
 * servicemanager: starts and supervises services (README section 31).
 *
 * Services are described in /etc/services.conf:
 *
 *     [service shell]
 *     description=Interactive shell on the console
 *     command=/bin/sh
 *     type=simple          simple: runs until stopped | oneshot: runs once to completion
 *     restart=always       always | on-failure | never
 *     depends=motd         space-separated names, started (oneshot: completed) first
 *     control=yes          pass a control channel as startup handle 3
 *     essential=yes        started in safe mode
 *
 * Failure detection: an exited service is restarted according to its policy
 * with a growing delay; after RESTART_LIMIT quick failures it is marked
 * failed. Dependents of a failed service are not started.
 *
 * Control protocol (text messages on the control channel, one reply each):
 *     list | status NAME | start NAME | stop NAME | restart NAME
 *
 * The manager sleeps in SYS_OBJECT_WAIT_MANY on all running service
 * processes and control channels, until the next pending restart at the
 * latest; it never polls.
 */

#include <ctype.h>
#include <errno.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jelly/os.h>

#define CONFIG_PATH      "/etc/services.conf"
#define MAX_SERVICES     32
#define MAX_DEPENDS      8
#define MAX_ARGS         16
#define BACKOFF_MIN_NS   250000000ULL     /* 250 ms, doubled per quick failure */
#define BACKOFF_MAX_NS   8000000000ULL
#define STABLE_NS        10000000000ULL   /* running this long resets the failure count */
#define RESTART_LIMIT    5
#define STOP_EXIT_CODE   143              /* reported for services stopped by request */
#define MESSAGE_MAX      4096

typedef enum { TYPE_SIMPLE, TYPE_ONESHOT } service_type_t;
typedef enum { RESTART_NEVER, RESTART_ON_FAILURE, RESTART_ALWAYS } restart_policy_t;
typedef enum {
    STATE_STOPPED,   /* not running; started when wanted and dependencies are ready */
    STATE_RUNNING,
    STATE_DONE,      /* oneshot finished successfully */
    STATE_WAITING,   /* waiting for a restart delay */
    STATE_FAILED,    /* gave up, or a dependency failed */
} service_state_t;

typedef struct {
    char             name[32];
    char             description[96];
    char             command[256];
    service_type_t   type;
    restart_policy_t restart;
    char             depends[MAX_DEPENDS][32];
    int              depend_count;
    int              control;
    int              essential;

    service_state_t  state;
    int              wanted;        /* start (again) when possible */
    jelly_handle_t   process;
    jelly_handle_t   channel;       /* our end of the control channel */
    uint64_t         pid;
    int              last_exit;
    int              failures;      /* quick failures in a row */
    uint64_t         started_ns;
    uint64_t         restart_at_ns;
    char             reason[64];
} service_t;

static service_t services[MAX_SERVICES];
static int service_count;
static int safe_mode;

/* --- Logging --------------------------------------------------------------------- */

static void log_message(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void log_message(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    printf("servicemanager: ");
    vprintf(format, args);
    printf("\n");
    va_end(args);
}

static const char *state_name(service_state_t state)
{
    switch (state) {
    case STATE_STOPPED: return "stopped";
    case STATE_RUNNING: return "running";
    case STATE_DONE:    return "done";
    case STATE_WAITING: return "waiting";
    case STATE_FAILED:  return "failed";
    }
    return "?";
}

/* --- Configuration -------------------------------------------------------------- */

static char *trim(char *s)
{
    while (isspace((unsigned char)*s))
        s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1]))
        *--end = '\0';
    return s;
}

static int is_yes(const char *value)
{
    return !strcmp(value, "yes") || !strcmp(value, "true") || !strcmp(value, "1");
}

static void set_option(service_t *s, const char *key, char *value, int line)
{
    if (!strcmp(key, "description")) {
        snprintf(s->description, sizeof(s->description), "%s", value);
    } else if (!strcmp(key, "command")) {
        snprintf(s->command, sizeof(s->command), "%s", value);
    } else if (!strcmp(key, "type")) {
        if (!strcmp(value, "simple"))
            s->type = TYPE_SIMPLE;
        else if (!strcmp(value, "oneshot"))
            s->type = TYPE_ONESHOT;
        else
            log_message("%s:%d: unknown type '%s'", CONFIG_PATH, line, value);
    } else if (!strcmp(key, "restart")) {
        if (!strcmp(value, "always"))
            s->restart = RESTART_ALWAYS;
        else if (!strcmp(value, "on-failure"))
            s->restart = RESTART_ON_FAILURE;
        else if (!strcmp(value, "never"))
            s->restart = RESTART_NEVER;
        else
            log_message("%s:%d: unknown restart policy '%s'", CONFIG_PATH, line, value);
    } else if (!strcmp(key, "depends")) {
        char *state;
        for (char *name = strtok_r(value, " \t,", &state); name; name = strtok_r(NULL, " \t,", &state)) {
            if (s->depend_count == MAX_DEPENDS) {
                log_message("%s:%d: too many dependencies", CONFIG_PATH, line);
                break;
            }
            snprintf(s->depends[s->depend_count++], sizeof(s->depends[0]), "%s", name);
        }
    } else if (!strcmp(key, "control")) {
        s->control = is_yes(value);
    } else if (!strcmp(key, "essential")) {
        s->essential = is_yes(value);
    } else {
        log_message("%s:%d: unknown key '%s'", CONFIG_PATH, line, key);
    }
}

static int load_config(void)
{
    FILE *file = fopen(CONFIG_PATH, "r");
    char buffer[512];
    service_t *current = NULL;
    int line = 0;

    if (!file) {
        log_message("cannot open %s: %s", CONFIG_PATH, strerror(errno));
        return -1;
    }
    while (fgets(buffer, sizeof(buffer), file)) {
        line++;
        char *text = trim(buffer);
        if (!*text || *text == '#' || *text == ';')
            continue;

        if (*text == '[') {
            char *end = strchr(text, ']');
            current = NULL;
            if (!end || strncmp(text, "[service ", 9)) {
                log_message("%s:%d: expected [service NAME]", CONFIG_PATH, line);
                continue;
            }
            *end = '\0';
            if (service_count == MAX_SERVICES) {
                log_message("%s:%d: too many services", CONFIG_PATH, line);
                continue;
            }
            current = &services[service_count++];
            memset(current, 0, sizeof(*current));
            snprintf(current->name, sizeof(current->name), "%s", trim(text + 9));
            current->restart = RESTART_ON_FAILURE;
            continue;
        }

        char *equals = strchr(text, '=');
        if (!equals || !current) {
            log_message("%s:%d: ignored", CONFIG_PATH, line);
            continue;
        }
        *equals = '\0';
        set_option(current, trim(text), trim(equals + 1), line);
    }
    fclose(file);
    return 0;
}

static service_t *find_service(const char *name)
{
    for (int i = 0; i < service_count; i++) {
        if (!strcmp(services[i].name, name))
            return &services[i];
    }
    return NULL;
}

/* Depth-first search for cycles; marks: 0 new, 1 on the path, 2 checked. */
static int has_cycle(service_t *s, char *marks)
{
    int index = (int)(s - services);
    if (marks[index] == 1)
        return 1;
    if (marks[index] == 2)
        return 0;
    marks[index] = 1;
    for (int d = 0; d < s->depend_count; d++) {
        service_t *dependency = find_service(s->depends[d]);
        if (dependency && has_cycle(dependency, marks))
            return 1;
    }
    marks[index] = 2;
    return 0;
}

/* Safe mode: essential services and everything they depend on. */
static void want_with_dependencies(service_t *s)
{
    if (s->wanted)
        return;
    s->wanted = 1;
    for (int d = 0; d < s->depend_count; d++) {
        service_t *dependency = find_service(s->depends[d]);
        if (dependency)
            want_with_dependencies(dependency);
    }
}

static void validate(void)
{
    for (int i = 0; i < service_count; i++) {
        service_t *s = &services[i];
        char marks[MAX_SERVICES] = { 0 };

        if (!s->command[0]) {
            s->state = STATE_FAILED;
            snprintf(s->reason, sizeof(s->reason), "no command");
        }
        for (int d = 0; d < s->depend_count; d++) {
            if (!find_service(s->depends[d])) {
                s->state = STATE_FAILED;
                snprintf(s->reason, sizeof(s->reason), "unknown dependency %s", s->depends[d]);
            }
        }
        if (has_cycle(s, marks)) {
            s->state = STATE_FAILED;
            snprintf(s->reason, sizeof(s->reason), "dependency cycle");
        }
        if (s->state == STATE_FAILED)
            log_message("%s: %s", s->name, s->reason);
    }

    for (int i = 0; i < service_count; i++) {
        if (!safe_mode)
            services[i].wanted = 1;
        else if (services[i].essential)
            want_with_dependencies(&services[i]);
    }
}

/* --- Supervision ---------------------------------------------------------------- */

/* 1: all dependencies ready, 0: not yet, -1: one failed (or is not wanted). */
static int dependencies_ready(const service_t *s)
{
    for (int d = 0; d < s->depend_count; d++) {
        service_t *dependency = find_service(s->depends[d]);
        if (dependency->state == STATE_FAILED)
            return -1;
        if (dependency->type == TYPE_ONESHOT ? dependency->state != STATE_DONE
                                             : dependency->state != STATE_RUNNING)
            return dependency->wanted ? 0 : -1;
    }
    return 1;
}

static void start_service(service_t *s)
{
    char command[sizeof(s->command)];
    char *argv[MAX_ARGS + 1];
    int argc = 0;
    char *state;
    jelly_handle_t child_channel = JELLY_HANDLE_INVALID;

    snprintf(command, sizeof(command), "%s", s->command);
    for (char *word = strtok_r(command, " \t", &state); word && argc < MAX_ARGS; word = strtok_r(NULL, " \t", &state))
        argv[argc++] = word;
    argv[argc] = NULL;

    process_options_t options = { .envp = NULL };
    for (int i = 0; i < 3; i++)
        options.stdio[i] = jelly_startup_handle((unsigned)i);
    for (int i = 0; i < 5; i++)
        options.extra[i] = JELLY_HANDLE_INVALID;
    if (s->control) {
        if (STATUS_IS_ERROR(jelly_channel_create(&s->channel, &child_channel))) {
            log_message("%s: cannot create the control channel", s->name);
            s->channel = JELLY_HANDLE_INVALID;
        } else {
            options.extra[0] = child_channel;
        }
    }

    int result = process_spawn(argv[0], argv, &options, &s->process);
    if (child_channel != JELLY_HANDLE_INVALID)
        jelly_handle_close(child_channel);
    if (result) {
        log_message("%s: cannot start %s: %s", s->name, argv[0], strerror(errno));
        if (s->channel != JELLY_HANDLE_INVALID) {
            jelly_handle_close(s->channel);
            s->channel = JELLY_HANDLE_INVALID;
        }
        s->state = STATE_FAILED;
        snprintf(s->reason, sizeof(s->reason), "%s", strerror(errno));
        return;
    }

    jelly_process_info_t info;
    s->pid = STATUS_IS_ERROR(jelly_process_info(s->process, &info)) ? 0 : info.pid;
    s->state = STATE_RUNNING;
    s->started_ns = jelly_clock_ns();
    s->reason[0] = '\0';
    log_message("started %s (process %lu)", s->name, (unsigned long)s->pid);
}

static void release_process(service_t *s)
{
    jelly_handle_close(s->process);
    s->process = JELLY_HANDLE_INVALID;
    if (s->channel != JELLY_HANDLE_INVALID) {
        jelly_handle_close(s->channel);
        s->channel = JELLY_HANDLE_INVALID;
    }
}

static void handle_exit(service_t *s, uint64_t now)
{
    jelly_process_info_t info;
    int code = STATUS_IS_ERROR(jelly_process_info(s->process, &info)) ? -1 : info.exit_code;
    release_process(s);
    s->last_exit = code;

    if (!s->wanted) {
        s->state = STATE_STOPPED;
        log_message("%s stopped", s->name);
        return;
    }
    if (s->type == TYPE_ONESHOT && code == 0) {
        s->state = STATE_DONE;
        log_message("%s completed", s->name);
        return;
    }
    log_message("%s exited with code %d", s->name, code);

    int restart = s->restart == RESTART_ALWAYS || (s->restart == RESTART_ON_FAILURE && code != 0);
    if (!restart) {
        s->state = code == 0 ? STATE_STOPPED : STATE_FAILED;
        s->wanted = 0;
        if (code != 0)
            snprintf(s->reason, sizeof(s->reason), "exited with code %d", code);
        return;
    }

    if (now - s->started_ns >= STABLE_NS)
        s->failures = 0;
    if (++s->failures > RESTART_LIMIT) {
        s->state = STATE_FAILED;
        s->wanted = 0;
        snprintf(s->reason, sizeof(s->reason), "restarted too often");
        log_message("%s keeps failing, giving up", s->name);
        return;
    }
    uint64_t delay = BACKOFF_MIN_NS << (s->failures - 1);
    if (delay > BACKOFF_MAX_NS)
        delay = BACKOFF_MAX_NS;
    s->state = STATE_WAITING;
    s->restart_at_ns = now + delay;
}

static void stop_service(service_t *s)
{
    s->wanted = 0;
    if (s->state == STATE_RUNNING)
        jelly_process_kill(s->process, STOP_EXIT_CODE); /* handle_exit sees the exit */
    else if (s->state == STATE_WAITING)
        s->state = STATE_STOPPED;
}

static void supervise(uint64_t now)
{
    for (int i = 0; i < service_count; i++) {
        service_t *s = &services[i];
        if (s->state == STATE_RUNNING && jelly_wait(s->process, JELLY_NO_WAIT) == STATUS_SUCCESS)
            handle_exit(s, now);
    }

    for (int i = 0; i < service_count; i++) {
        service_t *s = &services[i];
        if (!s->wanted)
            continue;
        if (s->state == STATE_WAITING && now >= s->restart_at_ns)
            s->state = STATE_STOPPED;
        if (s->state != STATE_STOPPED)
            continue;
        int ready = dependencies_ready(s);
        if (ready > 0) {
            start_service(s);
        } else if (ready < 0) {
            s->state = STATE_FAILED;
            s->wanted = 0;
            snprintf(s->reason, sizeof(s->reason), "dependency not available");
            log_message("%s: a dependency is not available", s->name);
        }
    }
}

/* --- Control channel ------------------------------------------------------------ */

static int append(char *reply, size_t size, size_t *used, const char *format, ...)
    __attribute__((format(printf, 4, 5)));
static int append(char *reply, size_t size, size_t *used, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int n = vsnprintf(reply + *used, size - *used, format, args);
    va_end(args);
    if (n > 0)
        *used = *used + (size_t)n < size ? *used + (size_t)n : size - 1;
    return n;
}

static void describe(service_t *s, char *reply, size_t size, size_t *used)
{
    append(reply, size, used, "%-16s %-8s", s->name, state_name(s->state));
    if (s->state == STATE_RUNNING)
        append(reply, size, used, " pid %-4lu", (unsigned long)s->pid);
    else
        append(reply, size, used, "         ");
    if (s->reason[0])
        append(reply, size, used, " (%s)", s->reason);
    else if (s->description[0])
        append(reply, size, used, " %s", s->description);
    append(reply, size, used, "\n");
}

static void handle_request(char *request, char *reply, size_t size)
{
    size_t used = 0;
    char *state;
    char *verb = strtok_r(request, " \t\n", &state);
    char *name = strtok_r(NULL, " \t\n", &state);
    reply[0] = '\0';

    if (!verb) {
        append(reply, size, &used, "error: empty request\n");
        return;
    }
    if (!strcmp(verb, "list")) {
        for (int i = 0; i < service_count; i++)
            describe(&services[i], reply, size, &used);
        return;
    }

    service_t *s = name ? find_service(name) : NULL;
    if (!s) {
        append(reply, size, &used, "error: %s\n", name ? "unknown service" : "service name missing");
        return;
    }
    if (!strcmp(verb, "status")) {
        describe(s, reply, size, &used);
        if (s->state != STATE_RUNNING && s->last_exit)
            append(reply, size, &used, "last exit code %d\n", s->last_exit);
    } else if (!strcmp(verb, "start")) {
        if (s->state == STATE_RUNNING) {
            append(reply, size, &used, "%s is already running\n", s->name);
            return;
        }
        s->wanted = 1;
        s->failures = 0;
        s->reason[0] = '\0';
        s->state = STATE_STOPPED;
        append(reply, size, &used, "starting %s\n", s->name);
    } else if (!strcmp(verb, "stop")) {
        stop_service(s);
        append(reply, size, &used, "stopping %s\n", s->name);
    } else if (!strcmp(verb, "restart")) {
        if (s->state == STATE_RUNNING) {
            /* Kill it; handle_exit restarts it because it stays wanted. */
            s->wanted = 1;
            s->failures = 0;
            jelly_process_kill(s->process, STOP_EXIT_CODE);
        } else {
            s->wanted = 1;
            s->failures = 0;
            s->reason[0] = '\0';
            s->state = STATE_STOPPED;
        }
        append(reply, size, &used, "restarting %s\n", s->name);
    } else {
        append(reply, size, &used, "error: unknown request '%s'\n", verb);
    }
}

static void serve_control(void)
{
    static char request[MESSAGE_MAX], reply[MESSAGE_MAX];

    for (int i = 0; i < service_count; i++) {
        service_t *s = &services[i];
        size_t length;
        if (s->channel == JELLY_HANDLE_INVALID)
            continue;
        status_t status = jelly_channel_receive(s->channel, request, sizeof(request) - 1, &length);
        if (status != STATUS_SUCCESS)
            continue;
        request[length] = '\0';
        handle_request(request, reply, sizeof(reply));
        /* The requester may have been stopped by its own request; its channel may be gone. */
        if (s->channel != JELLY_HANDLE_INVALID)
            jelly_channel_send(s->channel, reply, strlen(reply));
    }
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--safe-mode"))
            safe_mode = 1;
    }

    log_message("starting%s", safe_mode ? " in safe mode (essential services only)" : "");
    if (load_config())
        return 1;
    validate();
    log_message("%d services configured", service_count);

    for (;;) {
        uint64_t now = jelly_clock_ns();
        supervise(now);
        serve_control();
        fflush(stdout);

        /* Wake up for an exit, a control request or the next restart. */
        jelly_handle_t handles[JELLY_WAIT_MANY_MAX];
        uint32_t count = 0, index;
        uint64_t timeout = JELLY_WAIT_FOREVER;
        now = jelly_clock_ns();
        for (int i = 0; i < service_count; i++) {
            service_t *s = &services[i];
            if (s->state == STATE_RUNNING && count < JELLY_WAIT_MANY_MAX)
                handles[count++] = s->process;
            if (s->channel != JELLY_HANDLE_INVALID && count < JELLY_WAIT_MANY_MAX)
                handles[count++] = s->channel;
            if (s->state == STATE_WAITING && s->wanted) {
                uint64_t delay = s->restart_at_ns > now ? s->restart_at_ns - now : 0;
                if (delay < timeout)
                    timeout = delay;
            }
        }
        if (count)
            jelly_wait_many(handles, count, timeout, &index);
        else if (timeout != JELLY_WAIT_FOREVER)
            jelly_thread_sleep(timeout);
        else
            jelly_thread_sleep(1000000000ULL); /* nothing to supervise */
    }
}
