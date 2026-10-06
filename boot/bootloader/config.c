#include "config.h"

#include "file.h"
#include "log.h"
#include "text.h"

#define CONFIG_FILE_MAX      0x10000 /* 64 KiB */
#define CONFIG_TIMEOUT_MAX   3600
#define CONFIG_ATTEMPTS_MAX  100

#define DEFAULT_ENTRY        "JellyOS"
#define DEFAULT_RECOVERY     "Recovery"
#define DEFAULT_KERNEL       "/boot/kernels/kernel-current.elf"
#define DEFAULT_FALLBACK     "/boot/kernels/kernel-previous.elf"

typedef enum {
    SECTION_NONE,
    SECTION_BOOT,
    SECTION_ENTRY,
    SECTION_IGNORED,
} section_t;

typedef struct {
    boot_config_t *config;
    section_t      section;
    boot_entry_t  *entry;
    UINTN          line;
    char           default_name[CONFIG_NAME_MAX];
    char           recovery_name[CONFIG_NAME_MAX];
} parser_t;

/* --- Defaults -------------------------------------------------------------- */

static void add_entry(boot_config_t *config, const char *name, const char *cmdline)
{
    boot_entry_t *e = &config->entries[config->entry_count++];

    ZeroMem(e, sizeof(*e));
    text_copy(e->name, sizeof(e->name), name);
    text_copy(e->kernel, sizeof(e->kernel), DEFAULT_KERNEL);
    text_copy(e->cmdline, sizeof(e->cmdline), cmdline);
}

static void apply_default_settings(boot_config_t *config, parser_t *p)
{
    config->timeout = 5;
    config->menu = MENU_AUTO;
    config->max_attempts = 3;
    text_copy(config->fallback_kernel, sizeof(config->fallback_kernel), DEFAULT_FALLBACK);
    text_copy(p->default_name, sizeof(p->default_name), DEFAULT_ENTRY);
    text_copy(p->recovery_name, sizeof(p->recovery_name), DEFAULT_RECOVERY);
}

static void apply_default_entries(boot_config_t *config, parser_t *p)
{
    config->entry_count = 0;
    add_entry(config, DEFAULT_ENTRY, "loglevel=info");
    add_entry(config, DEFAULT_RECOVERY, "recovery=1");
    add_entry(config, "SafeMode", "safe_mode=1");
    text_copy(p->default_name, sizeof(p->default_name), DEFAULT_ENTRY);
    text_copy(p->recovery_name, sizeof(p->recovery_name), DEFAULT_RECOVERY);
}

/* --- Lexing helpers -------------------------------------------------------- */

static bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r';
}

static char *trim(char *s)
{
    while (is_space(*s))
        s++;
    char *end = s + text_length(s);
    while (end > s && is_space(end[-1]))
        *--end = '\0';
    return s;
}

static char *unquote(char *s)
{
    UINTN length = text_length(s);

    if (length >= 2 && s[0] == '"' && s[length - 1] == '"') {
        s[length - 1] = '\0';
        return s + 1;
    }
    return s;
}

static bool parse_number(const char *s, UINTN max, UINTN *out)
{
    UINTN value = 0;

    if (!*s)
        return false;
    for (; *s; s++) {
        if (*s < '0' || *s > '9')
            return false;
        value = value * 10 + (UINTN)(*s - '0');
        if (value > max)
            return false;
    }
    *out = value;
    return true;
}

/* --- Parser ---------------------------------------------------------------- */

static void warn_line(const parser_t *p, const CHAR16 *what, const char *detail)
{
    log_warn(L"%s line %d: %s '%a', ignored", CONFIG_PATH, p->line, what, detail);
}

static bool set_path(const parser_t *p, char *dest, const char *value)
{
    if (value[0] != '/' || !text_copy(dest, CONFIG_PATH_MAX, value)) {
        warn_line(p, L"invalid path", value);
        dest[0] = '\0';
        return false;
    }
    return true;
}

static void begin_section(parser_t *p, char *header)
{
    boot_config_t *config = p->config;

    if (text_equal(header, "boot")) {
        p->section = SECTION_BOOT;
        return;
    }

    if (!text_starts_with(header, "entry ")) {
        warn_line(p, L"unknown section", header);
        p->section = SECTION_IGNORED;
        return;
    }

    char *name = trim(header + 6);
    p->section = SECTION_IGNORED;

    if (!*name || text_length(name) >= CONFIG_NAME_MAX) {
        warn_line(p, L"invalid entry name", name);
        return;
    }
    for (UINTN i = 0; i < config->entry_count; i++) {
        if (text_equal(config->entries[i].name, name)) {
            warn_line(p, L"duplicate entry", name);
            return;
        }
    }
    if (config->entry_count == CONFIG_MAX_ENTRIES) {
        warn_line(p, L"too many entries", name);
        return;
    }

    p->entry = &config->entries[config->entry_count++];
    ZeroMem(p->entry, sizeof(*p->entry));
    text_copy(p->entry->name, sizeof(p->entry->name), name);
    p->section = SECTION_ENTRY;
}

