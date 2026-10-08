/*
 * Intel integrated graphics, generation 9 (Skylake to Comet Lake): the GT,
 * the part of the GPU that executes commands. The display engine next to
 * it (intel_gpu.c) puts a picture on a monitor; this part is what can draw
 * one. Everything that is called acceleration builds on it.
 *
 * What is here: the GT is woken, two of its engines are brought up,
 * commands can be given to them and waited for, memory can be entered into
 * the address space they draw in, and the blitter fills and copies
 * rectangles.
 *
 *   forcewake   The GT sleeps whenever nobody uses it (RC6) and its
 *               registers are then not there. A driver holds it awake by
 *               setting a bit per power domain and waiting for the
 *               acknowledgement. This driver sets them once and keeps them:
 *               no power saving of the GT yet.
 *
 *   clock       The GT's clock can run at a range of rates, and the
 *               firmware leaves it at a slow one: everything an engine
 *               does takes about three times as long as it has to. The
 *               highest rate is asked for once (the old JellyOS driver
 *               found this on the same machine). No scaling with the load
 *               yet.
 *
 *   engines     Each executes one kind of commands from a ring buffer: the
 *               render engine (3D, and the only one that can blend) and
 *               the blitter (copy and fill rectangles). Each is reset
 *               first, since the firmware never used it.
 *
 *   memory      The GPU reads everything it is given through translation
 *               tables. The global one (GGTT) is the display driver's: the
 *               framebuffers are in it, and it lends room for an engine's
 *               status page, context and ring. What the engines draw in
 *               and read from is in an address space of the contexts' own
 *               (PPGTT): four levels of tables like the CPU's, kept here.
 *               Memory is entered in pieces of 2 MiB of address space, so
 *               that each piece has its last-level tables to itself and
 *               they go when it goes; whatever is not entered leads to one
 *               scratch page.
 *
 *   contexts    An engine runs a context: its ring and its complete
 *               register state, kept in a context image that the hardware
 *               loads and saves. The image starts with a list of register
 *               writes at fixed places (the tables `*_layout` below, after
 *               i915's intel_lrc.c). The first load is told to leave the
 *               engine's state as it is ("restore inhibit"); the save that
 *               follows fills the image with the hardware's own values.
 *
 *   submission  A context is handed to an engine by writing its descriptor
 *               to the engine's submit port ("execlists"). The engine runs
 *               it until its ring is empty and switches it out again,
 *               which it reports in a small status buffer. Only then may
 *               the image be touched: the next commands go into the ring,
 *               the new tail into the image, the descriptor to the port.
 *               A context's tables are read anew with every load, so what
 *               was entered into the address space since is there.
 *
 *   batches     Commands that draw are not put into the ring but into a
 *               buffer of their own in the contexts' address space, and
 *               the ring only says "go there". Commands in the ring are
 *               privileged (they may write anywhere); those in a batch are
 *               not.
 *
 *   done        Every submission ends with a command that writes a
 *               sequence number into the engine's status page, and an
 *               interrupt. A caller sleeps until the number is there and
 *               the context is switched out. Where the interrupt does not
 *               arrive, it looks every two milliseconds instead.
 *
 * One context per engine, one submission at a time. After Linux's i915
 * (gt/intel_lrc.c, intel_execlists_submission.c, intel_reset.c,
 * intel_uncore.c, gen8_ppgtt.c, gen8_engine_cs.c).
 *
 * QEMU has no such device: this can only be tested on real hardware.
 * Developed on a Core i5-8400T with UHD Graphics 630.
 */

#include "drivers/graphics/intel_gt.h"

#include "core/arch.h"
#include "core/log.h"
#include "core/string.h"
#include "memory/heap.h"
#include "memory/layout.h"
#include "memory/pmm.h"
#include "scheduler/mutex.h"
#include "scheduler/thread.h"
#include "scheduler/wait.h"
#include "time/clock.h"

/* --- Registers ------------------------------------------------------------------------- */

/* Forcewake: write (bit << 16 | bit) to set a bit, (bit << 16) to clear it; the acknowledgement mirrors it */
#define FORCEWAKE_RENDER      0x0A278
#define FORCEWAKE_RENDER_ACK  0x00D84
#define FORCEWAKE_GT          0x0A188 /* the blitter and what the engines share */
#define FORCEWAKE_GT_ACK      0x130044
#define FORCEWAKE_KERNEL      1u
#define RC_CONTROL            0x0A090 /* how the GT is allowed to sleep */
#define RC_STATE              0x0A094
/* The GT's clock, in units of 50/3 MHz in bits 31:23 */
#define RP_REQUEST            0x0A008 /* the rate that is asked for */
#define RP_STATUS             0x0A01C /* the rate it runs at */
#define RP_CAPABILITY         0x145998 /* in units of 50 MHz: bits 7:0 the highest, 23:16 the lowest rate */
#define GDRST                 0x0941C /* reset of single engines: a bit each, clears itself */
#define GDRST_RENDER          (1u << 1)
#define GDRST_BLITTER         (1u << 3)
#define PRIVATE_PAT_LOW       0x040E0 /* what the "memory type" bits of a translation table entry mean: eight types */
#define PRIVATE_PAT_HIGH      0x040E4
#define GT_FAULT              0x04094 /* a page fault of an engine */
#define GT_ERROR              0x040A0
/* The engines' interrupts: render in the low half, blitter in the high half of each register */
#define GT_INTERRUPT_MASK     0x44304
#define GT_INTERRUPT_CAUSE    0x44308
#define GT_INTERRUPT_ENABLE   0x4430C
#define ENGINE_USER_INTERRUPT (1u << 0) /* the command MI_USER_INTERRUPT was executed */
#define ENGINE_CONTEXT_SWITCH (1u << 8) /* a status event was written */

