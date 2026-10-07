/*
 * Kernel tests for Phase 12, storage: the NVMe and AHCI drivers.
 *
 * `make test` attaches one disk to each controller, built like the VirtIO
 * test disk (GPT, one FAT32 partition from tests/storage/disk). The tests
 * use the sectors between the partition table and the partition (LBA
 * 34-2047), which nothing else owns, and read files through the file system.
 */

#include "tests/kernel/ktest.h"

#include "core/format.h"
#include "core/log.h"
#include "core/string.h"
#include "fs/block/block.h"
#include "fs/vfs/vfs.h"
#include "memory/heap.h"
#include "scheduler/thread.h"

#define SCRATCH_LBA   100
#define SCRATCH_COUNT 300 /* 150 KiB: several commands, and transfers with a PRP list on NVMe */

static uint8_t pattern(size_t i, uint8_t seed)
{
    return (uint8_t)(i * 13 + i / 509 + seed);
}

static bool file_starts_with(const char *path, const char *text)
{
    char buffer[64];
    file_t *file;
    size_t size = 0;

    if (vfs_open(path, JELLY_OPEN_READ, 0, NULL, &file) != STATUS_SUCCESS)
        return false;
    status_t status = vfs_read(file, buffer, sizeof(buffer), &size);
    object_release(&file->object);
    return status == STATUS_SUCCESS && size >= strlen(text) && memcmp(buffer, text, strlen(text)) == 0;
}

/* Returns false if the disk is not there (the test is skipped). */
static bool exercise_disk(const char *name, uint8_t seed)
{
    char partition[BLOCK_NAME_MAX], path[96];
    block_device_t *disk = block_find(name);

    if (!disk) {
        klog_info("ktest: no disk %s, skipped", name);
        return false;
    }
    KEXPECT(disk->sector_size == 512 && disk->sector_count == 64 * 2048);

    size_t bytes = SCRATCH_COUNT * 512;
    uint8_t *out = kmalloc(bytes), *in = kmalloc(bytes);
    if (!out || !in) {
        ktest_fail(__FILE__, __LINE__, "out of memory");
        return true;
    }

    /* The protective MBR of the GPT */
    KEXPECT(block_read(disk, 0, 1, in) == STATUS_SUCCESS);
    KEXPECT(in[510] == 0x55 && in[511] == 0xAA);

    /* A large transfer, read back whole and in pieces of every kind of size */
    for (size_t i = 0; i < bytes; i++)
        out[i] = pattern(i, seed);
    KEXPECT(block_write(disk, SCRATCH_LBA, SCRATCH_COUNT, out) == STATUS_SUCCESS);
    KEXPECT(block_flush(disk) == STATUS_SUCCESS);
    memset(in, 0, bytes);
    KEXPECT(block_read(disk, SCRATCH_LBA, SCRATCH_COUNT, in) == STATUS_SUCCESS);
    KEXPECT(memcmp(in, out, bytes) == 0);

    static const uint32_t counts[] = { 1, 3, 8, 9, 16, 17, 127, 128, 129 }; /* around one page, two pages, 64 KiB */
    for (size_t i = 0; i < sizeof(counts) / sizeof(counts[0]); i++) {
        uint32_t offset = (uint32_t)i * 7;
        memset(in, 0xEE, counts[i] * 512);
        KEXPECT(block_read(disk, SCRATCH_LBA + offset, counts[i], in) == STATUS_SUCCESS);
        KEXPECT(memcmp(in, out + offset * 512, counts[i] * 512) == 0);
    }

    /* A small write in the middle changes exactly its sectors */
    memset(in, 0x5A, 2 * 512);
    KEXPECT(block_write(disk, SCRATCH_LBA + 50, 2, in) == STATUS_SUCCESS);
    KEXPECT(block_read(disk, SCRATCH_LBA + 49, 4, in) == STATUS_SUCCESS);
    KEXPECT(memcmp(in, out + 49 * 512, 512) == 0 && in[512] == 0x5A && in[3 * 512 - 1] == 0x5A &&
            memcmp(in + 3 * 512, out + 52 * 512, 512) == 0);

    KEXPECT(block_read(disk, disk->sector_count, 1, in) == STATUS_INVALID_ARGUMENT);
    kfree(out);
    kfree(in);

    /* The partition was found and mounted; files come through the driver */
    format(partition, sizeof(partition), "%sp1", name);
    block_device_t *part = block_find(partition);
    KEXPECT(part && part->parent == disk && part->scheme == PARTITION_GPT);
    format(path, sizeof(path), "/volumes/%s/hello.txt", partition);
    KEXPECT(file_starts_with(path, "Hello from the JellyOS test disk!"));
    return true;
}

KTEST(nvme_disk)
{
    exercise_disk("nvme0n1", 0x11);
}

KTEST(ahci_disk)
{
    /* ahci0 is the machine's boot disk in QEMU (the EFI system partition); the test disk is the second one. */
    if (!exercise_disk("ahci1", 0x77))
        return;
    uint8_t sector[512];
    block_device_t *boot = block_find("ahci0");
    KASSERT(boot != NULL);
    KEXPECT(block_read(boot, 0, 1, sector) == STATUS_SUCCESS && sector[510] == 0x55 && sector[511] == 0xAA);
}

/*
 * USB sticks: one directly on a root port, one behind a hub. They are
 * found by the controller's and the hub's threads, so the test waits for
 * them; the order of the names depends on who is faster.
 */
KTEST(usb_storage_disks)
{
    if (!block_find("nvme0n1")) { /* the machine of `make test` has the sticks whenever it has the NVMe disk */
        klog_info("ktest: no USB sticks, skipped");
        return;
    }
    for (int i = 0; i < 100 && !(block_find("usb0p1") && block_find("usb1p1")); i++)
        thread_sleep(100000000);
    KASSERT(block_find("usb0") && block_find("usb1"));
    KEXPECT(exercise_disk("usb0", 0x21));
    KEXPECT(exercise_disk("usb1", 0x42));
}
