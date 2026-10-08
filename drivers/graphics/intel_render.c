/*
 * Intel graphics, generation 9: copying and blending rectangles on the
 * render engine, for putting a desktop together on the GPU (the way the
 * previous JellyOS did it, Kernel/drivers/gpu/igd_rcs.c and igd_comp.c).
 *
 * Not the 3D pipeline but the GPGPU one: the render engine starts a
 * hardware thread on the execution units for each block of 8 x 8 pixels
 * (or smaller at the edges), and each runs a small program
 * (intel_kernels.c) that reads its block of the source and of the
 * destination, blends, and writes the block back. A batch for a list of
 * rectangles:
 *
 *   PIPELINE_SELECT          the GPGPU pipeline
 *   STATE_BASE_ADDRESS       where states, constants and programs are
 *   MEDIA_VFE_STATE          how many threads may run at once
 *   per piece of a rectangle
 *     MEDIA_CURBE_LOAD       its constants: the corners in both surfaces
 *     MEDIA_INTERFACE_...    which program, which surfaces
 *     GPGPU_WALKER           one thread per block
 *     MEDIA_STATE_FLUSH
 *     PIPE_CONTROL           wait for every thread and flush the data
 *                            cache: the next piece sees what this one wrote
 *
 * Everything the engine reads for this (programs, states, batch) is one
 * piece of memory in the engines' address space. The surfaces are seen as
 * 8-bit surfaces (x in bytes); one that a display reads is written past
 * the GPU's caches (its memory object control entry), others go through
 * them.
 *
 * QEMU has no such device: this can only be tested on real hardware.
 */

#include "drivers/graphics/intel_render.h"
#include "drivers/graphics/intel_kernels.h"

#include "core/log.h"
#include "core/string.h"
#include "memory/heap.h"
#include "memory/layout.h"
#include "memory/pmm.h"
#include "scheduler/mutex.h"

/* Commands of the render engine's pipelines */
#define PIPELINE_SELECT_GPGPU 0x69040302u /* (bits 9:8: the mask for the selection, which is 2) */
#define STATE_BASE_ADDRESS    (0x61010000u | 17) /* 19 dwords */
#define MEDIA_VFE_STATE       (0x70000000u | 7)
#define MEDIA_CURBE_LOAD      (0x70010000u | 2)
#define MEDIA_IDD_LOAD        (0x70020000u | 2)
#define MEDIA_STATE_FLUSH     (0x70040000u | 0)
#define GPGPU_WALKER          (0x71050000u | 13)
#define PIPE_CONTROL          (3u << 29 | 3u << 27 | 2u << 24 | 4) /* 6 dwords */
#define PC_CS_STALL           (1u << 20)
#define PC_DC_FLUSH           (1u << 5)
#define PC_RT_FLUSH           (1u << 12)
#define MI_BATCH_BUFFER_END   (0x0Au << 23)
#define MI_NOOP               0u

#define SURFACE_2D            (1u << 29)
#define SURFACE_R8_UNORM      (0x140u << 18)
#define SURFACE_ALIGN         (1u << 16 | 1u << 14) /* vertical, horizontal alignment 4 */
#define SURFACE_CHANNELS      (4u << 25 | 5u << 22 | 6u << 19 | 7u << 16) /* red, green, blue, alpha */
#define SURFACE_MAX_BYTES     16384

#define THREADS               168 /* GT2: 24 execution units with 7 threads each */

/* The memory: programs, then a state block per piece, then the batch */
#define KERNEL_PAGES          4
#define STATE_PAGES           8
#define BATCH_PAGES           6
#define TOTAL_PAGES           (KERNEL_PAGES + STATE_PAGES + BATCH_PAGES)
#define PIECE_STATE           256 /* interface descriptor, constants, binding table, two surface states */
#define MAX_PIECES            (INTEL_COMPOSE_MAX * 4)