/* An engine's registers, from its base */
#define RING_TAIL             0x030
#define RING_HEAD             0x034
#define RING_START            0x038
#define RING_CTL              0x03C
#define RING_ACTHD            0x074 /* where the engine is executing */
#define RING_IPEIR            0x064
#define RING_IPEHR            0x068 /* the command it executed last */
#define RING_INSTDONE         0x06C
#define RING_HWS_PGA          0x080 /* the status page's address */
#define RING_HWSTAM           0x098
#define RING_MI_MODE          0x09C
#define RING_IMR              0x0A8
#define RING_EIR              0x0B0
#define RING_EMR              0x0B4
#define RING_ESR              0x0B8
#define RING_RESET_CTL        0x0D0
#define RING_ELSP             0x230 /* the submit port: four writes, two context descriptors */
#define RING_EXECLIST_STATUS  0x234
#define RING_MODE             0x29C
#define RING_CSB(i)           (0x370 + 8u * (uint32_t)(i)) /* status events: what happened, then to which context */
#define RING_CSB_POINTERS     0x3A0 /* bits 2:0 the last event written, 10:8 the last one read */

#define MASKED_ON(bits)       ((uint32_t)(bits) << 16 | (uint32_t)(bits))
#define MASKED_OFF(bits)      ((uint32_t)(bits) << 16)

#define MI_MODE_STOP_RING     (1u << 8)
#define MI_MODE_IDLE          (1u << 9)
#define RESET_REQUEST         (1u << 0)
#define RESET_READY           (1u << 1)
#define MODE_RUN_LIST         (1u << 15) /* contexts through the submit port, not the ring registers */
#define RING_CTL_VALID        1u
#define CSB_ENTRIES           6
#define CSB_IDLE_TO_ACTIVE    (1u << 0)
#define CSB_ACTIVE_TO_IDLE    (1u << 3)
#define CSB_COMPLETE          (1u << 4)

/* A context's descriptor, lower half: its image's address and these */
#define DESCRIPTOR_VALID      (1u << 0)
#define DESCRIPTOR_48_BIT     (3u << 3) /* a four level address space of its own */
#define DESCRIPTOR_PRIVILEGED (1u << 8)

/* Commands */
#define MI_NOOP               0u
#define MI_USER_INTERRUPT     (0x02u << 23)
#define MI_BATCH_BUFFER_END   (0x0Au << 23)
#define MI_STORE_DWORD_GGTT   (0x20u << 23 | 1u << 22 | 2) /* address (two dwords), value */
#define MI_LOAD_REGISTERS(n)  (0x22u << 23 | (2u * (n) - 1))
#define MI_LRI_FORCE_POSTED   (1u << 12)
#define MI_FLUSH_STORE        (0x26u << 23 | 1u << 14 | 2) /* what was drawn is in memory, then store: address, value */
#define MI_FLUSH_ADDRESS_GGTT (1u << 2)
#define MI_BATCH_START        (0x31u << 23 | 1u << 8 | 1)  /* go on at an address of the context's own address space */
/* The blitter's two: pixels of 32 bits, all four bytes written */
#define XY_COLOR_BLT          (2u << 29 | 0x50u << 22 | 3u << 20 | 5) /* 7 dwords */
#define XY_SRC_COPY_BLT       (2u << 29 | 0x53u << 22 | 3u << 20 | 8) /* 10 dwords */
#define BLT_DEPTH_32          (3u << 24)
#define BLT_ROP_COLOR         (0xF0u << 16)
#define BLT_ROP_COPY          (0xCCu << 16)
#define BLT_MAX               32767 /* coordinates and line lengths are 16 bits with a sign */

/* The page of registers in a context image (its second page), in dwords */
#define CTX_CONTEXT_CONTROL   0x03
#define CTX_RING_HEAD         0x05
#define CTX_RING_TAIL         0x07
#define CTX_RING_START        0x09
#define CTX_RING_CTL          0x0B
#define CTX_PDP0_UPPER        0x31 /* with four levels: the top table */
#define CTX_PDP0_LOWER        0x33
#define CTX_MI_MODE           0x55
#define CTX_RESTORE_INHIBIT   (1u << 0) /* leave the engine's state as it is when the context is loaded */
#define CTX_RS_ENABLE         (1u << 1)
#define CTX_SAVE_INHIBIT      (1u << 2)
#define CTX_NO_SYNC_SWITCH    (1u << 3)

/* The status page, in dwords: where our sequence number goes (the hardware's own entries are below 0x30) */
#define HWSP_SEQNO            0x40
#define HWSP_SCRATCH          0x80

#define RING_PAGES            4
#define RING_BYTES            (RING_PAGES * PAGE_SIZE)
#define BREADCRUMB_DWORDS     6
#define BATCH_START_DWORDS    4
#define BATCH_PAGES           1
#define BATCH_DWORDS          (BATCH_PAGES * PAGE_SIZE / 4)

/* The contexts' address space */
#define PTE_PRESENT           1ull
#define PTE_WRITABLE          2ull
#define PTE_UNCACHED          (3ull << 3) /* memory type 3: straight to memory, for what a display reads */
#define PTE_CACHED            (1ull << 7) /* memory type 4: in the cache the CPU shares */
#define PTE_ADDRESS           0x0000FFFFFFFFF000ull
#define SPACE_BASE            (4ull << 30) /* nothing below 4 GiB: no address here can be taken for one of the GGTT */
#define SPACE_GIB             64
#define SLOT_BYTES            (2ull << 20) /* what one last-level table covers */
#define SLOT_PAGES            512
#define SPACE_SLOTS           (SPACE_GIB * 512)

/*
 * The registers of a context image's first page, as the hardware lays them out: SKIP(n) dwords left alone, then
 * a "load registers" command for the n registers that follow (their offsets from the engine's base; the values
 * are filled in elsewhere or by the hardware). 0 ends the list.
 */
#define SKIP(n)   (0x8000 | (n))
#define LOAD(n)   (0x4000 | (n))
#define LOAD_P(n) (0x2000 | (n)) /* with "force posted" */

