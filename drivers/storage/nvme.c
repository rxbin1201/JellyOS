/*
 * NVMe driver (README section 25): solid-state disks on PCI Express.
 *
 * The controller is driven through pairs of queues in memory: commands go
 * into a submission queue, results come back in a completion queue, and
 * doorbell registers tell the controller how far each side has got. The
 * driver uses the admin queue pair for setup and one I/O queue pair for
 * data; both report through one interrupt (MSI-X or MSI).
 *
 * Requests are serialized by a mutex and go through a DMA bounce buffer of
 * up to 64 KiB (less if the controller's maximum transfer size is smaller),
 * so callers may pass any kernel buffer. Transfers of more than two pages
 * use a PRP list that is prepared once, since the bounce buffer never moves.
 *
 * Every active namespace with 512-byte blocks becomes a block device
 * "nvme<controller>n<namespace>".
 *
 * Not yet: several I/O queues, queue depth above 1, 4096-byte blocks,
 * power states, the shutdown notification (a flush is sent instead).
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

/* Controller registers */
#define REG_CAP   0x00 /* 64 bits */
#define REG_VS    0x08
#define REG_CC    0x14
#define REG_CSTS  0x1C
#define REG_AQA   0x24
#define REG_ASQ   0x28
#define REG_ACQ   0x30
#define REG_DOORBELLS 0x1000

#define CC_ENABLE     (1u << 0)
#define CC_IOSQES_64  (6u << 16) /* submission entries of 2^6 bytes */
#define CC_IOCQES_16  (4u << 20)
#define CSTS_READY    (1u << 0)
#define CSTS_FATAL    (1u << 1)

/* Admin commands */
#define ADMIN_CREATE_SQ    0x01
#define ADMIN_CREATE_CQ    0x05
#define ADMIN_IDENTIFY     0x06
#define ADMIN_SET_FEATURES 0x09
/* I/O commands */
#define IO_FLUSH 0x00
#define IO_WRITE 0x01
#define IO_READ  0x02

#define IDENTIFY_NAMESPACE  0
#define IDENTIFY_CONTROLLER 1
#define IDENTIFY_ACTIVE_NAMESPACES 2
#define FEATURE_QUEUE_COUNT 7

#define QUEUE_ENTRIES   64
#define BOUNCE_PAGES    16
#define SECTOR_SIZE     512
#define MAX_NAMESPACES  4
#define COMMAND_TIMEOUT_NS 10000000000ull
#define POLL_NS         10000000ull /* look at the queue this often even without an interrupt */

typedef struct {
    uint32_t opcode_cid; /* opcode, flags, command ID in the upper 16 bits */
    uint32_t nsid;
    uint64_t reserved;
    uint64_t metadata;
    uint64_t prp1, prp2;
    uint32_t cdw10, cdw11, cdw12, cdw13, cdw14, cdw15;
} nvme_command_t;

typedef struct {
    uint32_t result;
    uint32_t reserved;
    uint16_t sq_head, sq_id;
    uint16_t command_id;
    uint16_t status; /* bit 0: phase, the rest: status code (0 = success) */
} nvme_completion_t;

typedef struct {
    dma_buffer_t       sq_memory, cq_memory;
    nvme_command_t    *sq;
    volatile nvme_completion_t *cq;
    volatile uint32_t *sq_doorbell, *cq_doorbell;
    uint32_t           tail, head, phase;
    uint16_t           next_id;
} nvme_queue_t;

struct nvme;

typedef struct {
    struct nvme   *controller;
    uint32_t       nsid;
    block_device_t block;
} nvme_namespace_t;

typedef struct nvme {
    device_t         *device;
    volatile uint8_t *regs;
    uint32_t          number;
    uint32_t          doorbell_stride;
    uint32_t          irq;
    mutex_t           lock;
    wait_queue_t      completion;
    nvme_queue_t      admin, io;
    dma_buffer_t      bounce, prp_list, identify;
    uint32_t          max_sectors;  /* per command */
    nvme_namespace_t  namespaces[MAX_NAMESPACES];
} nvme_t;

static uint32_t controller_count;