struct intel_render {
    intel_gt_t *gt;
    mutex_t     lock;
    uint8_t    *memory;
    uint64_t    phys, address;
    uint32_t    programs[2][INTEL_SHAPES]; /* where each program starts: [blend][shape] */
};

static void flush_lines(const void *start, size_t bytes)
{
    for (size_t offset = 0; offset < bytes; offset += 64)
        __asm__ volatile("clflush (%0)" : : "r"((const uint8_t *)start + offset) : "memory");
    __asm__ volatile("mfence" : : : "memory");
}

/* A surface state: an 8-bit 2D surface of the whole surface's bytes, its caching. */
static void surface_state(uint32_t *s, const intel_surface_t *surface)
{
    memset(s, 0, 64);
    s[0] = SURFACE_2D | SURFACE_R8_UNORM | SURFACE_ALIGN;
    s[1] = (surface->scanout ? INTEL_MOCS_UNCACHED : INTEL_MOCS_CACHED) << 24;
    s[2] = (surface->width * 4 - 1) | (surface->height - 1) << 16;
    s[3] = surface->pitch - 1;
    s[7] = SURFACE_CHANNELS;
    s[8] = (uint32_t)surface->address;
    s[9] = (uint32_t)(surface->address >> 32);
}

static bool surface_ok(const intel_surface_t *s)
{
    return s && s->width && s->height && s->width * 4 <= SURFACE_MAX_BYTES && s->height <= 16384 &&
           s->pitch >= s->width * 4 && !(s->pitch & 63) && !(s->address & (PAGE_SIZE - 1));
}

static bool inside(uint32_t x, uint32_t y, uint32_t width, uint32_t height, const intel_surface_t *s)
{
    return width && height && x < s->width && y < s->height && width <= s->width - x && height <= s->height - y;
}

static bool operation_ok(const intel_compose_t *o)
{
    if (!surface_ok(o->to) || !surface_ok(o->from) || o->opacity > 256 ||
        !inside(o->to_x, o->to_y, o->width, o->height, o->to) ||
        !inside(o->from_x, o->from_y, o->width, o->height, o->from))
        return false;
    /* Within one surface the blocks run at once: the rectangle must not lie on itself. */
    if (o->to->address == o->from->address && o->to_x < o->from_x + o->width && o->from_x < o->to_x + o->width &&
        o->to_y < o->from_y + o->height && o->from_y < o->to_y + o->height)
        return false;
    return true;
}