static const uint16_t blitter_layout[] = {
    SKIP(1), LOAD_P(14), 0x244, 0x034, 0x030, 0x038, 0x03C, 0x168, 0x140, 0x110, 0x11C, 0x114, 0x118, 0x1C0, 0x1C4, 0x1C8,
    SKIP(3), LOAD_P(9),  0x3A8, 0x28C, 0x288, 0x284, 0x280, 0x27C, 0x278, 0x274, 0x270,
    SKIP(13), LOAD_P(1), 0x200,
    SKIP(13), LOAD_P(44), 0x028, 0x09C, 0x0C0, 0x178, 0x17C, 0x358, 0x170, 0x150, 0x154, 0x158, 0x41C,
    0x600, 0x604, 0x608, 0x60C, 0x610, 0x614, 0x618, 0x61C, 0x620, 0x624, 0x628, 0x62C, 0x630, 0x634, 0x638, 0x63C,
    0x640, 0x644, 0x648, 0x64C, 0x650, 0x654, 0x658, 0x65C, 0x660, 0x664, 0x668, 0x66C, 0x670, 0x674, 0x678, 0x67C,
    0x068, 0,
};

static const uint16_t render_layout[] = {
    SKIP(1), LOAD_P(14), 0x244, 0x034, 0x030, 0x038, 0x03C, 0x168, 0x140, 0x110, 0x11C, 0x114, 0x118, 0x1C0, 0x1C4, 0x1C8,
    SKIP(3), LOAD_P(9),  0x3A8, 0x28C, 0x288, 0x284, 0x280, 0x27C, 0x278, 0x274, 0x270,
    SKIP(13), LOAD(1),   0x0C8,
    SKIP(13), LOAD_P(44), 0x028, 0x09C, 0x0C0, 0x178, 0x17C, 0x358, 0x170, 0x150, 0x154, 0x158, 0x41C,
    0x600, 0x604, 0x608, 0x60C, 0x610, 0x614, 0x618, 0x61C, 0x620, 0x624, 0x628, 0x62C, 0x630, 0x634, 0x638, 0x63C,
    0x640, 0x644, 0x648, 0x64C, 0x650, 0x654, 0x658, 0x65C, 0x660, 0x664, 0x668, 0x66C, 0x670, 0x674, 0x678, 0x67C,
    0x068, 0,
};

typedef struct {
    const char        *name;
    uint32_t           base;
    uint32_t           reset_bit;
    uint32_t           context_pages; /* the image: a status page of the context's own, the registers, the engine's state */
    const uint16_t    *layout;
    uint32_t           interrupt_shift; /* its half of the interrupt registers */
    bool               flushes;       /* it draws: a submission ends with "flush, then store" */

    mutex_t            lock;
    bool               works;
    uint32_t           hwsp_gtt, context_gtt, ring_gtt; /* addresses in the global graphics address space */
    volatile uint32_t *hwsp;
    uint32_t          *registers;     /* the image's page of registers */
    uint32_t          *ring;
    uint32_t           tail;          /* bytes */
    uint32_t           seqno;
    uint32_t           csb_head;
    uint32_t           submissions;
    uint32_t           events[4];     /* the status events of the last submission, for the log */
    uint32_t           event_count;
    uint64_t           batch_phys, batch_address; /* its batch buffer, in memory and in the contexts' address space */
    uint32_t          *batch;
} engine_t;

struct intel_gt {
    intel_gt_host_t   host;
    engine_t          engines[INTEL_ENGINE_COUNT];

    /* The contexts' address space */
    mutex_t           space_lock;
    uint64_t          tables_phys;    /* top table, three scratch tables (one per level below), the scratch page */
    uint64_t          directory_phys; /* the table of the first 512 GiB, which holds all we use */
    uint64_t          directories[SPACE_GIB]; /* the tables of each GiB of our part, 0 until needed */
    uint8_t           slots[SPACE_SLOTS / 8]; /* a bit for each 2 MiB that is taken */

    /* Interrupts */
    bool              interrupts;     /* waiting sleeps until one comes */
    volatile uint64_t interrupt_count;
    wait_queue_t      waiters;
};

static uint32_t rd(const intel_gt_t *gt, uint32_t reg)
{
    return *(volatile uint32_t *)(gt->host.regs + reg);
}

static void wr(const intel_gt_t *gt, uint32_t reg, uint32_t value)
{
    *(volatile uint32_t *)(gt->host.regs + reg) = value;
}

static uint64_t now_ms(void)
{
    return clock_monotonic_ns() / 1000000;
}

/* Poll until (reg & mask) == want. The clock ticks in milliseconds, so short waits round up. */
static bool wait_bits(const intel_gt_t *gt, uint32_t reg, uint32_t mask, uint32_t want, uint32_t ms)
{
    uint64_t end = now_ms() + ms + 1;

    while ((rd(gt, reg) & mask) != want) {
        if (now_ms() > end)
            return false;
    }
    return true;
}

/* --- Forcewake --------------------------------------------------------------------------- */

static bool forcewake(const intel_gt_t *gt, uint32_t reg, uint32_t ack, const char *name)
{
    /* Whatever the firmware left set is cleared first (as i915 does at its start). */
    wr(gt, reg, MASKED_OFF(0xFFFF));
    wait_bits(gt, ack, FORCEWAKE_KERNEL, 0, 5);
    wr(gt, reg, MASKED_ON(FORCEWAKE_KERNEL));
    if (!wait_bits(gt, ack, FORCEWAKE_KERNEL, FORCEWAKE_KERNEL, 50)) {
        klog_warn("igpu: the %s part of the GPU does not wake (acknowledgement 0x%x)", name, rd(gt, ack));
        return false;
    }
    return true;
}

uint32_t intel_gt_clock_mhz(const intel_gt_t *gt)
{
    return gt ? ((rd(gt, RP_STATUS) >> 23) & 0x1FF) * 50 / 3 : 0;
}

/* --- Interrupts -------------------------------------------------------------------------- */

