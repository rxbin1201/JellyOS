/*
 * sh: the JellyOS command shell.
 *
 *   sh                 interactive on stdin (prompt only when stdin is the console)
 *   sh FILE            run the commands in FILE
 *   sh -c COMMAND      run one command line
 *
 * Syntax: words separated by blanks, '...' and "..." quoting, \ escapes,
 * $NAME, ${NAME} and $? expansion (not inside '...'), # comments,
 * redirections < FILE, > FILE, >> FILE, 2> FILE, pipelines a | b | c and
 * command lists a ; b.
 *
 * Builtins: cd, pwd, exit, help, env, export, unset, svc, sync, poweroff,
 * reboot, status. Everything else is started from $PATH; the shell waits for
 * the whole pipeline and keeps the exit code of the last command in $?.
 */

#include <errno.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jelly/os.h>

#define LINE_MAX       1024
#define MAX_WORDS      64
#define MAX_COMMANDS   8
#define WORD_BUFFER    4096
#define CONTROL_HANDLE 3      /* startup slot of the service manager's control channel */
#define CONTROL_TIMEOUT_NS 3000000000ULL

typedef struct {
    char *argv[MAX_WORDS + 1];
    int   argc;
    char *input;              /* < FILE */
    char *output;             /* > FILE or >> FILE */
    int   append;
    char *error;              /* 2> FILE */
} command_t;

static int last_status;
static int interactive;
static int should_exit;
static int exit_code;

/* --- Parsing --------------------------------------------------------------------- */

typedef struct {
    char  buffer[WORD_BUFFER];
    size_t used;
} word_store_t;

static int store_char(word_store_t *store, char c)
{
    if (store->used >= sizeof(store->buffer) - 1)
        return -1;
    store->buffer[store->used++] = c;
    return 0;
}

static int store_text(word_store_t *store, const char *text)
{
    for (; *text; text++) {
        if (store_char(store, *text))
            return -1;
    }
    return 0;
}

static int is_name_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

/* Expand $NAME, ${NAME} or $? at *p (just after '$') into the store. */
static int expand_variable(const char **p, word_store_t *store)
{
    char name[128];
    size_t n = 0;
    const char *s = *p;

    if (*s == '?') {
        char number[16];
        snprintf(number, sizeof(number), "%d", last_status);
        *p = s + 1;
        return store_text(store, number);
    }
    if (*s == '{') {
        s++;
        while (*s && *s != '}' && n < sizeof(name) - 1)
            name[n++] = *s++;
        if (*s != '}')
            return -1;
        s++;
    } else {
        while (is_name_char(*s) && n < sizeof(name) - 1)
            name[n++] = *s++;
    }
    name[n] = '\0';
    *p = s;
    if (n == 0)
        return store_char(store, '$');
    const char *value = getenv(name);
    return value ? store_text(store, value) : 0;
}

typedef enum { TOKEN_WORD, TOKEN_PIPE, TOKEN_SEMICOLON, TOKEN_IN, TOKEN_OUT, TOKEN_APPEND, TOKEN_ERR, TOKEN_END } token_t;