status_t intel_render_compose(intel_render_t *r, const intel_compose_t *operations, uint32_t count)
{
    if (!r || !count || count > INTEL_COMPOSE_MAX)
        return STATUS_INVALID_ARGUMENT;
    for (uint32_t i = 0; i < count; i++) {
        if (!operation_ok(&operations[i]))
            return STATUS_INVALID_ARGUMENT;
    }

    mutex_lock(&r->lock);
    uint8_t *states = r->memory + KERNEL_PAGES * PAGE_SIZE;
    uint32_t *b = (uint32_t *)(r->memory + (KERNEL_PAGES + STATE_PAGES) * PAGE_SIZE), k = 0, pieces = 0;
    uint64_t kernels = r->address, state_base = r->address + KERNEL_PAGES * PAGE_SIZE;
    uint64_t batch = r->address + (KERNEL_PAGES + STATE_PAGES) * PAGE_SIZE;

    b[k++] = PIPELINE_SELECT_GPGPU;
    /* Surface and dynamic state in the state blocks, programs from the start of the memory. */
    b[k++] = STATE_BASE_ADDRESS;
    b[k++] = 1;                                   /* general state: 0 */
    b[k++] = 0;
    b[k++] = 1;                                   /* stateless access (as IGT sets it) */
    b[k++] = (uint32_t)state_base | 1;            /* surface state */
    b[k++] = (uint32_t)(state_base >> 32);
    b[k++] = (uint32_t)state_base | 1;            /* dynamic state */
    b[k++] = (uint32_t)(state_base >> 32);
    b[k++] = 1;                                   /* indirect objects: 0 */
    b[k++] = 0;
    b[k++] = (uint32_t)kernels | 1;               /* instructions */
    b[k++] = (uint32_t)(kernels >> 32);
    b[k++] = 0xFFFFF000u | 1;                     /* the sizes */
    b[k++] = (STATE_PAGES << 12) | 1;
    b[k++] = 0xFFFFF000u | 1;
    b[k++] = (KERNEL_PAGES << 12) | 1;
    b[k++] = 1;                                   /* bindless surface state: 0 */
    b[k++] = 0;
    b[k++] = 0xFFFFF000u;
    b[k++] = MEDIA_VFE_STATE;
    b[k++] = 0;                                   /* no scratch space */
    b[k++] = 0;
    b[k++] = (THREADS - 1) << 16 | 1u << 8;       /* threads at once, one URB entry */
    b[k++] = 0;
    b[k++] = 1;                                   /* constants: one register */
    b[k++] = 0;
    b[k++] = 0;
    b[k++] = 0;

    for (uint32_t i = 0; i < count; i++) {
        const intel_compose_t *o = &operations[i];
        intel_piece_t parts[4];
        uint32_t n = intel_pieces(o->to_x, o->to_y, o->from_x, o->from_y, o->width, o->height, parts);
        for (uint32_t p = 0; p < n; p++, pieces++) {
            uint32_t at = pieces * PIECE_STATE;
            uint32_t *idd = (uint32_t *)(states + at), *constants = idd + 16, *table = idd + 24;
            bool last = i + 1 == count && p + 1 == n;
            memset(idd, 0, PIECE_STATE);
            idd[0] = r->programs[o->blend][parts[p].shape];
            idd[2] = 1u << 18;                    /* single program flow */
            idd[4] = at + 96;                     /* the binding table */
            idd[5] = 1u << 16;                    /* constants: one register */
            idd[6] = 1;                           /* one thread per group */
            constants[0] = parts[p].to_x;
            constants[1] = parts[p].to_y;
            constants[2] = parts[p].from_x;
            constants[3] = parts[p].from_y;
            constants[4] = o->blend ? o->opacity : 256;
            table[0] = at + 128;
            table[1] = at + 192;
            surface_state(idd + 32, o->to);
            surface_state(idd + 48, o->from);
            b[k++] = MEDIA_CURBE_LOAD;
            b[k++] = 0;
            b[k++] = 32;
            b[k++] = at + 64;
            b[k++] = MEDIA_IDD_LOAD;
            b[k++] = 0;
            b[k++] = 32;
            b[k++] = at;
            b[k++] = GPGPU_WALKER;
            b[k++] = 0;                           /* interface descriptor 0 */
            b[k++] = 0;
            b[k++] = 0;
            b[k++] = 1u << 30;                    /* SIMD16, one thread per group */
            b[k++] = 0;                           /* groups across from 0 */
            b[k++] = 0;
            b[k++] = parts[p].columns;
            b[k++] = 0;                           /* down from 0 */
            b[k++] = 0;
            b[k++] = parts[p].rows;
            b[k++] = 0;
            b[k++] = 1;
            b[k++] = 0xFFFF;                      /* all sixteen channels */
            b[k++] = 0xFFFFFFFFu;
            b[k++] = MEDIA_STATE_FLUSH;
            b[k++] = 0;
            b[k++] = PIPE_CONTROL;                /* every thread done, the data in memory: the next piece sees it */
            b[k++] = PC_CS_STALL | PC_DC_FLUSH | (last ? PC_RT_FLUSH : 0);
            b[k++] = 0;
            b[k++] = 0;
            b[k++] = 0;
            b[k++] = 0;
        }
    }
    b[k++] = MI_BATCH_BUFFER_END;
    b[k++] = MI_NOOP;
    flush_lines(states, (size_t)pieces * PIECE_STATE);
    flush_lines(b, (size_t)k * 4);
    status_t status = intel_gt_run_batch(r->gt, INTEL_ENGINE_RENDER, batch, 1000000000ull);
    mutex_unlock(&r->lock);
    return status;
}