void intel_gt_interrupt(intel_gt_t *gt)
{
    uint32_t cause = rd(gt, GT_INTERRUPT_CAUSE);

    if (!cause)
        return;
    wr(gt, GT_INTERRUPT_CAUSE, cause);
    gt->interrupt_count++;
    wait_queue_wake_all(&gt->waiters, STATUS_SUCCESS);
}

/* --- The contexts' address space ------------------------------------------------------------ */

static uint64_t *table(uint64_t phys)
{
    return phys_to_virt(phys);
}

/* The scratch table of a level (1: for a GiB, 2: for 2 MiB) or the scratch page (3), as an entry. */
static uint64_t scratch_entry(const intel_gt_t *gt, uint32_t level)
{
    return (gt->tables_phys + (uint64_t)(level + 1) * PAGE_SIZE) | PTE_PRESENT | PTE_WRITABLE | (level == 3 ? PTE_CACHED : 0);
}

/*
 * An address space in which every address leads to one page that nobody reads: the top table, a scratch table
 * for each level below it, and the page. Then the table of the first 512 GiB as a real one, still all scratch.
 */
static bool space_create(intel_gt_t *gt)
{
    if (STATUS_IS_ERROR(pmm_alloc_pages(5, &gt->tables_phys)))
        return false;
    if (STATUS_IS_ERROR(pmm_alloc_pages(1, &gt->directory_phys))) {
        pmm_free_pages(gt->tables_phys, 5);
        return false;
    }
    uint64_t *tables = table(gt->tables_phys);
    memset(tables, 0, 5 * PAGE_SIZE);
    for (uint32_t level = 0; level < 4; level++) {
        for (uint32_t i = 0; i < 512; i++)
            tables[level * 512 + i] = scratch_entry(gt, level);
    }
    for (uint32_t i = 0; i < 512; i++)
        table(gt->directory_phys)[i] = scratch_entry(gt, 1);
    tables[0] = gt->directory_phys | PTE_PRESENT | PTE_WRITABLE;
    mutex_init(&gt->space_lock);
    return true;
}

static bool slot_taken(const intel_gt_t *gt, uint32_t slot)
{
    return gt->slots[slot / 8] & (1u << (slot % 8));
}

/* The entry for a slot in the table of its GiB (made if it is not there yet); NULL without memory. */
static uint64_t *slot_entry(intel_gt_t *gt, uint32_t slot)
{
    uint32_t gib = slot / 512;

    if (!gt->directories[gib]) {
        uint64_t phys;
        if (STATUS_IS_ERROR(pmm_alloc_pages(1, &phys)))
            return NULL;
        for (uint32_t i = 0; i < 512; i++)
            table(phys)[i] = scratch_entry(gt, 2);
        gt->directories[gib] = phys;
        table(gt->directory_phys)[SPACE_BASE / (1ull << 30) + gib] = phys | PTE_PRESENT | PTE_WRITABLE;
    }
    return &table(gt->directories[gib])[slot % 512];
}

static void slots_release(intel_gt_t *gt, uint32_t first, uint32_t count)
{
    for (uint32_t slot = first; slot < first + count; slot++) {
        uint64_t *entry = slot_entry(gt, slot);
        if (entry && slot_taken(gt, slot) && (*entry & PTE_ADDRESS) != (scratch_entry(gt, 2) & PTE_ADDRESS)) {
            uint64_t phys = *entry & PTE_ADDRESS;
            *entry = scratch_entry(gt, 2);
            pmm_free_pages(phys, 1);
        }
        gt->slots[slot / 8] &= (uint8_t)~(1u << (slot % 8));
    }
}

uint64_t intel_gt_map(intel_gt_t *gt, uint64_t phys, uint32_t pages, bool scanout)
{
    uint32_t count = (pages + SLOT_PAGES - 1) / SLOT_PAGES, first = 0, run = 0;
    uint64_t type = scanout ? PTE_UNCACHED : PTE_CACHED;
    bool ok = true;

    if (!gt || !pages || (phys & (PAGE_SIZE - 1)))
        return 0;
    mutex_lock(&gt->space_lock);
    for (uint32_t slot = 0; slot < SPACE_SLOTS && run < count; slot++) {
        if (slot_taken(gt, slot)) {
            run = 0;
        } else if (run++ == 0) {
            first = slot;
        }
    }
    if (run < count) {
        mutex_unlock(&gt->space_lock);
        return 0;
    }
    for (uint32_t s = 0; s < count && ok; s++) {
        uint64_t *entry = slot_entry(gt, first + s), leaf;
        if (!entry || STATUS_IS_ERROR(pmm_alloc_pages(1, &leaf))) {
            ok = false;
            break;
        }
        for (uint32_t i = 0; i < SLOT_PAGES; i++) {
            uint32_t page = s * SLOT_PAGES + i;
            table(leaf)[i] = page < pages ? (phys + (uint64_t)page * PAGE_SIZE) | PTE_PRESENT | PTE_WRITABLE | type
                                          : scratch_entry(gt, 3);
        }
        gt->slots[(first + s) / 8] |= (uint8_t)(1u << ((first + s) % 8));
        *entry = leaf | PTE_PRESENT | PTE_WRITABLE;
    }
    if (!ok)
        slots_release(gt, first, count);
    __asm__ volatile("mfence" : : : "memory");
    mutex_unlock(&gt->space_lock);
    return ok ? SPACE_BASE + (uint64_t)first * SLOT_BYTES : 0;
}

void intel_gt_unmap(intel_gt_t *gt, uint64_t address, uint32_t pages)
{
    if (!gt || address < SPACE_BASE || !pages)
        return;
    mutex_lock(&gt->space_lock);
    slots_release(gt, (uint32_t)((address - SPACE_BASE) / SLOT_BYTES), (pages + SLOT_PAGES - 1) / SLOT_PAGES);
    __asm__ volatile("mfence" : : : "memory");
    mutex_unlock(&gt->space_lock);
}

/* --- Engines ----------------------------------------------------------------------------- */

