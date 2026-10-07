/*
 * AHCI driver (README section 25): SATA disks.
 *
 * An AHCI controller has up to 32 ports; each port with a disk gets a
 * command list (the driver uses slot 0 only), an area for received FISes
 * and a command table holding the ATA command and the list of data buffers.
 * Commands are READ/WRITE DMA EXT (48-bit LBA), FLUSH CACHE EXT and
 * IDENTIFY DEVICE.
 *
 * Requests on a port are serialized by a mutex and go through a DMA bounce
 * buffer of 64 KiB, so callers may pass any kernel buffer. Completion comes
 * by interrupt (MSI) or, if there is none, by polling.
 *
 * Every SATA disk with 512-byte sectors becomes a block device "ahci<n>".
 *
 * Not yet: ATAPI (optical drives), native command queuing, hot plug, port
 * multipliers, power management.
 */

#include "drivers/bus/pci/pci.h"
#include "drivers/core/module.h"
#include "fs/block/block.h"

#include "core/format.h"
#include "core/log.h"
#include "core/string.h"
#include "memory/heap.h"
#include "memory/layout.h"
#include "scheduler/mutex.h"
#include "scheduler/thread.h"

/* Host registers */
#define HBA_CAP   0x00
#define HBA_GHC   0x04
#define HBA_IS    0x08
#define HBA_PI    0x0C
#define HBA_CAP2  0x24
#define HBA_BOHC  0x28
#define HBA_PORTS 0x100 /* + 0x80 * port */

#define CAP_64BIT      (1u << 31)
#define GHC_INTERRUPTS (1u << 1)
#define GHC_AHCI       (1u << 31)
#define CAP2_BOH       (1u << 0)
#define BOHC_BIOS_OWNED (1u << 0)
#define BOHC_OS_OWNED   (1u << 1)

/* Port registers */
#define PORT_CLB  0x00
#define PORT_CLBU 0x04
#define PORT_FB   0x08
#define PORT_FBU  0x0C
#define PORT_IS   0x10
#define PORT_IE   0x14
#define PORT_CMD  0x18
#define PORT_TFD  0x20
#define PORT_SIG  0x24
#define PORT_SSTS 0x28
#define PORT_SCTL 0x2C
#define PORT_SERR 0x30
#define PORT_CI   0x38

#define CMD_START        (1u << 0)
#define CMD_SPIN_UP      (1u << 1)
#define CMD_POWER_ON     (1u << 2)
#define CMD_FIS_RECEIVE  (1u << 4)
#define CMD_FIS_RUNNING  (1u << 14)
#define CMD_RUNNING      (1u << 15)
#define CMD_ICC_MASK     (0xFu << 28)
#define CMD_ICC_ACTIVE   (1u << 28) /* wake the link from a power saving state */
#define TFD_ERROR        (1u << 0)
#define TFD_DRQ          (1u << 3)
#define TFD_BUSY         (1u << 7)
#define IS_TASK_FILE_ERROR (1u << 30)
#define IE_DEFAULT       0x7D80003F /* errors and every kind of completion */

#define SIGNATURE_SATA   0x00000101

#define ATA_READ_DMA_EXT    0x25
#define ATA_WRITE_DMA_EXT   0x35
#define ATA_FLUSH_CACHE_EXT 0xEA
#define ATA_IDENTIFY        0xEC

#define FIS_HOST_TO_DEVICE 0x27
#define HEADER_WRITE       (1u << 6)

#define SECTOR_SIZE  512
#define BOUNCE_SIZE  (64 * 1024)
#define MAX_PORTS    32
#define COMMAND_TIMEOUT_NS 10000000000ull
#define POLL_NS      10000000ull

typedef struct {
    uint16_t flags;       /* FIS length in dwords, write bit */
    uint16_t prdt_length;
    uint32_t transferred;
    uint64_t table;
    uint32_t reserved[4];
} ahci_header_t;

typedef struct {
    uint8_t  fis[64];
    uint8_t  atapi[16];
    uint8_t  reserved[48];
    struct {
        uint64_t address;
        uint32_t reserved;
        uint32_t count;   /* bytes - 1 */
    } prdt[1];
} ahci_table_t;

struct ahci;

typedef struct {
    struct ahci      *controller;
    volatile uint8_t *regs;
    uint32_t          number;
    mutex_t           lock;
    dma_buffer_t      memory;  /* command list at 0, received FIS at 1024, command table at 2048 */
    dma_buffer_t      bounce;
    block_device_t    block;
    bool              lba48;
} ahci_port_t;