/* Read the next token from *p; words go into the store (NUL terminated). */
static token_t next_token(const char **p, word_store_t *store, char **word, int *error)
{
    const char *s = *p;
    while (*s == ' ' || *s == '\t')
        s++;
    if (!*s || *s == '#' || *s == '\n') {
        *p = s;
        return TOKEN_END;
    }
    switch (*s) {
    case '|': *p = s + 1; return TOKEN_PIPE;
    case ';': *p = s + 1; return TOKEN_SEMICOLON;
    case '<': *p = s + 1; return TOKEN_IN;
    case '>':
        if (s[1] == '>') {
            *p = s + 2;
            return TOKEN_APPEND;
        }
        *p = s + 1;
        return TOKEN_OUT;
    case '2':
        if (s[1] == '>') {
            *p = s + 2;
            return TOKEN_ERR;
        }
        break;
    }

    *word = store->buffer + store->used;
    while (*s && !strchr(" \t\n|;<>", *s)) {
        if (*s == '\'') {
            s++;
            while (*s && *s != '\'') {
                if (store_char(store, *s++))
                    goto overflow;
            }
            if (*s != '\'') {
                fprintf(stderr, "sh: missing closing '\n");
                *error = 1;
                return TOKEN_END;
            }
            s++;
        } else if (*s == '"') {
            s++;
            while (*s && *s != '"') {
                if (*s == '\\' && (s[1] == '"' || s[1] == '\\' || s[1] == '$')) {
                    s++;
                    if (store_char(store, *s++))
                        goto overflow;
                } else if (*s == '$') {
                    s++;
                    if (expand_variable(&s, store))
                        goto overflow;
                } else if (store_char(store, *s++)) {
                    goto overflow;
                }
            }
            if (*s != '"') {
                fprintf(stderr, "sh: missing closing \"\n");
                *error = 1;
                return TOKEN_END;
            }
            s++;
        } else if (*s == '\\' && s[1]) {
            if (store_char(store, s[1]))
                goto overflow;
            s += 2;
        } else if (*s == '$') {
            s++;
            if (expand_variable(&s, store))
                goto overflow;
        } else if (store_char(store, *s++)) {
            goto overflow;
        }
    }
    if (store_char(store, '\0'))
        goto overflow;
    *p = s;
    return TOKEN_WORD;

overflow:
    fprintf(stderr, "sh: line too long\n");
    *error = 1;
    return TOKEN_END;
}

/*
 * Parse one pipeline from *p (up to ';' or the end of the line).
 * Returns the number of commands, 0 for an empty pipeline, -1 on a syntax error.
 */
static int parse_pipeline(const char **p, word_store_t *store, command_t *commands)
{
    int count = 0, error = 0;
    command_t *c = &commands[0];
    memset(commands, 0, sizeof(command_t) * MAX_COMMANDS);

    for (;;) {
        char *word = NULL;
        token_t token = next_token(p, store, &word, &error);
        if (error)
            return -1;

        if (token == TOKEN_WORD) {
            if (c->argc == MAX_WORDS) {
                fprintf(stderr, "sh: too many words\n");
                return -1;
            }
            c->argv[c->argc++] = word;
            continue;
        }
        if (token == TOKEN_IN || token == TOKEN_OUT || token == TOKEN_APPEND || token == TOKEN_ERR) {
            char *target = NULL;
            if (next_token(p, store, &target, &error) != TOKEN_WORD || error) {
                fprintf(stderr, "sh: redirection without a file name\n");
                return -1;
            }
            if (token == TOKEN_IN)
                c->input = target;
            else if (token == TOKEN_ERR)
                c->error = target;
            else {
                c->output = target;
                c->append = token == TOKEN_APPEND;
            }
            continue;
        }

        /* PIPE, SEMICOLON or END finish the current command. */
        if (c->argc == 0) {
            if (token == TOKEN_PIPE || count > 0) {
                fprintf(stderr, "sh: syntax error near '|'\n");
                return -1;
            }
            return 0;
        }
        c->argv[c->argc] = NULL;
        count++;
        if (token != TOKEN_PIPE)
            return count;
        if (count == MAX_COMMANDS) {
            fprintf(stderr, "sh: pipeline too long\n");
            return -1;
        }
        c = &commands[count];
    }
}

/* --- Builtins ------------------------------------------------------------------- */

static int builtin_cd(int argc, char **argv, FILE *out)
{
    (void)out;
    const char *path = argc > 1 ? argv[1] : getenv("HOME");
    if (!path)
        path = "/";
    status_t status = jelly_chdir(path);
    if (STATUS_IS_ERROR(status)) {
        fprintf(stderr, "cd: %s: %s\n", path, strerror((int)status));
        return 1;
    }
    char cwd[1024];
    size_t length;
    if (!STATUS_IS_ERROR(jelly_getcwd(cwd, sizeof(cwd), &length)))
        setenv("PWD", cwd, 1);
    return 0;
}

static int builtin_pwd(int argc, char **argv, FILE *out)
{
    (void)argc;
    (void)argv;
    char cwd[1024];
    size_t length;
    status_t status = jelly_getcwd(cwd, sizeof(cwd), &length);
    if (STATUS_IS_ERROR(status)) {
        fprintf(stderr, "pwd: %s\n", strerror((int)status));
        return 1;
    }
    fprintf(out, "%s\n", cwd);
    return 0;
}