static void engine_report(const intel_gt_t *gt, const engine_t *e, const char *what)
{
    uint32_t b = e->base;

    klog_warn("igpu: %s engine: %s", e->name, what);
    klog_warn("igpu:   ring head 0x%x, tail 0x%x, start 0x%x, control 0x%x; executing at 0x%x, last command 0x%x",
              rd(gt, b + RING_HEAD), rd(gt, b + RING_TAIL), rd(gt, b + RING_START), rd(gt, b + RING_CTL),
              rd(gt, b + RING_ACTHD), rd(gt, b + RING_IPEHR));
    klog_warn("igpu:   mode 0x%x, MI mode 0x%x, errors 0x%x 0x%x 0x%x, done 0x%x, reset control 0x%x",
              rd(gt, b + RING_MODE), rd(gt, b + RING_MI_MODE), rd(gt, b + RING_EIR), rd(gt, b + RING_ESR),
              rd(gt, b + RING_IPEIR), rd(gt, b + RING_INSTDONE), rd(gt, b + RING_RESET_CTL));
    klog_warn("igpu:   submit status 0x%x 0x%x, events up to %u (read %u): 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x",
              rd(gt, b + RING_EXECLIST_STATUS), rd(gt, b + RING_EXECLIST_STATUS + 4), rd(gt, b + RING_CSB_POINTERS) & 7,
              e->csb_head, rd(gt, b + RING_CSB(0)), rd(gt, b + RING_CSB(1)), rd(gt, b + RING_CSB(2)),
              rd(gt, b + RING_CSB(3)), rd(gt, b + RING_CSB(4)), rd(gt, b + RING_CSB(5)));
    klog_warn("igpu:   status page: sequence number %u (wanted %u), its address 0x%x; fault 0x%x, error 0x%x; awake 0x%x "
              "0x%x; interrupts %lu (cause 0x%x)",
              e->hwsp ? e->hwsp[HWSP_SEQNO] : 0, e->seqno, rd(gt, b + RING_HWS_PGA), rd(gt, GT_FAULT), rd(gt, GT_ERROR),
              rd(gt, FORCEWAKE_RENDER_ACK), rd(gt, FORCEWAKE_GT_ACK), gt->interrupt_count, rd(gt, GT_INTERRUPT_CAUSE));
}

/* The engine as after power-on (i915's gen8_reset_engines()): it agrees to be reset, then its reset bit. */
static bool engine_reset(const intel_gt_t *gt, const engine_t *e)
{
    uint32_t b = e->base;
    bool ok = true;

    wr(gt, b + RING_MI_MODE, MASKED_ON(MI_MODE_STOP_RING));
    wait_bits(gt, b + RING_MI_MODE, MI_MODE_IDLE, MI_MODE_IDLE, 20);
    if (!(rd(gt, b + RING_RESET_CTL) & RESET_READY)) {
        wr(gt, b + RING_RESET_CTL, MASKED_ON(RESET_REQUEST));
        ok = wait_bits(gt, b + RING_RESET_CTL, RESET_READY, RESET_READY, 700);
    }
    if (ok) {
        wr(gt, GDRST, e->reset_bit);
        ok = wait_bits(gt, GDRST, e->reset_bit, 0, 500);
    }
    wr(gt, b + RING_RESET_CTL, MASKED_OFF(RESET_REQUEST));
    return ok;
}

/* The first page of registers of a context image, from its layout: the commands and which registers they load. */
static void image_layout(uint32_t *registers, const uint16_t *layout, uint32_t base)
{
    uint32_t at = 0;

    while (*layout) {
        uint32_t item = *layout++, count = item & 0xFF;
        if (item & 0x8000) {
            at += count;
            continue;
        }
        registers[at++] = MI_LOAD_REGISTERS(count) | ((item & 0x2000) ? MI_LRI_FORCE_POSTED : 0);
        for (uint32_t i = 0; i < count; i++, at += 2)
            registers[at] = base + *layout++;
    }
    registers[at] = MI_BATCH_BUFFER_END;
}