/* --- Start ------------------------------------------------------------------------------- */

static uint32_t pattern(uint32_t i, uint32_t seed)
{
    uint32_t x = i * 2654435761u ^ seed * 40503u;
    x ^= x >> 15;
    x *= 2246822519u;
    return x ^ (x >> 13);
}

/*
 * The programs against the CPU, on surfaces of the engine's own (256 x 64 pixels): a copy of an odd rectangle
 * (all four shapes of pieces), then two blends that overlap, the second at half opacity, then a copy into a
 * surface written past the caches. Every pixel of the destinations is compared.
 */
static bool self_test(intel_render_t *r)
{
    enum { W = 256, H = 64, PAGES = W * H * 4 / PAGE_SIZE };
    uint64_t phys, address, scanout_address;
    static uint32_t want[W * H];
    bool ok = false;

    if (STATUS_IS_ERROR(pmm_alloc_pages(3 * PAGES, &phys)))
        return false;
    address = intel_gt_map(r->gt, phys, 2 * PAGES, false);
    scanout_address = intel_gt_map(r->gt, phys + 2 * PAGES * PAGE_SIZE, PAGES, true);
    if (!address || !scanout_address) {
        klog_warn("igpu: render engine: no room in the address space for its test");
        goto out;
    }
    uint32_t *a = phys_to_virt(phys), *b = a + W * H, *c = b + W * H;
    intel_surface_t sa = { address, W, H, W * 4, false }, sb = { address + PAGES * PAGE_SIZE, W, H, W * 4, false };
    intel_surface_t sc = { scanout_address, W, H, W * 4, true };

    for (uint32_t i = 0; i < W * H; i++) {
        a[i] = pattern(i, 7);
        b[i] = 0;
        c[i] = 0;
    }
    flush_lines(a, 3 * PAGES * PAGE_SIZE);
    const intel_compose_t copy = { false, 256, &sb, &sa, 37, 11, 5, 3, 101, 29 };
    status_t status = intel_render_compose(r, &copy, 1);
    if (STATUS_IS_ERROR(status)) {
        klog_warn("igpu: render engine: a copy does not get done (%s)", status_name(status));
        goto out;
    }
    flush_lines(b, PAGES * PAGE_SIZE);
    uint32_t wrong = 0, first = 0, is = 0, should = 0;
    for (uint32_t y = 0; y < H; y++) {
        for (uint32_t x = 0; x < W; x++) {
            bool in = x >= 37 && x < 37 + 101 && y >= 11 && y < 11 + 29;
            want[y * W + x] = in ? a[(y - 11 + 3) * W + (x - 37 + 5)] : 0;
            if (b[y * W + x] != want[y * W + x] && !wrong++) {
                first = y * W + x;
                is = b[first];
                should = want[first];
            }
        }
    }
    if (wrong) {
        klog_warn("igpu: render engine: copying goes wrong: %u pixels, the first (%u,%u) is 0x%x, not 0x%x", wrong,
                  first % W, first / W, is, should);
        goto out;
    }

    /* Blending onto what the copy left, and a second rectangle over the first; then into the scanout surface. */
    for (uint32_t i = 0; i < W * H; i++) {
        b[i] = want[i] ? want[i] : pattern(i, 99);
        want[i] = b[i];
    }
    flush_lines(b, PAGES * PAGE_SIZE);
    const intel_compose_t list[3] = {
        { true, 256, &sb, &sa, 9, 7, 0, 0, 205, 51 },
        { true, 128, &sb, &sa, 2, 40, 13, 1, 60, 20 },
        { false, 256, &sc, &sa, 19, 21, 3, 30, 77, 33 },
    };
    status = intel_render_compose(r, list, 3);
    if (STATUS_IS_ERROR(status)) {
        klog_warn("igpu: render engine: blending does not get done (%s)", status_name(status));
        goto out;
    }
    for (uint32_t i = 0; i < 2; i++) {
        const intel_compose_t *o = &list[i];
        for (uint32_t y = 0; y < o->height; y++) {
            for (uint32_t x = 0; x < o->width; x++) {
                uint32_t *d = &want[(o->to_y + y) * W + o->to_x + x];
                *d = intel_blend_pixel(a[(o->from_y + y) * W + o->from_x + x], *d, o->opacity);
            }
        }
    }
    flush_lines(a, 3 * PAGES * PAGE_SIZE);
    for (uint32_t i = 0; i < W * H; i++) {
        if (b[i] != want[i] && !wrong++) {
            first = i;
            is = b[i];
            should = want[i];
        }
    }
    for (uint32_t y = 0; y < H; y++) {
        for (uint32_t x = 0; x < W; x++) {
            bool in = x >= 19 && x < 19 + 77 && y >= 21 && y < 21 + 33;
            uint32_t expected = in ? a[(y - 21 + 30) * W + (x - 19 + 3)] : 0;
            if (c[y * W + x] != expected && !wrong++) {
                first = W * H + y * W + x;
                is = c[y * W + x];
                should = expected;
            }
        }
    }
    if (wrong) {
        klog_warn("igpu: render engine: copying and blending go wrong: %u pixels, the first (%s, %u,%u) is 0x%x, not 0x%x",
                  wrong, first < W * H ? "blended" : "copied past the caches", first % W, (first % (W * H)) / W, is, should);
        goto out;
    }
    klog_info("igpu: render engine: copies and blends correctly");
    ok = true;
out:
    /* (The memory and the address space stay: an engine that went wrong may still be at them.) */
    return ok;
}