typedef struct ahci {
    device_t         *device;
    volatile uint8_t *regs;
    uint32_t          irq;
    uint64_t          dma_limit;
    wait_queue_t      completion;
    ahci_port_t      *ports[MAX_PORTS];
} ahci_t;

static uint32_t disk_count;

static uint32_t rd(volatile uint8_t *base, uint32_t reg)
{
    return *(volatile uint32_t *)(base + reg);
}

static void wr(volatile uint8_t *base, uint32_t reg, uint32_t value)
{
    *(volatile uint32_t *)(base + reg) = value;
}

static bool wait_clear(volatile uint8_t *base, uint32_t reg, uint32_t mask, uint32_t timeout_ms)
{
    for (uint32_t waited = 0; rd(base, reg) & mask; waited++) {
        if (waited >= timeout_ms)
            return false;
        thread_sleep(1000000);
    }
    return true;
}

static void ahci_interrupt(void *context)
{
    ahci_t *a = context;
    uint32_t pending = rd(a->regs, HBA_IS);

    /* The ports keep their status for the waiting thread to see errors; only the summary is acknowledged. */
    for (uint32_t i = 0; i < MAX_PORTS; i++) {
        if ((pending & (1u << i)) && a->ports[i])
            wr(a->ports[i]->regs, PORT_IE, 0); /* quiet until the thread has looked */
    }
    wr(a->regs, HBA_IS, pending);
    wait_queue_wake_all(&a->completion, STATUS_SUCCESS);
}

/* --- Commands -------------------------------------------------------------------- */

/* Run the ATA command in slot 0: `bytes` of data in the bounce buffer. port->lock held. */
static status_t run(ahci_port_t *port, uint8_t command, uint64_t lba, uint32_t sectors, uint32_t bytes, bool write)
{
    ahci_t *a = port->controller;
    ahci_header_t *header = port->memory.virt;
    ahci_table_t *table = (ahci_table_t *)((uint8_t *)port->memory.virt + 2048);

    if (!wait_clear(port->regs, PORT_TFD, TFD_BUSY | TFD_DRQ, 1000))
        return STATUS_DEVICE_ERROR;

    memset(table, 0, sizeof(*table));
    table->fis[0] = FIS_HOST_TO_DEVICE;
    table->fis[1] = 0x80; /* a command, not device control */
    table->fis[2] = command;
    table->fis[4] = (uint8_t)lba;
    table->fis[5] = (uint8_t)(lba >> 8);
    table->fis[6] = (uint8_t)(lba >> 16);
    table->fis[7] = 0x40; /* LBA mode */
    table->fis[8] = (uint8_t)(lba >> 24);
    table->fis[9] = (uint8_t)(lba >> 32);
    table->fis[10] = (uint8_t)(lba >> 40);
    table->fis[12] = (uint8_t)sectors;
    table->fis[13] = (uint8_t)(sectors >> 8);
    if (bytes) {
        table->prdt[0].address = port->bounce.phys;
        table->prdt[0].count = bytes - 1;
    }
    memset(header, 0, sizeof(*header));
    header->flags = (uint16_t)(5 | (write ? HEADER_WRITE : 0)); /* the FIS has 5 dwords */
    header->prdt_length = bytes ? 1 : 0;
    header->table = port->memory.phys + 2048;

    wr(port->regs, PORT_IS, 0xFFFFFFFF);
    uint64_t deadline = wait_deadline(COMMAND_TIMEOUT_NS);
    uint64_t flags = arch_interrupts_save();
    wr(port->regs, PORT_IE, IE_DEFAULT);
    wr(port->regs, PORT_CI, 1);
    /* The interrupt wakes us; the short deadline makes it work without one, too. */
    while ((rd(port->regs, PORT_CI) & 1) && !(rd(port->regs, PORT_IS) & IS_TASK_FILE_ERROR)) {
        if (wait_deadline(0) >= deadline)
            break;
        uint64_t next = wait_deadline(POLL_NS);
        wait_queue_block_uninterruptible(&a->completion, next < deadline ? next : deadline);
        /* Acknowledge what was seen (errors stay for the check below), then listen again. */
        wr(port->regs, PORT_IS, rd(port->regs, PORT_IS) & ~IS_TASK_FILE_ERROR);
        wr(port->regs, PORT_IE, IE_DEFAULT);
    }
    arch_interrupts_restore(flags);

    uint32_t status = rd(port->regs, PORT_IS), task_file = rd(port->regs, PORT_TFD);
    wr(port->regs, PORT_IS, status);
    if (rd(port->regs, PORT_CI) & 1) {
        klog_error("block %s: command 0x%x timed out", port->block.name, command);
        return STATUS_TIMEOUT;
    }
    if ((status & IS_TASK_FILE_ERROR) || (task_file & TFD_ERROR)) {
        klog_debug("block %s: command 0x%x failed (task file 0x%x)", port->block.name, command, task_file);
        /* The port stops on an error: restart command processing. */
        wr(port->regs, PORT_CMD, rd(port->regs, PORT_CMD) & ~CMD_START);
        wait_clear(port->regs, PORT_CMD, CMD_RUNNING, 500);
        wr(port->regs, PORT_SERR, 0xFFFFFFFF);
        wr(port->regs, PORT_CMD, rd(port->regs, PORT_CMD) | CMD_START);
        return STATUS_IO_ERROR;
    }
    return STATUS_SUCCESS;
}