/* Memory, registers and the context of one engine; afterwards it waits for its first submission. */
static bool engine_setup(intel_gt_t *gt, engine_t *e)
{
    uint32_t b = e->base, pages = 1 + e->context_pages + RING_PAGES;
    uint64_t phys = 0;

    if (!engine_reset(gt, e)) {
        engine_report(gt, e, "it cannot be reset: not used");
        return false;
    }
    uint32_t gtt = gt->host.alloc(gt->host.context, pages, &phys);
    if (!gtt) {
        klog_warn("igpu: %s engine: no memory in the graphics address space: not used", e->name);
        return false;
    }
    uint8_t *memory = phys_to_virt(phys);
    e->hwsp_gtt = gtt;
    e->hwsp = (volatile uint32_t *)memory;
    e->context_gtt = gtt + PAGE_SIZE;
    e->registers = (uint32_t *)(memory + 2 * PAGE_SIZE); /* (the image's first page is a status page of its own) */
    e->ring_gtt = gtt + (1 + e->context_pages) * PAGE_SIZE;
    e->ring = (uint32_t *)(memory + (1 + e->context_pages) * PAGE_SIZE);
    e->tail = 0;

    /* Its batch buffer: in the address space commands are fetched from once the ring says "go there". */
    if (!STATUS_IS_ERROR(pmm_alloc_pages(BATCH_PAGES, &e->batch_phys))) {
        e->batch = phys_to_virt(e->batch_phys);
        memset(e->batch, 0, BATCH_PAGES * PAGE_SIZE);
        e->batch_address = intel_gt_map(gt, e->batch_phys, BATCH_PAGES, false);
    }
    if (!e->batch_address)
        klog_warn("igpu: %s engine: no batch buffer: it can only run commands from its ring", e->name);

    /* The engine (i915's enable_execlists()): contexts through the submit port, the status page, its interrupts. */
    wr(gt, b + RING_HWSTAM, 0xFFFFFFFF);
    /* (The engine's own mask register has its bits where the common registers have them: in its half.) */
    wr(gt, b + RING_IMR, gt->interrupts ? ~((ENGINE_USER_INTERRUPT | ENGINE_CONTEXT_SWITCH) << e->interrupt_shift)
                                        : 0xFFFFFFFF);
    wr(gt, b + RING_MODE, MASKED_ON(MODE_RUN_LIST));
    wr(gt, b + RING_MI_MODE, MASKED_OFF(MI_MODE_STOP_RING));
    wr(gt, b + RING_HWS_PGA, e->hwsp_gtt);
    (void)rd(gt, b + RING_HWS_PGA);
    wr(gt, b + RING_EMR, 0xFFFFFFFF);
    wr(gt, b + RING_EIR, 0xFFFFFFFF);
    /* The status events start again at entry 0: both pointers on the last entry. */
    e->csb_head = CSB_ENTRIES - 1;
    wr(gt, b + RING_CSB_POINTERS, 0xFFFFu << 16 | (CSB_ENTRIES - 1) << 8 | (CSB_ENTRIES - 1));
    (void)rd(gt, b + RING_CSB_POINTERS);

    /* The context (i915's __lrc_init_regs()): its ring, its address space, and "do not restore" for the first load. */
    uint32_t *r = e->registers;
    image_layout(r, e->layout, b);
    r[CTX_CONTEXT_CONTROL] = MASKED_ON(CTX_NO_SYNC_SWITCH) | MASKED_OFF(CTX_SAVE_INHIBIT | CTX_RS_ENABLE) |
                             MASKED_ON(CTX_RESTORE_INHIBIT);
    r[CTX_RING_HEAD] = 0;
    r[CTX_RING_TAIL] = 0;
    r[CTX_RING_START] = e->ring_gtt;
    r[CTX_RING_CTL] = (RING_BYTES - PAGE_SIZE) | RING_CTL_VALID;
    r[CTX_PDP0_UPPER] = (uint32_t)(gt->tables_phys >> 32);
    r[CTX_PDP0_LOWER] = (uint32_t)gt->tables_phys;
    r[CTX_MI_MODE] = MASKED_OFF(MI_MODE_STOP_RING);
    __asm__ volatile("mfence" : : : "memory");
    e->works = true;
    return true;
}

/* The status events since the last look. True if one of them says that the context was switched out. */
static bool engine_events(const intel_gt_t *gt, engine_t *e)
{
    uint32_t b = e->base, written = rd(gt, b + RING_CSB_POINTERS) & 7;
    bool out = false;

    if (written >= CSB_ENTRIES)
        return false;
    while (e->csb_head != written) {
        e->csb_head = (e->csb_head + 1) % CSB_ENTRIES;
        uint32_t status = rd(gt, b + RING_CSB(e->csb_head));
        if (e->event_count < 4)
            e->events[e->event_count++] = status;
        if (status & CSB_ACTIVE_TO_IDLE)
            out = true;
    }
    wr(gt, b + RING_CSB_POINTERS, (7u << 8) << 16 | e->csb_head << 8);
    return out;
}

/*
 * Commands into the ring (`count` dwords of them, then "go on in the batch buffer" if `batch`), the context to
 * the engine, and wait until it is through and switched out. Lock held.
 */
static status_t engine_submit(intel_gt_t *gt, engine_t *e, const uint32_t *commands, uint32_t count, bool batch,
                              uint64_t timeout_ns)
{
    uint32_t b = e->base, used = count + (batch ? BATCH_START_DWORDS : 0) + BREADCRUMB_DWORDS;
    uint32_t dwords = (used + 1) & ~1u, bytes = dwords * 4;
    uint32_t tail = e->tail, seqno = e->seqno + 1;

    if (tail + bytes > RING_BYTES) {
        /* Not enough room up to the end: the rest is filled with commands that do nothing, and on at the start. */
        for (uint32_t i = tail / 4; i < RING_BYTES / 4; i++)
            e->ring[i] = MI_NOOP;
        tail = 0;
    }
    uint32_t *out = e->ring + tail / 4;
    for (uint32_t i = 0; i < count; i++)
        *out++ = commands[i];
    if (batch) {
        *out++ = MI_BATCH_START;
        *out++ = (uint32_t)e->batch_address;
        *out++ = (uint32_t)(e->batch_address >> 32);
        *out++ = MI_NOOP;
    }
    /* The end of every submission: our sequence number into the status page (for what draws: once it is in memory). */
    *out++ = e->flushes ? MI_FLUSH_STORE : MI_STORE_DWORD_GGTT;
    *out++ = (e->hwsp_gtt + HWSP_SEQNO * 4) | (e->flushes ? MI_FLUSH_ADDRESS_GGTT : 0);
    *out++ = 0;
    *out++ = seqno;
    *out++ = MI_USER_INTERRUPT;
    *out++ = MI_NOOP;
    if (used & 1)
        *out++ = MI_NOOP; /* the tail is on a boundary of eight bytes */
    tail = (tail + bytes) % RING_BYTES;
    e->tail = tail;
    e->seqno = seqno;
    e->event_count = 0;

    /* The image is ours while the context is switched out: the new tail, and from the second load on its state. */
    e->registers[CTX_RING_TAIL] = tail;
    if (e->submissions++)
        e->registers[CTX_CONTEXT_CONTROL] = MASKED_ON(CTX_NO_SYNC_SWITCH) | MASKED_OFF(CTX_RESTORE_INHIBIT);
    __asm__ volatile("mfence" : : : "memory");

    /* The submit port takes two contexts, the second first; each as upper half, lower half. We have one. */
    wr(gt, b + RING_ELSP, 0);
    wr(gt, b + RING_ELSP, 0);
    wr(gt, b + RING_ELSP, (uint32_t)(e - gt->engines) + 1); /* a number for the context, which comes back in the events */
    wr(gt, b + RING_ELSP, e->context_gtt | DESCRIPTOR_VALID | DESCRIPTOR_48_BIT | DESCRIPTOR_PRIVILEGED);

    /* Switching out follows the last command within microseconds; a tenth of a second after it is "never". */
    uint64_t start = clock_monotonic_ns(), deadline = start + timeout_ns;
    bool done = false, out_again = false;
    for (;;) {
        uint64_t flags = arch_interrupts_save();
        if (!done)
            done = e->hwsp[HWSP_SEQNO] == seqno;
        if (done && !out_again)
            out_again = engine_events(gt, e);
        uint64_t now = clock_monotonic_ns();
        bool over = (done && out_again) || now > deadline + (done ? 100000000ull : 0);
        if (!over && gt->interrupts)
            wait_queue_block_uninterruptible(&gt->waiters, now + 2000000); /* an interrupt, or a look after 2 ms */
        arch_interrupts_restore(flags);
        if (over)
            break;
        if (!gt->interrupts) {
            if (now - start > 2000000)
                thread_sleep(1000000);
            else
                __asm__ volatile("pause");
        }
    }
    if (!done || !out_again) {
        e->works = false;
        engine_report(gt, e, done ? "its context is not switched out after the commands: not used any more"
                                  : "it does not execute the commands in time: not used any more");
        return STATUS_TIMEOUT;
    }
    return STATUS_SUCCESS;
}

