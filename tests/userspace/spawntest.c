/*
 * libc test program for tests/kernel/userspace_tests.c.
 *
 * The kernel writes this program into the ramfs and starts it through the
 * normal spawn path with arguments, environment and a pipe as stdout. It
 * reports what it received on stdout and exercises libc (heap, formatting,
 * conversion, sorting, environment, threads). Exit code 0 means every check
 * passed, otherwise the exit code is the line of the first failed check.
 */

#include <errno.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#include <jelly/os.h>

#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            fprintf(stderr, "spawntest: %s failed\n", #cond);      \
            return __LINE__;                                       \
        }                                                          \
    } while (0)

static int compare_ints(const void *a, const void *b)
{
    return *(const int *)a - *(const int *)b;
}

static mtx_t counter_lock;
static int counter;

static int count_up(void *arg)
{
    for (int i = 0; i < 1000; i++) {
        mtx_lock(&counter_lock);
        counter++;
        mtx_unlock(&counter_lock);
    }
    return (int)(long)arg;
}

static int check_heap(void)
{
    void *blocks[64];
    for (int i = 0; i < 64; i++) {
        blocks[i] = malloc((size_t)(i * 37 + 1));
        CHECK(blocks[i] != NULL);
        memset(blocks[i], i, (size_t)(i * 37 + 1));
    }
    for (int i = 0; i < 64; i += 2)
        free(blocks[i]);
    char *big = malloc(300000); /* own mapping */
    CHECK(big != NULL);
    big[299999] = 1;
    free(big);
    for (int i = 1; i < 64; i += 2) {
        unsigned char *b = blocks[i];
        CHECK(b[0] == i && b[i * 37] == i);
        free(b);
    }
    char *grown = calloc(4, 4);
    CHECK(grown && grown[15] == 0);
    strcpy(grown, "abc");
    grown = realloc(grown, 5000);
    CHECK(grown && !strcmp(grown, "abc"));
    free(grown);
    return 0;
}

static int check_text(void)
{
    char buffer[128];
    CHECK(snprintf(buffer, sizeof(buffer), "%d|%5s|%-3x|%08.3f|%lu|%c|%%", -42, "ab", 255, 3.14159, 1234567890123UL,
                   'z') > 0);
    CHECK(!strcmp(buffer, "-42|   ab|ff |0003.142|1234567890123|z|%"));
    CHECK(snprintf(buffer, 4, "%s", "truncated") == 9 && !strcmp(buffer, "tru"));
    CHECK(strtol("  -0x1F", NULL, 0) == -31);
    CHECK(strtoul("777", NULL, 8) == 511);
    CHECK(atoi("123abc") == 123);

    int numbers[] = { 5, 3, 9, 1, 7, 2 };
    qsort(numbers, 6, sizeof(int), compare_ints);
    CHECK(numbers[0] == 1 && numbers[5] == 9);
    int key = 7;
    CHECK(bsearch(&key, numbers, 6, sizeof(int), compare_ints) == &numbers[4]);

    char words[] = "a,b,,c";
    char *state;
    CHECK(!strcmp(strtok_r(words, ",", &state), "a"));
    CHECK(!strcmp(strtok_r(NULL, ",", &state), "b"));
    CHECK(!strcmp(strtok_r(NULL, ",", &state), "c"));
    CHECK(strtok_r(NULL, ",", &state) == NULL);
    return 0;
}

static int check_environment(void)
{
    CHECK(getenv("JELLY_TEST") && !strcmp(getenv("JELLY_TEST"), "yes"));
    CHECK(setenv("NEW", "1", 0) == 0 && !strcmp(getenv("NEW"), "1"));
    CHECK(setenv("NEW", "2", 0) == 0 && !strcmp(getenv("NEW"), "1"));
    CHECK(setenv("JELLY_TEST", "changed", 1) == 0 && !strcmp(getenv("JELLY_TEST"), "changed"));
    CHECK(unsetenv("NEW") == 0 && getenv("NEW") == NULL);
    return 0;
}

static int check_threads(void)
{
    thrd_t threads[3];
    mtx_init(&counter_lock, mtx_plain);
    for (long i = 0; i < 3; i++)
        CHECK(thrd_create(&threads[i], count_up, (void *)(i + 10)) == thrd_success);
    for (int i = 0; i < 3; i++) {
        int result;
        CHECK(thrd_join(threads[i], &result) == thrd_success && result == i + 10);
    }
    CHECK(counter == 3000);
    return 0;
}

static int check_files(void)
{
    FILE *f = fopen("/tmp/spawntest.txt", "w+");
    CHECK(f != NULL);
    CHECK(fprintf(f, "line one\nline two\n") == 18);
    rewind(f);
    char line[32];
    CHECK(fgets(line, sizeof(line), f) && !strcmp(line, "line one\n"));
    CHECK(ftell(f) == 9);
    CHECK(fclose(f) == 0);
    CHECK(remove("/tmp/spawntest.txt") == 0);
    CHECK(fopen("/tmp/spawntest.txt", "r") == NULL && errno == ENOENT);
    return 0;
}

int main(int argc, char **argv)
{
    char cwd[64];
    size_t length;
    int result;

    printf("argc=%d", argc);
    for (int i = 0; i < argc; i++)
        printf(" [%s]", argv[i]);
    printf(" env=%s", getenv("JELLY_TEST") ? getenv("JELLY_TEST") : "-");
    printf(" stdin=%s", jelly_startup_handle(JELLY_STDIN) != JELLY_HANDLE_INVALID ? "yes" : "no");
    printf(" extra=%s", jelly_startup_handle(3) == JELLY_HANDLE_INVALID ? "none" : "yes");
    CHECK(!STATUS_IS_ERROR(jelly_getcwd(cwd, sizeof(cwd), &length)));
    printf(" cwd=%s\n", cwd);
    fflush(stdout);

    if ((result = check_heap()) || (result = check_text()) || (result = check_environment()) ||
        (result = check_threads()) || (result = check_files()))
        return result;

    /* Read stdin (/dev/zero in the test) through the buffered stream. */
    CHECK(getchar() == 0);
    puts("libc ok");
    return argc == 3 ? 0 : 1;
}