static status_t transfer(ahci_port_t *port, bool write, uint64_t lba, uint32_t count, void *buffer)
{
    uint8_t *data = buffer;
    status_t status = STATUS_SUCCESS;

    mutex_lock(&port->lock);
    while (count && !STATUS_IS_ERROR(status)) {
        uint32_t chunk = count < BOUNCE_SIZE / SECTOR_SIZE ? count : BOUNCE_SIZE / SECTOR_SIZE;
        uint32_t bytes = chunk * SECTOR_SIZE;

        if (write)
            memcpy(port->bounce.virt, data, bytes);
        status = run(port, write ? ATA_WRITE_DMA_EXT : ATA_READ_DMA_EXT, lba, chunk, bytes, write);
        if (!STATUS_IS_ERROR(status) && !write)
            memcpy(data, port->bounce.virt, bytes);
        data += bytes;
        lba += chunk;
        count -= chunk;
    }
    mutex_unlock(&port->lock);
    return status;
}

static status_t ahci_read(block_device_t *block, uint64_t lba, uint32_t count, void *buffer)
{
    return transfer(block->driver_data, false, lba, count, buffer);
}

static status_t ahci_write(block_device_t *block, uint64_t lba, uint32_t count, const void *buffer)
{
    return transfer(block->driver_data, true, lba, count, (void *)buffer);
}

static status_t ahci_flush(block_device_t *block)
{
    ahci_port_t *port = block->driver_data;
    mutex_lock(&port->lock);
    status_t status = run(port, ATA_FLUSH_CACHE_EXT, 0, 0, 0, false);
    mutex_unlock(&port->lock);
    return status;
}

static const block_ops_t ahci_ops = {
    .read = ahci_read,
    .write = ahci_write,
    .flush = ahci_flush,
};

/* --- Ports ----------------------------------------------------------------------- */

static void port_stop(volatile uint8_t *regs)
{
    wr(regs, PORT_CMD, rd(regs, PORT_CMD) & ~CMD_START);
    wait_clear(regs, PORT_CMD, CMD_RUNNING, 500);
    wr(regs, PORT_CMD, rd(regs, PORT_CMD) & ~CMD_FIS_RECEIVE);
    wait_clear(regs, PORT_CMD, CMD_FIS_RUNNING, 500);
}

/* ATA strings are stored as big-endian 16-bit words, padded with spaces. */
static void ata_text(char *out, size_t size, const uint16_t *words, size_t count)
{
    size_t n = 0;
    for (size_t i = 0; i < count && n + 2 < size; i++) {
        out[n++] = (char)(words[i] >> 8);
        out[n++] = (char)(words[i] & 0xFF);
    }
    while (n && (out[n - 1] == ' ' || out[n - 1] == '\0'))
        n--;
    out[n] = '\0';
    for (size_t i = 0; i < n; i++) {
        if (out[i] < 0x20 || out[i] > 0x7E)
            out[i] = '?';
    }
}