static int builtin_exit(int argc, char **argv, FILE *out)
{
    (void)out;
    should_exit = 1;
    exit_code = argc > 1 ? atoi(argv[1]) : last_status;
    return exit_code;
}

static int builtin_env(int argc, char **argv, FILE *out)
{
    (void)argc;
    (void)argv;
    for (char **e = environ; e && *e; e++)
        fprintf(out, "%s\n", *e);
    return 0;
}

static int builtin_export(int argc, char **argv, FILE *out)
{
    if (argc == 1)
        return builtin_env(argc, argv, out);
    for (int i = 1; i < argc; i++) {
        char *equals = strchr(argv[i], '=');
        if (!equals) {
            fprintf(stderr, "export: expected NAME=VALUE\n");
            return 1;
        }
        *equals = '\0';
        if (setenv(argv[i], equals + 1, 1)) {
            fprintf(stderr, "export: %s: %s\n", argv[i], strerror(errno));
            return 1;
        }
    }
    return 0;
}

static int builtin_unset(int argc, char **argv, FILE *out)
{
    (void)out;
    for (int i = 1; i < argc; i++)
        unsetenv(argv[i]);
    return 0;
}

static int builtin_status(int argc, char **argv, FILE *out)
{
    (void)argc;
    (void)argv;
    fprintf(out, "%d\n", last_status);
    return 0;
}

/* svc list | status NAME | start NAME | stop NAME | restart NAME */
static int builtin_svc(int argc, char **argv, FILE *out)
{
    static char message[4096];
    jelly_handle_t control = jelly_startup_handle(CONTROL_HANDLE);
    size_t used = 0, length;

    if (control == JELLY_HANDLE_INVALID) {
        fprintf(stderr, "svc: no connection to the service manager\n");
        return 1;
    }
    if (argc < 2) {
        fprintf(stderr, "usage: svc list | status NAME | start NAME | stop NAME | restart NAME\n");
        return 2;
    }
    for (int i = 1; i < argc; i++)
        used += (size_t)snprintf(message + used, sizeof(message) - used, i > 1 ? " %s" : "%s", argv[i]);

    status_t status = jelly_channel_send(control, message, strlen(message));
    if (!STATUS_IS_ERROR(status))
        status = jelly_wait(control, CONTROL_TIMEOUT_NS);
    if (!STATUS_IS_ERROR(status))
        status = jelly_channel_receive(control, message, sizeof(message) - 1, &length);
    if (STATUS_IS_ERROR(status)) {
        fprintf(stderr, "svc: %s\n", strerror((int)status));
        return 1;
    }
    message[length] = '\0';
    fputs(message, out);
    return strncmp(message, "error:", 6) ? 0 : 1;
}

static int builtin_sync(int argc, char **argv, FILE *out)
{
    (void)argc;
    (void)argv;
    (void)out;
    status_t status = jelly_sync();
    if (STATUS_IS_ERROR(status)) {
        fprintf(stderr, "sync: %s\n", strerror((int)status));
        return 1;
    }
    return 0;
}

static int power(uint32_t action, const char *name)
{
    fflush(stdout);
    status_t status = jelly_system_power(action);
    fprintf(stderr, "%s: %s\n", name, strerror((int)status));
    return 1;
}

static int builtin_poweroff(int argc, char **argv, FILE *out)
{
    (void)argc;
    (void)argv;
    fprintf(out, "Powering off.\n");
    return power(JELLY_POWER_OFF, "poweroff");
}

static int builtin_reboot(int argc, char **argv, FILE *out)
{
    (void)argc;
    (void)argv;
    fprintf(out, "Rebooting.\n");
    return power(JELLY_POWER_REBOOT, "reboot");
}

static int builtin_help(int argc, char **argv, FILE *out);

typedef struct {
    const char *name;
    int (*run)(int argc, char **argv, FILE *out);
    const char *help;
} builtin_t;