static uint32_t read32(nvme_t *n, uint32_t reg)
{
    return *(volatile uint32_t *)(n->regs + reg);
}

static void write32(nvme_t *n, uint32_t reg, uint32_t value)
{
    *(volatile uint32_t *)(n->regs + reg) = value;
}

static void write64(nvme_t *n, uint32_t reg, uint64_t value)
{
    write32(n, reg, (uint32_t)value);
    write32(n, reg + 4, (uint32_t)(value >> 32));
}

static bool wait_ready(nvme_t *n, bool ready, uint32_t timeout_ms)
{
    for (uint32_t waited = 0;; waited++) {
        uint32_t status = read32(n, REG_CSTS);
        if (((status & CSTS_READY) != 0) == ready)
            return true;
        if ((status & CSTS_FATAL) || waited >= timeout_ms)
            return false;
        thread_sleep(1000000);
    }
}

static void nvme_interrupt(void *context)
{
    nvme_t *n = context;
    wait_queue_wake_all(&n->completion, STATUS_SUCCESS);
}

/* --- Queues ---------------------------------------------------------------------- */

static status_t queue_alloc(nvme_t *n, nvme_queue_t *q, uint32_t id)
{
    status_t status = dma_alloc(n->device, QUEUE_ENTRIES * sizeof(nvme_command_t), ~0ull, &q->sq_memory);
    if (!STATUS_IS_ERROR(status))
        status = dma_alloc(n->device, QUEUE_ENTRIES * sizeof(nvme_completion_t), ~0ull, &q->cq_memory);
    if (STATUS_IS_ERROR(status))
        return status;
    q->sq = q->sq_memory.virt;
    q->cq = q->cq_memory.virt;
    q->sq_doorbell = (volatile uint32_t *)(n->regs + REG_DOORBELLS + (2 * id) * n->doorbell_stride);
    q->cq_doorbell = (volatile uint32_t *)(n->regs + REG_DOORBELLS + (2 * id + 1) * n->doorbell_stride);
    q->tail = q->head = 0;
    q->phase = 1;
    return STATUS_SUCCESS;
}

static void queue_free(nvme_queue_t *q)
{
    dma_free(&q->sq_memory);
    dma_free(&q->cq_memory);
}

/* Run one command and wait for its completion. n->lock held (or still single-threaded in probe). */
static status_t run(nvme_t *n, nvme_queue_t *q, nvme_command_t *command, uint32_t *result)
{
    uint16_t id = q->next_id++;

    command->opcode_cid = (command->opcode_cid & 0xFFFF) | (uint32_t)id << 16;
    q->sq[q->tail] = *command;
    q->tail = (q->tail + 1) % QUEUE_ENTRIES;
    *q->sq_doorbell = q->tail;

    /* The interrupt wakes us; the short deadline makes it work without one, too. */
    uint64_t deadline = wait_deadline(COMMAND_TIMEOUT_NS);
    uint64_t flags = arch_interrupts_save();
    while ((q->cq[q->head].status & 1) != q->phase) {
        uint64_t next = wait_deadline(POLL_NS);
        wait_queue_block_uninterruptible(&n->completion, next < deadline ? next : deadline);
        if (wait_deadline(0) >= deadline && (q->cq[q->head].status & 1) != q->phase) {
            arch_interrupts_restore(flags);
            klog_error("nvme%u: command 0x%x timed out (status register 0x%x)", n->number, command->opcode_cid & 0xFF,
                       read32(n, REG_CSTS));
            return STATUS_TIMEOUT;
        }
    }
    arch_interrupts_restore(flags);

    nvme_completion_t done = q->cq[q->head];
    if (++q->head == QUEUE_ENTRIES) {
        q->head = 0;
        q->phase ^= 1;
    }
    *q->cq_doorbell = q->head;

    if (done.command_id != id || (done.status >> 1)) {
        klog_debug("nvme%u: command 0x%x failed: status 0x%x", n->number, command->opcode_cid & 0xFF, done.status >> 1);
        return STATUS_IO_ERROR;
    }
    if (result)
        *result = done.result;
    return STATUS_SUCCESS;
}