static void port_init(ahci_t *a, uint32_t number)
{
    volatile uint8_t *regs = a->regs + HBA_PORTS + 0x80 * number;
    uint32_t link = rd(regs, PORT_SSTS);
    char model[41];

    /* Device detected with an established link: DET = 3 */
    if ((link & 0xF) != 3) {
        if (link & 0xF)
            klog_info("ahci: port %u: device without a link (status 0x%x), ignored", number, link);
        return;
    }
    /* Firmware may leave the link in a power saving state (IPM = 2 or 6): wake it up. */
    if (((link >> 8) & 0xF) != 1) {
        wr(regs, PORT_CMD, (rd(regs, PORT_CMD) & ~CMD_ICC_MASK) | CMD_ICC_ACTIVE);
        for (int i = 0; i < 100 && ((rd(regs, PORT_SSTS) >> 8) & 0xF) != 1; i++)
            thread_sleep(1000000);
    }
    uint32_t signature = rd(regs, PORT_SIG);
    if (signature != SIGNATURE_SATA && signature != 0xFFFFFFFF && signature != 0) {
        klog_info("ahci: port %u: not a SATA disk (signature 0x%x), ignored", number, signature);
        return;
    }
    ahci_port_t *port = kcalloc(1, sizeof(*port));
    if (!port)
        return;
    port->controller = a;
    port->regs = regs;
    port->number = number;
    mutex_init(&port->lock);
    if (STATUS_IS_ERROR(dma_alloc(a->device, PAGE_SIZE, a->dma_limit, &port->memory)) ||
        STATUS_IS_ERROR(dma_alloc(a->device, BOUNCE_SIZE, a->dma_limit, &port->bounce))) {
        dma_free(&port->memory);
        kfree(port);
        return;
    }

    port_stop(regs);
    wr(regs, PORT_CLB, (uint32_t)port->memory.phys);
    wr(regs, PORT_CLBU, (uint32_t)(port->memory.phys >> 32));
    wr(regs, PORT_FB, (uint32_t)(port->memory.phys + 1024));
    wr(regs, PORT_FBU, (uint32_t)((port->memory.phys + 1024) >> 32));
    wr(regs, PORT_SERR, 0xFFFFFFFF);
    wr(regs, PORT_IS, 0xFFFFFFFF);
    wr(regs, PORT_CMD, rd(regs, PORT_CMD) | CMD_POWER_ON | CMD_SPIN_UP | CMD_FIS_RECEIVE);
    wr(regs, PORT_CMD, rd(regs, PORT_CMD) | CMD_START);
    a->ports[number] = port;
    format(port->block.name, sizeof(port->block.name), "ahci%u", disk_count);

    status_t status = run(port, ATA_IDENTIFY, 0, 0, SECTOR_SIZE, false);
    if (STATUS_IS_ERROR(status)) {
        /* A disk that does not answer gets a link reset (COMRESET) and a second chance. */
        klog_info("ahci: port %u: IDENTIFY failed (%s, task file 0x%x), resetting the link", number,
                  status_name(status), rd(regs, PORT_TFD));
        wr(regs, PORT_CMD, rd(regs, PORT_CMD) & ~CMD_START);
        wait_clear(regs, PORT_CMD, CMD_RUNNING, 500);
        wr(regs, PORT_SCTL, (rd(regs, PORT_SCTL) & ~0xFu) | 1);
        thread_sleep(2000000);
        wr(regs, PORT_SCTL, rd(regs, PORT_SCTL) & ~0xFu);
        for (int i = 0; i < 1000 && (rd(regs, PORT_SSTS) & 0xF) != 3; i++)
            thread_sleep(1000000);
        wr(regs, PORT_SERR, 0xFFFFFFFF);
        wait_clear(regs, PORT_TFD, TFD_BUSY | TFD_DRQ, 5000);
        wr(regs, PORT_CMD, rd(regs, PORT_CMD) | CMD_START);
        status = run(port, ATA_IDENTIFY, 0, 0, SECTOR_SIZE, false);
    }
    const uint16_t *id = port->bounce.virt;
    uint64_t sectors = 0;
    if (!STATUS_IS_ERROR(status)) {
        port->lba48 = id[83] & (1u << 10);
        sectors = port->lba48 ? ((uint64_t)id[103] << 48 | (uint64_t)id[102] << 32 | (uint64_t)id[101] << 16 | id[100])
                              : ((uint32_t)id[61] << 16 | id[60]);
        /* Word 106: bit 12 = the logical sector is larger than 512 bytes */
        if ((id[106] & 0xC000) == 0x4000 && (id[106] & (1u << 12)))
            status = STATUS_NOT_SUPPORTED;
    }
    if (STATUS_IS_ERROR(status) || !sectors || !port->lba48) {
        klog_warn("ahci: port %u: disk not usable (%s)", number,
                  STATUS_IS_ERROR(status) ? status_name(status) : "no 48-bit addressing");
        port_stop(regs);
        a->ports[number] = NULL;
        dma_free(&port->memory);
        dma_free(&port->bounce);
        kfree(port);
        return;
    }
    ata_text(model, sizeof(model), id + 27, 20);
    klog_info("ahci: port %u: %s, %lu MiB", number, model, sectors / (1024 * 1024 / SECTOR_SIZE));

    disk_count++;
    port->block.sector_size = SECTOR_SIZE;
    port->block.sector_count = sectors;
    port->block.read_only = !block_internal_disks_writable();
    port->block.ops = &ahci_ops;
    port->block.driver_data = port;
    block_register(&port->block);
}

