/*
 * Kernel tests for Phase 7: devfs, pipes, the initramfs unpacker and
 * starting programs from files.
 *
 * spawn_runs_libc_program writes tests/userspace/spawntest.c (a libc
 * program embedded by user_images.S) into the ramfs, starts it through
 * process_spawn with arguments, environment and handles, and checks its
 * report and exit code. The boot into init and the shell is covered by
 * tests/integration/shell_test.sh.
 */

#include "tests/kernel/ktest.h"

#include "core/format.h"
#include "core/log.h"
#include "core/string.h"
#include "fs/initramfs/initramfs.h"
#include "fs/vfs/vfs.h"
#include "ipc/ipc.h"
#include "memory/heap.h"
#include "process/spawn.h"

#include <jelly/syscall.h>

#define PROCESS_TIMEOUT_NS 30000000000ULL

extern const uint8_t spawntest_image_start[], spawntest_image_end[];

static const credentials_t root = { UID_ROOT, GID_ROOT };

static status_t write_file(const char *path, const void *data, size_t size, uint32_t mode)
{
    file_t *file;
    size_t done;
    status_t status = vfs_open(path, JELLY_OPEN_WRITE | JELLY_OPEN_CREATE | JELLY_OPEN_TRUNCATE, mode, NULL, &file);
    if (STATUS_IS_ERROR(status))
        return status;
    status = vfs_write(file, data, size, &done);
    object_release(&file->object);
    return STATUS_IS_ERROR(status) ? status : done == size ? STATUS_SUCCESS : STATUS_IO_ERROR;
}

/* Read until EOF (or the buffer is full); returns the byte count. */
static size_t read_all(file_t *file, char *buffer, size_t capacity)
{
    size_t total = 0, done;
    while (total < capacity - 1 && vfs_read(file, buffer + total, capacity - 1 - total, &done) == STATUS_SUCCESS &&
           done)
        total += done;
    buffer[total] = '\0';
    return total;
}

static bool contains(const char *text, const char *word)
{
    size_t n = strlen(word);
    for (; *text; text++) {
        if (strncmp(text, word, n) == 0)
            return true;
    }
    return false;
}

/* --- devfs ---------------------------------------------------------------------- */

KTEST(devfs_null_and_zero)
{
    file_t *file;
    vfs_stat_t stat;
    size_t done;
    uint8_t buffer[64];

    KASSERT(vfs_stat("/dev/null", &root, true, &stat) == STATUS_SUCCESS);
    KEXPECT(stat.type == JELLY_FILE_TYPE_DEVICE);
    KASSERT(vfs_stat("/dev/console", &root, true, &stat) == STATUS_SUCCESS);
    KEXPECT(stat.type == JELLY_FILE_TYPE_DEVICE);

    KASSERT(vfs_open("/dev/null", JELLY_OPEN_READ | JELLY_OPEN_WRITE, 0, NULL, &file) == STATUS_SUCCESS);
    KEXPECT(vfs_write(file, "discarded", 9, &done) == STATUS_SUCCESS && done == 9);
    KEXPECT(vfs_read(file, buffer, sizeof(buffer), &done) == STATUS_SUCCESS && done == 0);
    KEXPECT(vfs_seek(file, 0, JELLY_SEEK_SET, NULL) == STATUS_NOT_SUPPORTED);
    object_release(&file->object);

    memset(buffer, 0xAA, sizeof(buffer));
    KASSERT(vfs_open("/dev/zero", JELLY_OPEN_READ, 0, NULL, &file) == STATUS_SUCCESS);
    KEXPECT(vfs_read(file, buffer, sizeof(buffer), &done) == STATUS_SUCCESS && done == sizeof(buffer));
    KEXPECT(buffer[0] == 0 && buffer[sizeof(buffer) - 1] == 0);
    object_release(&file->object);

    /* Device nodes cannot be created or removed by name. */
    KEXPECT(vfs_mkdir("/dev/new", 0755, &root) != STATUS_SUCCESS);
    KEXPECT(vfs_unlink("/dev/null", &root) != STATUS_SUCCESS);
}