static status_t identify(nvme_t *n, uint32_t what, uint32_t nsid)
{
    nvme_command_t c = { .opcode_cid = ADMIN_IDENTIFY, .nsid = nsid, .prp1 = n->identify.phys, .cdw10 = what };
    memset(n->identify.virt, 0, PAGE_SIZE);
    return run(n, &n->admin, &c, NULL);
}

/* --- Block device ---------------------------------------------------------------- */

static status_t transfer(nvme_namespace_t *ns, uint32_t opcode, uint64_t lba, uint32_t count, void *buffer)
{
    nvme_t *n = ns->controller;
    uint8_t *data = buffer;
    status_t status = STATUS_SUCCESS;

    mutex_lock(&n->lock);
    while (count && !STATUS_IS_ERROR(status)) {
        uint32_t chunk = count < n->max_sectors ? count : n->max_sectors;
        uint32_t bytes = chunk * SECTOR_SIZE;

        if (opcode == IO_WRITE)
            memcpy(n->bounce.virt, data, bytes);
        nvme_command_t c = {
            .opcode_cid = opcode,
            .nsid = ns->nsid,
            .prp1 = n->bounce.phys,
            .cdw10 = (uint32_t)lba,
            .cdw11 = (uint32_t)(lba >> 32),
            .cdw12 = chunk - 1,
        };
        /* The second pointer is the second page, or a list of all further pages. */
        if (bytes > 2 * PAGE_SIZE)
            c.prp2 = n->prp_list.phys;
        else if (bytes > PAGE_SIZE)
            c.prp2 = n->bounce.phys + PAGE_SIZE;
        status = run(n, &n->io, &c, NULL);
        if (!STATUS_IS_ERROR(status) && opcode == IO_READ)
            memcpy(data, n->bounce.virt, bytes);
        data += bytes;
        lba += chunk;
        count -= chunk;
    }
    mutex_unlock(&n->lock);
    return status;
}

static status_t nvme_read(block_device_t *block, uint64_t lba, uint32_t count, void *buffer)
{
    return transfer(block->driver_data, IO_READ, lba, count, buffer);
}

static status_t nvme_write(block_device_t *block, uint64_t lba, uint32_t count, const void *buffer)
{
    return transfer(block->driver_data, IO_WRITE, lba, count, (void *)buffer);
}

static status_t nvme_flush(block_device_t *block)
{
    nvme_namespace_t *ns = block->driver_data;
    nvme_t *n = ns->controller;
    nvme_command_t c = { .opcode_cid = IO_FLUSH, .nsid = ns->nsid };

    mutex_lock(&n->lock);
    status_t status = run(n, &n->io, &c, NULL);
    mutex_unlock(&n->lock);
    return status;
}

static const block_ops_t nvme_ops = {
    .read = nvme_read,
    .write = nvme_write,
    .flush = nvme_flush,
};

/* --- Controller start -------------------------------------------------------------- */

/* Copy an identify string (space padded, not terminated) without the padding. */
static void identify_text(char *out, size_t size, const uint8_t *field, size_t length)
{
    while (length && (field[length - 1] == ' ' || field[length - 1] == '\0'))
        length--;
    if (length > size - 1)
        length = size - 1;
    for (size_t i = 0; i < length; i++)
        out[i] = field[i] >= 0x20 && field[i] < 0x7F ? (char)field[i] : '?';
    out[length] = '\0';
}

static void add_namespace(nvme_t *n, nvme_namespace_t *ns, uint32_t nsid)
{
    if (STATUS_IS_ERROR(identify(n, IDENTIFY_NAMESPACE, nsid)))
        return;
    const uint8_t *data = n->identify.virt;
    uint64_t blocks;
    uint32_t format_entry;
    memcpy(&blocks, data, sizeof(blocks));                                   /* NSZE */
    memcpy(&format_entry, data + 128 + 4 * (data[26] & 0x0F), sizeof(format_entry)); /* the LBA format in use */
    uint32_t block_size = 1u << ((format_entry >> 16) & 0xFF);

    if (!blocks)
        return;
    if (block_size != SECTOR_SIZE) {
        klog_warn("nvme%u: namespace %u has %u-byte blocks, only %u are supported", n->number, nsid, block_size,
                  SECTOR_SIZE);
        return;
    }
    ns->controller = n;
    ns->nsid = nsid;
    format(ns->block.name, sizeof(ns->block.name), "nvme%un%u", n->number, nsid);
    ns->block.sector_size = SECTOR_SIZE;
    ns->block.sector_count = blocks;
    ns->block.read_only = !block_internal_disks_writable();
    ns->block.ops = &nvme_ops;
    ns->block.driver_data = ns;
    klog_info("nvme%u: namespace %u: %lu MiB", n->number, nsid, blocks / (1024 * 1024 / SECTOR_SIZE));
    block_register(&ns->block);
}