static status_t ahci_probe(device_t *device)
{
    pci_device_t *pci = pci_from_device(device);

    /*
     * SATA controllers in AHCI mode; Intel controllers in RAID mode (class 01.04, "Intel RST") have the
     * same registers and their disks are plain SATA disks unless a RAID volume was actually created.
     */
    bool raid = device->id.subclass == 0x04;
    if (raid ? device->id.vendor != 0x8086 : device->id.prog_if != 0x01) {
        klog_info("ahci: %s: SATA controller in %s mode is not supported (switch it to AHCI in the firmware setup)",
                  device->name, raid ? "RAID" : "IDE");
        return STATUS_NOT_SUPPORTED;
    }
    ahci_t *a = kcalloc(1, sizeof(*a));
    if (!a)
        return STATUS_OUT_OF_MEMORY;
    a->device = device;
    wait_queue_init(&a->completion);

    status_t status = pci_enable_device(pci, true);
    if (!STATUS_IS_ERROR(status)) {
        a->regs = (volatile uint8_t *)pci_map_bar(pci, 5);
        if (!a->regs)
            status = STATUS_DEVICE_ERROR;
    }
    if (STATUS_IS_ERROR(status)) {
        kfree(a);
        return status;
    }

    /* Take the controller from the firmware, switch to AHCI mode. */
    if (rd(a->regs, HBA_CAP2) & CAP2_BOH) {
        wr(a->regs, HBA_BOHC, rd(a->regs, HBA_BOHC) | BOHC_OS_OWNED);
        wait_clear(a->regs, HBA_BOHC, BOHC_BIOS_OWNED, 1000);
    }
    wr(a->regs, HBA_GHC, rd(a->regs, HBA_GHC) | GHC_AHCI);
    a->dma_limit = (rd(a->regs, HBA_CAP) & CAP_64BIT) ? ~0ull : 0xFFFFFFFFull;

    if (STATUS_IS_ERROR(pci_enable_msi(pci, ahci_interrupt, a, &a->irq)))
        klog_warn("ahci: %s: no MSI interrupt, polling", device->name);
    else
        wr(a->regs, HBA_GHC, rd(a->regs, HBA_GHC) | GHC_INTERRUPTS);
    wr(a->regs, HBA_IS, 0xFFFFFFFF);
    device->driver_data = a;

    uint32_t implemented = rd(a->regs, HBA_PI), version = rd(a->regs, 0x10);
    klog_info("ahci: %s: AHCI %u.%u%s, ports 0x%x", device->name, version >> 16, (version >> 8) & 0xFF,
              raid ? " (RAID mode)" : "", implemented);
    for (uint32_t i = 0; i < MAX_PORTS; i++) {
        if (implemented & (1u << i))
            port_init(a, i);
    }
    return STATUS_SUCCESS;
}

static const device_match_t ahci_ids[] = {
    DEVICE_MATCH_CLASS(0x01, 0x06), /* mass storage, SATA */
    DEVICE_MATCH_CLASS(0x01, 0x04), /* mass storage, RAID (Intel) */
    DEVICE_MATCH_END,
};

static driver_t ahci_driver = {
    .name = "ahci",
    .bus_name = "pci",
    .version = 1,
    .capabilities = DRIVER_CAP_STORAGE,
    .ids = ahci_ids,
    .probe = ahci_probe,
};

static status_t ahci_module_init(void)
{
    return driver_register(&ahci_driver);
}

static const char *const ahci_dependencies[] = { "pci", NULL };

MODULE(.name = "ahci", .description = "SATA disks (AHCI)", .version = 1,
       .min_kernel_version = KERNEL_VERSION(0, 12, 0), .dependencies = ahci_dependencies, .init = ahci_module_init);