/* --- Pipes ---------------------------------------------------------------------- */

KTEST(pipe_transfers_data_and_reports_eof)
{
    file_t *read_end, *write_end;
    char buffer[32];
    size_t done;

    KASSERT(pipe_create(&read_end, &write_end) == STATUS_SUCCESS);
    KEXPECT(vfs_write(write_end, "through the pipe", 16, &done) == STATUS_SUCCESS && done == 16);
    KEXPECT(vfs_read(read_end, buffer, 7, &done) == STATUS_SUCCESS && done == 7 && !memcmp(buffer, "through", 7));
    KEXPECT(vfs_read(read_end, buffer, sizeof(buffer), &done) == STATUS_SUCCESS && done == 9 &&
            !memcmp(buffer, " the pipe", 9));
    KEXPECT(vfs_seek(read_end, 0, JELLY_SEEK_SET, NULL) == STATUS_NOT_SUPPORTED);

    /* Without writers the reader sees end of file instead of blocking. */
    object_release(&write_end->object);
    KEXPECT(vfs_read(read_end, buffer, sizeof(buffer), &done) == STATUS_SUCCESS && done == 0);
    object_release(&read_end->object);

    /* Without readers a write fails. */
    KASSERT(pipe_create(&read_end, &write_end) == STATUS_SUCCESS);
    object_release(&read_end->object);
    KEXPECT(vfs_write(write_end, "x", 1, &done) == STATUS_PEER_CLOSED);
    object_release(&write_end->object);
}

/* --- Initramfs ------------------------------------------------------------------ */

static size_t cpio_entry(uint8_t *out, const char *name, uint32_t mode, const char *body)
{
    size_t name_size = strlen(name) + 1, body_size = body ? strlen(body) : 0;
    size_t n = format((char *)out, 111, "070701%08X%08X%08X%08X%08X%08X%08X%08X%08X%08X%08X%08X%08X", 1u, mode, 0u,
                      0u, 1u, 0u, (unsigned)body_size, 0u, 0u, 0u, 0u, (unsigned)name_size, 0u);
    memcpy(out + n, name, name_size);
    n += name_size;
    while (n % 4)
        out[n++] = 0;
    if (body_size)
        memcpy(out + n, body, body_size);
    n += body_size;
    while (n % 4)
        out[n++] = 0;
    return n;
}

KTEST(initramfs_unpacks_cpio_archives)
{
    uint8_t *archive = kmalloc(2048);
    unsigned entries;
    vfs_stat_t stat;
    char target[16];
    size_t length;
    KASSERT(archive != NULL);

    size_t n = 0;
    n += cpio_entry(archive + n, ".", 0040755, NULL);
    n += cpio_entry(archive + n, "tmp/ktest-cpio", 0040750, NULL);
    n += cpio_entry(archive + n, "tmp/ktest-cpio/tool", 0100755, "#!binary");
    n += cpio_entry(archive + n, "tmp/ktest-cpio/link", 0120777, "tool");
    n += cpio_entry(archive + n, "TRAILER!!!", 0, NULL);

    KEXPECT(initramfs_unpack(archive, n, &entries) == STATUS_SUCCESS);
    KEXPECT(entries == 4); /* the root entry "." counts as unpacked */
    KEXPECT(vfs_stat("/tmp/ktest-cpio", &root, true, &stat) == STATUS_SUCCESS && stat.type == VNODE_DIRECTORY &&
            stat.mode == 0750);
    KEXPECT(vfs_stat("/tmp/ktest-cpio/tool", &root, true, &stat) == STATUS_SUCCESS && stat.size == 8 &&
            stat.mode == 0755 && stat.uid == 0);
    KEXPECT(vfs_readlink("/tmp/ktest-cpio/link", &root, target, sizeof(target), &length) == STATUS_SUCCESS &&
            length == 4 && !memcmp(target, "tool", 4));

    /* Damaged archives are rejected. */
    archive[0] = 'X';
    KEXPECT(initramfs_unpack(archive, n, &entries) == STATUS_INVALID_ARGUMENT);
    KEXPECT(initramfs_unpack(archive, 50, &entries) == STATUS_INVALID_ARGUMENT);

    vfs_unlink("/tmp/ktest-cpio/link", &root);
    vfs_unlink("/tmp/ktest-cpio/tool", &root);
    vfs_unlink("/tmp/ktest-cpio", &root);
    kfree(archive);
}