static const builtin_t builtins[] = {
    { "cd",       builtin_cd,       "cd [DIR]          change the working directory" },
    { "pwd",      builtin_pwd,      "pwd               print the working directory" },
    { "exit",     builtin_exit,     "exit [CODE]       leave the shell" },
    { "help",     builtin_help,     "help              show this list" },
    { "env",      builtin_env,      "env               print the environment" },
    { "export",   builtin_export,   "export NAME=VALUE set an environment variable" },
    { "unset",    builtin_unset,    "unset NAME        remove an environment variable" },
    { "status",   builtin_status,   "status            print the exit code of the last command" },
    { "svc",      builtin_svc,      "svc list|status|start|stop|restart [NAME]  control services" },
    { "sync",     builtin_sync,     "sync              write cached file data to disk" },
    { "poweroff", builtin_poweroff, "poweroff          sync and switch the machine off" },
    { "reboot",   builtin_reboot,   "reboot            sync and restart the machine" },
};

static int builtin_help(int argc, char **argv, FILE *out)
{
    (void)argc;
    (void)argv;
    fprintf(out, "JellyOS shell builtins:\n");
    for (size_t i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++)
        fprintf(out, "  %s\n", builtins[i].help);
    fprintf(out, "Programs in $PATH (%s) run as separate processes.\n"
                 "Syntax: a | b, a ; b, < in, > out, >> out, 2> err, $NAME, $?, '...', \"...\"\n",
            getenv("PATH") ? getenv("PATH") : "");
    return 0;
}

static const builtin_t *find_builtin(const char *name)
{
    for (size_t i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++) {
        if (!strcmp(builtins[i].name, name))
            return &builtins[i];
    }
    return NULL;
}

/* --- Running commands ----------------------------------------------------------- */

static int open_redirect(const char *path, int output, int append, jelly_handle_t *handle)
{
    uint32_t flags = output ? JELLY_OPEN_WRITE | JELLY_OPEN_CREATE | (append ? JELLY_OPEN_APPEND : JELLY_OPEN_TRUNCATE)
                            : JELLY_OPEN_READ;
    status_t status = jelly_open(path, flags, 0644, handle);
    if (STATUS_IS_ERROR(status)) {
        fprintf(stderr, "sh: %s: %s\n", path, strerror((int)status));
        return -1;
    }
    return 0;
}

static int run_builtin(const builtin_t *builtin, command_t *c)
{
    FILE *out = stdout;
    jelly_handle_t handle;

    if (c->output) {
        if (open_redirect(c->output, 1, c->append, &handle))
            return 1;
        out = fdopen_handle(handle, "w");
        if (!out) {
            jelly_handle_close(handle);
            return 1;
        }
    }
    int status = builtin->run(c->argc, c->argv, out);
    if (out != stdout)
        fclose(out);
    else
        fflush(stdout);
    return status;
}