status_t intel_gt_run(intel_gt_t *gt, uint32_t engine, const uint32_t *commands, uint32_t count, uint64_t timeout_ns)
{
    if (!gt || engine >= INTEL_ENGINE_COUNT)
        return STATUS_INVALID_ARGUMENT;
    engine_t *e = &gt->engines[engine];
    if ((count + BATCH_START_DWORDS + BREADCRUMB_DWORDS + 1) * 4 > RING_BYTES / 2 || (count && !commands))
        return STATUS_INVALID_ARGUMENT;
    mutex_lock(&e->lock);
    status_t status = e->works ? engine_submit(gt, e, commands, count, false, timeout_ns) : STATUS_DEVICE_ERROR;
    mutex_unlock(&e->lock);
    return status;
}

bool intel_gt_engine_works(const intel_gt_t *gt, uint32_t engine)
{
    return gt && engine < INTEL_ENGINE_COUNT && gt->engines[engine].works;
}

/* --- The blitter's rectangles ---------------------------------------------------------------- */

status_t intel_gt_blit(intel_gt_t *gt, const intel_blit_t *blits, uint32_t count)
{
    if (!gt || !count || count > INTEL_BLIT_MAX)
        return STATUS_INVALID_ARGUMENT;
    engine_t *e = &gt->engines[INTEL_ENGINE_BLITTER];
    if (!e->works || !e->batch_address)
        return STATUS_NOT_SUPPORTED;
    for (uint32_t i = 0; i < count; i++) {
        const intel_blit_t *r = &blits[i];
        if (!r->width || !r->height || r->x + r->width > BLT_MAX || r->y + r->height > BLT_MAX || r->to_pitch > BLT_MAX ||
            (r->copy && (r->from_x + r->width > BLT_MAX || r->from_y + r->height > BLT_MAX || r->from_pitch > BLT_MAX)))
            return STATUS_INVALID_ARGUMENT;
    }

    mutex_lock(&e->lock);
    uint32_t *out = e->batch;
    for (uint32_t i = 0; i < count; i++) {
        const intel_blit_t *r = &blits[i];
        *out++ = r->copy ? XY_SRC_COPY_BLT : XY_COLOR_BLT;
        *out++ = BLT_DEPTH_32 | (r->copy ? BLT_ROP_COPY : BLT_ROP_COLOR) | r->to_pitch;
        *out++ = r->y << 16 | r->x;
        *out++ = (r->y + r->height) << 16 | (r->x + r->width);
        *out++ = (uint32_t)r->to;
        *out++ = (uint32_t)(r->to >> 32);
        if (r->copy) {
            *out++ = r->from_y << 16 | r->from_x;
            *out++ = r->from_pitch;
            *out++ = (uint32_t)r->from;
            *out++ = (uint32_t)(r->from >> 32);
        } else {
            *out++ = r->color;
        }
    }
    *out++ = MI_BATCH_BUFFER_END;
    *out++ = MI_NOOP;
    __asm__ volatile("mfence" : : : "memory");
    status_t status = e->works ? engine_submit(gt, e, NULL, 0, true, 1000000000ull) : STATUS_DEVICE_ERROR;
    mutex_unlock(&e->lock);
    return status;
}

/* --- Start ------------------------------------------------------------------------------- */

/* An engine executes what it is given: nothing, again nothing (the second load of the context), and a store. */
static bool engine_try(intel_gt_t *gt, uint32_t index)
{
    engine_t *e = &gt->engines[index];
    const uint32_t marker = 0x4A454C59; /* "JELY" */
    uint32_t store[4] = { MI_STORE_DWORD_GGTT, e->hwsp_gtt + HWSP_SCRATCH * 4, 0, marker };
    const uint64_t second = 1000000000ull;

    uint64_t start = clock_monotonic_ns();
    if (STATUS_IS_ERROR(intel_gt_run(gt, index, NULL, 0, second)))
        return false;
    uint64_t first_ms = (clock_monotonic_ns() - start) / 1000000;
    uint32_t events[2] = { e->events[0], e->event_count > 1 ? e->events[1] : 0 };
    if (STATUS_IS_ERROR(intel_gt_run(gt, index, NULL, 0, second)) ||
        STATUS_IS_ERROR(intel_gt_run(gt, index, store, 4, second)))
        return false;
    if (e->hwsp[HWSP_SCRATCH] != marker) {
        e->works = false;
        engine_report(gt, e, "a command that stores a value stored nothing: not used");
        return false;
    }
    /* Once around the ring, to see that it goes on at its start. */
    for (uint32_t i = 0; i < RING_BYTES / (BREADCRUMB_DWORDS * 4) + 8; i++) {
        if (STATUS_IS_ERROR(intel_gt_run(gt, index, NULL, 0, second)))
            return false;
    }
    /* A batch with nothing in it: there and back. */
    if (e->batch_address) {
        mutex_lock(&e->lock);
        e->batch[0] = MI_BATCH_BUFFER_END;
        e->batch[1] = MI_NOOP;
        status_t status = engine_submit(gt, e, NULL, 0, true, second);
        mutex_unlock(&e->lock);
        if (STATUS_IS_ERROR(status))
            return false;
    }
    klog_info("igpu: %s engine: executes commands (%u submissions in %lu ms; the first took %lu ms, status events 0x%x "
              "0x%x)", e->name, e->submissions, (clock_monotonic_ns() - start) / 1000000, first_ms, events[0], events[1]);
    return true;
}