/* --- Spawning programs from files ---------------------------------------------- */

KTEST(spawn_checks_the_program_file)
{
    process_t *p;
    char *const argv[] = { "x" };
    spawn_request_t request = { .argv = argv, .argc = 1, .credentials = &root, .cwd = "/" };

    request.path = "/tmp/ktest-missing";
    KEXPECT(process_spawn(&request, &p) == STATUS_NOT_FOUND);

    KASSERT(write_file("/tmp/ktest-noexec", "\x7F" "ELF", 4, 0644) == STATUS_SUCCESS);
    request.path = "/tmp/ktest-noexec";
    KEXPECT(process_spawn(&request, &p) == STATUS_ACCESS_DENIED);

    KASSERT(write_file("/tmp/ktest-garbage", "not an ELF file", 15, 0755) == STATUS_SUCCESS);
    request.path = "/tmp/ktest-garbage";
    KEXPECT(STATUS_IS_ERROR(process_spawn(&request, &p)));

    request.path = "/tmp";
    KEXPECT(process_spawn(&request, &p) == STATUS_ACCESS_DENIED);

    vfs_unlink("/tmp/ktest-noexec", &root);
    vfs_unlink("/tmp/ktest-garbage", &root);
}

KTEST(spawn_runs_libc_program)
{
    file_t *zero, *read_end, *write_end;
    process_t *p;
    char *output = kmalloc(1024);
    KASSERT(output != NULL);

    KASSERT(write_file("/tmp/spawntest", spawntest_image_start,
                       (size_t)(spawntest_image_end - spawntest_image_start), 0755) == STATUS_SUCCESS);
    KASSERT(vfs_open("/dev/zero", JELLY_OPEN_READ, 0, NULL, &zero) == STATUS_SUCCESS);
    KASSERT(pipe_create(&read_end, &write_end) == STATUS_SUCCESS);

    char *const argv[] = { "spawntest", "alpha", "beta gamma" };
    char *const envp[] = { "JELLY_TEST=yes", "PATH=/bin" };
    spawn_request_t request = {
        .path = "/tmp/spawntest",
        .argv = argv,
        .argc = 3,
        .envp = envp,
        .envc = 2,
        .objects = { &zero->object, &write_end->object, &write_end->object },
        .rights = { JELLY_RIGHT_READ, JELLY_RIGHT_WRITE, JELLY_RIGHT_WRITE },
        .handle_count = 3,
        .credentials = &root,
        .cwd = "/tmp",
    };
    status_t status = process_spawn(&request, &p);
    /* The child holds the only write end now: EOF arrives when it exits. */
    object_release(&write_end->object);
    object_release(&zero->object);
    KEXPECT(status == STATUS_SUCCESS);

    if (status == STATUS_SUCCESS) {
        read_all(read_end, output, 1024);
        KEXPECT(object_wait(&p->object, PROCESS_TIMEOUT_NS) == STATUS_SUCCESS);
        if (p->exit_code != 0)
            klog_error("ktest: spawntest exit code %d (line of the failed check), output: %s", p->exit_code, output);
        KEXPECT(p->exit_code == 0);
        KEXPECT(contains(output, "argc=3 [spawntest] [alpha] [beta gamma] env=yes stdin=yes extra=none cwd=/tmp\n"));
        KEXPECT(contains(output, "libc ok\n"));
        object_release(&p->object);
    }
    object_release(&read_end->object);
    vfs_unlink("/tmp/spawntest", &root);
    kfree(output);
}