static status_t controller_start(nvme_t *n, pci_device_t *pci)
{
    uint64_t cap = read32(n, REG_CAP) | (uint64_t)read32(n, REG_CAP + 4) << 32;
    uint32_t timeout_ms = (uint32_t)((cap >> 24) & 0xFF) * 500 + 500;
    char model[41];

    if (((cap >> 48) & 0xF) > 0) { /* MPSMIN: the smallest page size is larger than 4 KiB */
        klog_error("nvme%u: the controller needs memory pages above 4 KiB", n->number);
        return STATUS_NOT_SUPPORTED;
    }
    n->doorbell_stride = 4u << ((cap >> 32) & 0xF);

    /* Disable, set up the admin queues, enable. */
    write32(n, REG_CC, read32(n, REG_CC) & ~CC_ENABLE);
    if (!wait_ready(n, false, timeout_ms))
        return STATUS_TIMEOUT;
    status_t status = queue_alloc(n, &n->admin, 0);
    if (!STATUS_IS_ERROR(status))
        status = queue_alloc(n, &n->io, 1);
    if (!STATUS_IS_ERROR(status))
        status = dma_alloc(n->device, BOUNCE_PAGES * PAGE_SIZE, ~0ull, &n->bounce);
    if (!STATUS_IS_ERROR(status))
        status = dma_alloc(n->device, PAGE_SIZE, ~0ull, &n->prp_list);
    if (!STATUS_IS_ERROR(status))
        status = dma_alloc(n->device, PAGE_SIZE, ~0ull, &n->identify);
    if (STATUS_IS_ERROR(status))
        return status;
    for (uint32_t i = 1; i < BOUNCE_PAGES; i++)
        ((uint64_t *)n->prp_list.virt)[i - 1] = n->bounce.phys + i * PAGE_SIZE;

    write32(n, REG_AQA, (QUEUE_ENTRIES - 1) << 16 | (QUEUE_ENTRIES - 1));
    write64(n, REG_ASQ, n->admin.sq_memory.phys);
    write64(n, REG_ACQ, n->admin.cq_memory.phys);
    write32(n, REG_CC, CC_ENABLE | CC_IOSQES_64 | CC_IOCQES_16); /* NVM command set, 4 KiB pages */
    if (!wait_ready(n, true, timeout_ms))
        return STATUS_TIMEOUT;

    /* Without an interrupt the driver still works by polling. */
    if (STATUS_IS_ERROR(pci_enable_msix(pci, 0, nvme_interrupt, n, &n->irq)) &&
        STATUS_IS_ERROR(pci_enable_msi(pci, nvme_interrupt, n, &n->irq)))
        klog_warn("nvme%u: no MSI or MSI-X interrupt, polling", n->number);

    status = identify(n, IDENTIFY_CONTROLLER, 0);
    if (STATUS_IS_ERROR(status))
        return status;
    const uint8_t *data = n->identify.virt;
    identify_text(model, sizeof(model), data + 24, 40);
    /* MDTS: the largest transfer is 2^MDTS pages (0: no limit) */
    uint32_t max_bytes = BOUNCE_PAGES * PAGE_SIZE;
    if (data[77] && data[77] < 16 && (PAGE_SIZE << data[77]) < max_bytes)
        max_bytes = (uint32_t)(PAGE_SIZE << data[77]);
    n->max_sectors = max_bytes / SECTOR_SIZE;
    uint32_t version = read32(n, REG_VS);
    klog_info("nvme%u: %s (NVMe %u.%u, up to %u KiB per command)", n->number, model, version >> 16,
              (version >> 8) & 0xFF, max_bytes / 1024);

    /* One I/O queue pair; controllers want to be asked for the number first. */
    nvme_command_t count = { .opcode_cid = ADMIN_SET_FEATURES, .cdw10 = FEATURE_QUEUE_COUNT, .cdw11 = 0 };
    run(n, &n->admin, &count, NULL);
    nvme_command_t create_cq = {
        .opcode_cid = ADMIN_CREATE_CQ,
        .prp1 = n->io.cq_memory.phys,
        .cdw10 = (QUEUE_ENTRIES - 1) << 16 | 1,
        .cdw11 = 0x3, /* contiguous, interrupts enabled, vector 0 */
    };
    status = run(n, &n->admin, &create_cq, NULL);
    if (STATUS_IS_ERROR(status))
        return status;
    nvme_command_t create_sq = {
        .opcode_cid = ADMIN_CREATE_SQ,
        .prp1 = n->io.sq_memory.phys,
        .cdw10 = (QUEUE_ENTRIES - 1) << 16 | 1,
        .cdw11 = 1u << 16 | 0x1, /* completion queue 1, contiguous */
    };
    status = run(n, &n->admin, &create_sq, NULL);
    if (STATUS_IS_ERROR(status))
        return status;

    /* Namespaces: the list of active IDs (older controllers: just try namespace 1). */
    uint32_t ids[MAX_NAMESPACES] = { 1 };
    uint32_t found = 1;
    if (!STATUS_IS_ERROR(identify(n, IDENTIFY_ACTIVE_NAMESPACES, 0))) {
        const uint32_t *list = n->identify.virt;
        for (found = 0; found < MAX_NAMESPACES && list[found]; found++)
            ids[found] = list[found];
        if (!found) {
            ids[0] = 1;
            found = 1;
        }
    }
    for (uint32_t i = 0; i < found; i++)
        add_namespace(n, &n->namespaces[i], ids[i]);
    return STATUS_SUCCESS;
}