status_t intel_gt_start(const intel_gt_host_t *host, intel_gt_t **out)
{
    intel_gt_t *gt = kcalloc(1, sizeof(*gt));

    *out = NULL;
    if (!gt)
        return STATUS_OUT_OF_MEMORY;
    gt->host = *host;
    gt->engines[INTEL_ENGINE_RENDER] = (engine_t){ .name = "render", .base = 0x02000, .reset_bit = GDRST_RENDER,
                                                   .context_pages = 22, .layout = render_layout, .interrupt_shift = 0 };
    gt->engines[INTEL_ENGINE_BLITTER] = (engine_t){ .name = "blitter", .base = 0x22000, .reset_bit = GDRST_BLITTER,
                                                    .context_pages = 2, .layout = blitter_layout, .interrupt_shift = 16,
                                                    .flushes = true };
    for (uint32_t i = 0; i < INTEL_ENGINE_COUNT; i++)
        mutex_init(&gt->engines[i].lock);
    wait_queue_init(&gt->waiters);

    klog_info("igpu: GPU: sleep control 0x%x, state 0x%x; awake before: render 0x%x, the rest 0x%x", rd(gt, RC_CONTROL),
              rd(gt, RC_STATE), rd(gt, FORCEWAKE_RENDER_ACK), rd(gt, FORCEWAKE_GT_ACK));
    bool awake = forcewake(gt, FORCEWAKE_GT, FORCEWAKE_GT_ACK, "common");
    awake = forcewake(gt, FORCEWAKE_RENDER, FORCEWAKE_RENDER_ACK, "render") && awake;
    if (!awake || !space_create(gt)) {
        if (awake)
            klog_warn("igpu: no memory for the GPU's translation tables");
        kfree(gt);
        return awake ? STATUS_OUT_OF_MEMORY : STATUS_DEVICE_ERROR;
    }

    /* The clock: the highest rate the GT has, in place of the slow one the firmware left. */
    uint32_t capability = rd(gt, RP_CAPABILITY), highest = capability & 0xFF, lowest = (capability >> 16) & 0xFF;
    uint32_t before = intel_gt_clock_mhz(gt);
    if (highest) {
        wr(gt, RP_REQUEST, (highest * 3) << 23);
        thread_sleep(20000000);
    }
    klog_info("igpu: GPU: its clock can run at %u to %u MHz; it ran at %u MHz, now at %u MHz", lowest * 50, highest * 50,
              before, intel_gt_clock_mhz(gt));

    /*
     * The memory types of the GPU's translation tables, as i915 sets them (its bdw_setup_private_ppat()): type 0,
     * which every entry of the global table has, is "cached, in the cache the CPU shares", so that both see
     * what the other wrote.
     */
    klog_debug("igpu: GPU: memory types were 0x%x 0x%x", rd(gt, PRIVATE_PAT_LOW), rd(gt, PRIVATE_PAT_HIGH));
    wr(gt, PRIVATE_PAT_LOW, 0x000A0907);
    wr(gt, PRIVATE_PAT_HIGH, 0x3B2B1B0B);

    /* The engines' interrupts: "a user interrupt command was executed" and "a status event was written". */
    uint32_t wanted = 0;
    for (uint32_t i = 0; i < INTEL_ENGINE_COUNT; i++)
        wanted |= (ENGINE_USER_INTERRUPT | ENGINE_CONTEXT_SWITCH) << gt->engines[i].interrupt_shift;
    gt->interrupts = host->interrupts;
    wr(gt, GT_INTERRUPT_MASK, gt->interrupts ? ~wanted : 0xFFFFFFFF);
    wr(gt, GT_INTERRUPT_ENABLE, gt->interrupts ? wanted : 0);
    wr(gt, GT_INTERRUPT_CAUSE, 0xFFFFFFFF);
    *out = gt; /* the host's handler may call us from here on */

    uint32_t working = 0;
    for (uint32_t i = 0; i < INTEL_ENGINE_COUNT; i++) {
        if (engine_setup(gt, &gt->engines[i]) && engine_try(gt, i))
            working++;
    }
    if (gt->interrupts && !gt->interrupt_count) {
        /* Hundreds of submissions and not one interrupt: each wait ran into its two milliseconds. */
        klog_warn("igpu: the engines' interrupts do not arrive (cause 0x%x, mask 0x%x): they are polled instead",
                  rd(gt, GT_INTERRUPT_CAUSE), rd(gt, GT_INTERRUPT_MASK));
        gt->interrupts = false;
        wr(gt, GT_INTERRUPT_ENABLE, 0);
        wr(gt, GT_INTERRUPT_MASK, 0xFFFFFFFF);
    } else if (gt->interrupts) {
        klog_info("igpu: GPU: the engines report by interrupt (%lu so far)", gt->interrupt_count);
    }
    if (!working) {
        klog_warn("igpu: no engine of the GPU executes commands: the GPU is not used (the display is not affected)");
        wr(gt, GT_INTERRUPT_ENABLE, 0);
        wr(gt, GT_INTERRUPT_MASK, 0xFFFFFFFF);
        *out = NULL; /* (what was allocated stays: an engine may still hold it) */
        return STATUS_DEVICE_ERROR;
    }
    return STATUS_SUCCESS;
}