static void parse_boot_key(parser_t *p, const char *key, const char *value)
{
    boot_config_t *config = p->config;

    if (text_equal(key, "default")) {
        if (!text_copy(p->default_name, sizeof(p->default_name), value))
            warn_line(p, L"entry name too long", value);
    } else if (text_equal(key, "timeout")) {
        if (!parse_number(value, CONFIG_TIMEOUT_MAX, &config->timeout))
            warn_line(p, L"invalid timeout", value);
    } else if (text_equal(key, "menu")) {
        if (text_equal(value, "auto"))
            config->menu = MENU_AUTO;
        else if (text_equal(value, "always"))
            config->menu = MENU_ALWAYS;
        else if (text_equal(value, "hidden"))
            config->menu = MENU_HIDDEN;
        else
            warn_line(p, L"invalid menu mode", value);
    } else if (text_equal(key, "fallback_kernel")) {
        if (!*value)
            config->fallback_kernel[0] = '\0';
        else
            set_path(p, config->fallback_kernel, value);
    } else if (text_equal(key, "recovery")) {
        if (!text_copy(p->recovery_name, sizeof(p->recovery_name), value))
            warn_line(p, L"entry name too long", value);
    } else if (text_equal(key, "max_attempts")) {
        UINTN attempts;
        if (parse_number(value, CONFIG_ATTEMPTS_MAX, &attempts) && attempts > 0)
            config->max_attempts = attempts;
        else
            warn_line(p, L"invalid max_attempts", value);
    } else {
        warn_line(p, L"unknown key", key);
    }
}

static void parse_entry_key(parser_t *p, const char *key, const char *value)
{
    boot_entry_t *e = p->entry;

    if (text_equal(key, "kernel")) {
        set_path(p, e->kernel, value);
    } else if (text_equal(key, "initrd")) {
        set_path(p, e->initrd, value);
    } else if (text_equal(key, "module")) {
        if (e->module_count == CONFIG_MAX_MODULES)
            warn_line(p, L"too many modules", value);
        else if (set_path(p, e->modules[e->module_count], value))
            e->module_count++;
    } else if (text_equal(key, "cmdline")) {
        if (!text_copy(e->cmdline, sizeof(e->cmdline), value))
            warn_line(p, L"command line too long", key);
    } else {
        warn_line(p, L"unknown key", key);
    }
}

static void parse_line(parser_t *p, char *line)
{
    line = trim(line);
    if (!*line || *line == '#' || *line == ';')
        return;

    if (*line == '[') {
        UINTN length = text_length(line);
        if (line[length - 1] != ']') {
            warn_line(p, L"malformed section", line);
            p->section = SECTION_IGNORED;
            return;
        }
        line[length - 1] = '\0';
        begin_section(p, trim(line + 1));
        return;
    }

    char *eq = line;
    while (*eq && *eq != '=')
        eq++;
    if (!*eq) {
        warn_line(p, L"expected key=value", line);
        return;
    }
    *eq = '\0';
    char *key = trim(line);
    char *value = unquote(trim(eq + 1));

    switch (p->section) {
    case SECTION_BOOT:
        parse_boot_key(p, key, value);
        break;
    case SECTION_ENTRY:
        parse_entry_key(p, key, value);
        break;
    case SECTION_NONE:
        warn_line(p, L"key outside of a section", key);
        break;
    case SECTION_IGNORED:
        break;
    }
}

static void parse(parser_t *p, char *text)
{
    p->line = 0;
    while (*text) {
        char *line = text;
        while (*text && *text != '\n')
            text++;
        if (*text)
            *text++ = '\0';
        p->line++;
        parse_line(p, line);
    }
}

/* --- Validation ------------------------------------------------------------ */

static void drop_incomplete_entries(boot_config_t *config)
{
    UINTN kept = 0;

    for (UINTN i = 0; i < config->entry_count; i++) {
        boot_entry_t *e = &config->entries[i];
        if (!e->kernel[0]) {
            log_warn(L"Entry '%a' has no kernel, ignored", e->name);
            continue;
        }
        if (kept != i)
            config->entries[kept] = *e;
        kept++;
    }
    config->entry_count = kept;
}

static UINTN find_entry(const boot_config_t *config, const char *name)
{
    for (UINTN i = 0; i < config->entry_count; i++) {
        if (text_equal(config->entries[i].name, name))
            return i;
    }
    return CONFIG_NO_ENTRY;
}

static void resolve_names(boot_config_t *config, const parser_t *p)
{
    config->default_index = find_entry(config, p->default_name);
    if (config->default_index == CONFIG_NO_ENTRY) {
        log_warn(L"Default entry '%a' not found, using '%a'", p->default_name, config->entries[0].name);
        config->default_index = 0;
    }

    config->recovery_index = find_entry(config, p->recovery_name);
    if (config->recovery_index == CONFIG_NO_ENTRY)
        log_warn(L"Recovery entry '%a' not found, automatic recovery unavailable", p->recovery_name);
}

void config_load(EFI_HANDLE image, boot_config_t *config)
{
    parser_t parser = { .config = config, .section = SECTION_NONE };
    char *text;
    UINTN size;

    ZeroMem(config, sizeof(*config));
    apply_default_settings(config, &parser);

    EFI_STATUS status = file_read_all(image, CONFIG_PATH, (void **)&text, &size);
    if (EFI_ERROR(status)) {
        log_warn(L"No boot configuration %s (%r), using built-in defaults", CONFIG_PATH, status);
    } else if (size > CONFIG_FILE_MAX) {
        log_warn(L"Boot configuration is larger than %d bytes, using built-in defaults", CONFIG_FILE_MAX);
        FreePool(text);
    } else {
        parse(&parser, text);
        FreePool(text);
        drop_incomplete_entries(config);
        config->from_file = config->entry_count > 0;
        if (!config->from_file)
            log_warn(L"Boot configuration has no usable entry, using built-in entries");
    }

    if (config->entry_count == 0)
        apply_default_entries(config, &parser);

    resolve_names(config, &parser);
    log_debug(L"Configuration: %d entries, default '%a', timeout %d s",
              config->entry_count, config->entries[config->default_index].name, config->timeout);
}