static int run_pipeline(command_t *commands, int count)
{
    jelly_handle_t processes[MAX_COMMANDS];
    jelly_handle_t previous_read = JELLY_HANDLE_INVALID;
    int started = 0, status = 0;

    if (count == 1) {
        const builtin_t *builtin = find_builtin(commands[0].argv[0]);
        if (builtin)
            return run_builtin(builtin, &commands[0]);
    }
    fflush(stdout);

    for (int i = 0; i < count; i++) {
        command_t *c = &commands[i];
        jelly_handle_t opened[3] = { JELLY_HANDLE_INVALID, JELLY_HANDLE_INVALID, JELLY_HANDLE_INVALID };
        jelly_handle_t next_read = JELLY_HANDLE_INVALID;
        process_options_t options = { .envp = NULL };
        int failed = 0;

        for (int s = 0; s < 3; s++)
            options.stdio[s] = jelly_startup_handle((unsigned)s);
        for (int e = 0; e < 5; e++)
            options.extra[e] = JELLY_HANDLE_INVALID;

        if (find_builtin(c->argv[0])) {
            fprintf(stderr, "sh: %s: builtins cannot be part of a pipeline\n", c->argv[0]);
            failed = 1;
        }
        if (previous_read != JELLY_HANDLE_INVALID)
            options.stdio[0] = previous_read;
        if (!failed && i + 1 < count) {
            jelly_handle_t write_end;
            status_t s = jelly_pipe_create(&next_read, &write_end);
            if (STATUS_IS_ERROR(s)) {
                fprintf(stderr, "sh: pipe: %s\n", strerror((int)s));
                failed = 1;
            } else {
                options.stdio[1] = opened[1] = write_end;
            }
        }
        if (!failed && c->input) {
            failed = open_redirect(c->input, 0, 0, &opened[0]);
            options.stdio[0] = opened[0];
        }
        if (!failed && c->output) {
            if (opened[1] != JELLY_HANDLE_INVALID)
                jelly_handle_close(opened[1]);
            failed = open_redirect(c->output, 1, c->append, &opened[1]);
            options.stdio[1] = opened[1];
        }
        if (!failed && c->error) {
            failed = open_redirect(c->error, 1, 0, &opened[2]);
            options.stdio[2] = opened[2];
        }

        if (!failed && process_spawn(c->argv[0], c->argv, &options, &processes[started])) {
            fprintf(stderr, "sh: %s: %s\n", c->argv[0], errno == ENOENT ? "command not found" : strerror(errno));
            failed = 1;
        }
        if (!failed)
            started++;

        /* The children hold their own copies now. */
        for (int s = 0; s < 3; s++) {
            if (opened[s] != JELLY_HANDLE_INVALID)
                jelly_handle_close(opened[s]);
        }
        if (previous_read != JELLY_HANDLE_INVALID)
            jelly_handle_close(previous_read);
        previous_read = next_read;

        if (failed) {
            status = 127;
            if (previous_read != JELLY_HANDLE_INVALID) {
                jelly_handle_close(previous_read);
                previous_read = JELLY_HANDLE_INVALID;
            }
            break;
        }
    }

    /* Wait for every started command; the last one's code counts. */
    for (int i = 0; i < started; i++) {
        int code;
        if (process_wait(processes[i], &code))
            code = 1;
        jelly_handle_close(processes[i]);
        if (i == count - 1)
            status = code;
    }
    return status;
}

static void run_line(const char *line)
{
    static word_store_t store;
    static command_t commands[MAX_COMMANDS];
    const char *p = line;

    for (;;) {
        store.used = 0;
        int count = parse_pipeline(&p, &store, commands);
        if (count < 0) {
            last_status = 2;
            return;
        }
        if (count > 0) {
            last_status = run_pipeline(commands, count);
            if (interactive && last_status != 0 && !should_exit)
                printf("[exit %d]\n", last_status);
        }
        if (should_exit)
            return;
        /* parse_pipeline stopped after a ';' or at the end of the line. */
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p || *p == '#' || *p == '\n')
            return;
    }
}

static void prompt(void)
{
    char cwd[256];
    size_t length;
    if (STATUS_IS_ERROR(jelly_getcwd(cwd, sizeof(cwd), &length)))
        strcpy(cwd, "?");
    printf("jelly:%s# ", cwd);
    fflush(stdout);
}

static int run_stream(FILE *input)
{
    char line[LINE_MAX];
    for (;;) {
        if (interactive)
            prompt();
        if (!fgets(line, sizeof(line), input)) {
            if (interactive)
                printf("\n");
            break;
        }
        line[strcspn(line, "\n")] = '\0';
        run_line(line);
        if (should_exit)
            return exit_code;
    }
    return last_status;
}

int main(int argc, char **argv)
{
    if (argc > 2 && !strcmp(argv[1], "-c")) {
        run_line(argv[2]);
        return should_exit ? exit_code : last_status;
    }
    if (argc > 1) {
        FILE *script = fopen(argv[1], "r");
        if (!script) {
            fprintf(stderr, "sh: %s: %s\n", argv[1], strerror(errno));
            return 127;
        }
        int status = run_stream(script);
        fclose(script);
        return status;
    }

    jelly_stat_t stat;
    interactive = !STATUS_IS_ERROR(jelly_fstat(jelly_startup_handle(JELLY_STDIN), &stat)) &&
                  stat.type == JELLY_FILE_TYPE_DEVICE;
    if (interactive)
        printf("JellyOS shell. Type 'help' for the builtins.\n");
    return run_stream(stdin);
}