status_t intel_render_start(intel_gt_t *gt, intel_render_t **out)
{
    static uint32_t code[INTEL_KERNEL_MAX][4];
    intel_render_t *r = kcalloc(1, sizeof(*r));

    *out = NULL;
    if (!r)
        return STATUS_OUT_OF_MEMORY;
    r->gt = gt;
    mutex_init(&r->lock);
    if (STATUS_IS_ERROR(pmm_alloc_pages(TOTAL_PAGES, &r->phys))) {
        kfree(r);
        return STATUS_OUT_OF_MEMORY;
    }
    r->memory = phys_to_virt(r->phys);
    memset(r->memory, 0, TOTAL_PAGES * PAGE_SIZE);
    r->address = intel_gt_map(gt, r->phys, TOTAL_PAGES, false);
    if (!r->address) {
        klog_warn("igpu: render engine: no room in the address space");
        return STATUS_OUT_OF_MEMORY; /* (r stays: nothing is to be had back from a failed start) */
    }

    /* The eight programs, each on a boundary of 64 bytes. */
    uint32_t at = 0, longest = 0;
    for (uint32_t blend = 0; blend < 2; blend++) {
        for (uint32_t shape = 0; shape < INTEL_SHAPES; shape++) {
            uint32_t n = intel_kernel(blend, shape, code);
            if (n > INTEL_KERNEL_MAX || at + n * 16 > KERNEL_PAGES * PAGE_SIZE) {
                klog_warn("igpu: render engine: the programs do not fit");
                return STATUS_LIMIT_EXCEEDED;
            }
            memcpy(r->memory + at, code, n * 16);
            r->programs[blend][shape] = at;
            at = (at + n * 16 + 63) & ~63u;
            longest = n > longest ? n : longest;
        }
    }
    flush_lines(r->memory, KERNEL_PAGES * PAGE_SIZE);
    klog_info("igpu: render engine: 8 programs of %u bytes (the longest %u instructions)", at, longest);

    if (!self_test(r))
        return STATUS_DEVICE_ERROR;
    *out = r;
    return STATUS_SUCCESS;
}