static status_t nvme_probe(device_t *device)
{
    pci_device_t *pci = pci_from_device(device);

    if (device->id.prog_if != 0x02) /* NVM Express I/O controller */
        return STATUS_NOT_SUPPORTED;
    nvme_t *n = kcalloc(1, sizeof(*n));
    if (!n)
        return STATUS_OUT_OF_MEMORY;
    n->device = device;
    n->number = controller_count;
    mutex_init(&n->lock);
    wait_queue_init(&n->completion);

    status_t status = pci_enable_device(pci, true);
    if (!STATUS_IS_ERROR(status)) {
        n->regs = (volatile uint8_t *)pci_map_bar(pci, 0);
        status = n->regs ? controller_start(n, pci) : STATUS_DEVICE_ERROR;
    }
    if (STATUS_IS_ERROR(status)) {
        klog_error("nvme: %s: %s", device->name, status_name(status));
        if (n->regs)
            write32(n, REG_CC, 0);
        pci_disable_msix(pci);
        pci_disable_msi(pci);
        queue_free(&n->admin);
        queue_free(&n->io);
        dma_free(&n->bounce);
        dma_free(&n->prp_list);
        dma_free(&n->identify);
        kfree(n);
        return status;
    }
    controller_count++;
    device->driver_data = n;
    return STATUS_SUCCESS;
}

static const device_match_t nvme_ids[] = {
    DEVICE_MATCH_CLASS(0x01, 0x08), /* mass storage, non-volatile memory */
    DEVICE_MATCH_END,
};

static driver_t nvme_driver = {
    .name = "nvme",
    .bus_name = "pci",
    .version = 1,
    .capabilities = DRIVER_CAP_STORAGE,
    .ids = nvme_ids,
    .probe = nvme_probe,
};

static status_t nvme_module_init(void)
{
    return driver_register(&nvme_driver);
}

static const char *const nvme_dependencies[] = { "pci", NULL };

MODULE(.name = "nvme", .description = "NVMe solid-state disks", .version = 1,
       .min_kernel_version = KERNEL_VERSION(0, 12, 0), .dependencies = nvme_dependencies, .init = nvme_module_init);
