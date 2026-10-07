/*
 * xHCI host controller driver (USB 3 controllers, which also serve USB 2
 * and USB 1 devices on their root ports).
 *
 * The controller works on rings of 16-byte TRBs: the driver queues commands
 * on the command ring and transfers on one ring per endpoint; results come
 * back on the event ring, announced by an interrupt (MSI-X or MSI).
 *
 * A thread per controller watches the root hub ports. For a new device it
 * resets the port, enables a slot, assigns the address and hands the device
 * to the USB core; when a device disappears it disables the slot and lets
 * the core unbind the class drivers.
 *
 * Commands and control transfers are serialized by one mutex and block
 * until their event arrives. Interrupt transfers are queued from any
 * context; their completion runs in the interrupt handler.
 *
 * Devices behind hubs get a slot like any other; the hub driver (hub.c)
 * asks for them, and the slot's route string tells the controller the way.
 *
 * Not yet: isochronous transfers, streams, power management.
 */

#include "drivers/bus/pci/pci.h"
#include "drivers/bus/usb/usb.h"
#include "drivers/core/module.h"

#include "core/log.h"
#include "core/string.h"
#include "memory/heap.h"
#include "memory/layout.h"
#include "scheduler/mutex.h"
#include "scheduler/thread.h"
#include "time/clock.h"

/* Capability registers */
#define CAP_CAPLENGTH  0x00
#define CAP_HCSPARAMS1 0x04
#define CAP_HCSPARAMS2 0x08
#define CAP_HCCPARAMS1 0x10
#define CAP_DBOFF      0x14
#define CAP_RTSOFF     0x18

#define HCC_AC64 (1u << 0) /* 64-bit addresses */
#define HCC_CSZ  (1u << 2) /* 64-byte contexts */

/* Operational registers */
#define OP_USBCMD 0x00
#define OP_USBSTS 0x04
#define OP_CRCR   0x18
#define OP_DCBAAP 0x30
#define OP_CONFIG 0x38
#define OP_PORTSC 0x400 /* + 0x10 * (port - 1) */

#define CMD_RUN   (1u << 0)
#define CMD_RESET (1u << 1)
#define CMD_INTE  (1u << 2)
#define STS_HALTED    (1u << 0)
#define STS_EINT      (1u << 3)
#define STS_NOT_READY (1u << 11)

#define PORT_CONNECTED   (1u << 0)
#define PORT_ENABLED     (1u << 1)  /* writing 1 disables the port */
#define PORT_RESET       (1u << 4)
#define PORT_POWER       (1u << 9)
#define PORT_SPEED(v)    (((v) >> 10) & 0xF)
#define PORT_CSC         (1u << 17) /* connect status change */
#define PORT_PRC         (1u << 21) /* reset change */
#define PORT_CHANGES     (0x7Fu << 17)
/* Bits a write must carry over; everything else is write-1-to-act. */
#define PORT_PRESERVE    (PORT_POWER | (3u << 14) | (7u << 25))

/* Interrupter 0 (runtime registers + 0x20) */
#define IR_IMAN   0x00
#define IR_IMOD   0x04
#define IR_ERSTSZ 0x08
#define IR_ERSTBA 0x10
#define IR_ERDP   0x18
#define IMAN_PENDING (1u << 0)
#define IMAN_ENABLE  (1u << 1)
#define ERDP_BUSY    (1u << 3)

/* TRBs */
typedef struct {
    uint64_t parameter;
    uint32_t status;
    uint32_t control;
} xhci_trb_t;

#define TRB_CYCLE        (1u << 0)
#define TRB_TOGGLE_CYCLE (1u << 1)  /* link TRBs */
#define TRB_ISP          (1u << 2)  /* interrupt on short packet */
#define TRB_IOC          (1u << 5)  /* interrupt on completion */
#define TRB_IDT          (1u << 6)  /* immediate data (setup stage) */
#define TRB_DIR_IN       (1u << 16)
#define TRB_TYPE(t)      ((uint32_t)(t) << 10)
#define TRB_TYPE_OF(c)   (((c) >> 10) & 0x3F)

#define TRB_NORMAL             1
#define TRB_SETUP              2
#define TRB_DATA               3
#define TRB_STATUS             4
#define TRB_LINK               6
#define TRB_ENABLE_SLOT        9
#define TRB_DISABLE_SLOT       10
#define TRB_ADDRESS_DEVICE     11
#define TRB_CONFIGURE_ENDPOINT 12
#define TRB_EVALUATE_CONTEXT   13
#define TRB_RESET_ENDPOINT     14
#define TRB_STOP_ENDPOINT      15
#define TRB_SET_TR_DEQUEUE     16
#define TRB_TRANSFER_EVENT     32
#define TRB_COMMAND_COMPLETION 33
#define TRB_PORT_STATUS_CHANGE 34

#define CODE_SUCCESS      1
#define CODE_STALL        6
#define CODE_SHORT_PACKET 13

#define RING_TRBS    256 /* one page; the last TRB links back to the first */
#define MAX_SLOTS    32
#define ENDPOINTS    32  /* device context indexes: 1 = endpoint 0, 2n = OUT n, 2n + 1 = IN n */
#define COMMAND_TIMEOUT_NS 2000000000ull
#define CONTROL_TIMEOUT_NS 3000000000ull

typedef struct {
    dma_buffer_t memory;
    xhci_trb_t  *trbs;
    uint32_t     enqueue;
    uint32_t     cycle;
} xhci_ring_t;

/* A thread waiting for an event */
typedef struct {
    wait_queue_t queue;
    bool         done;
    uint32_t     code;
    uint32_t     slot;
    uint32_t     residual;
} xhci_wait_t;

struct xhci;

typedef struct {
    usb_device_t    usb;
    struct xhci    *hc;
    uint8_t         id;
    bool            gone;
    /* The way to the device: root hub port, then one hub port per route digit */
    uint32_t        route, root_port;
    uint8_t         tt_slot, tt_port;   /* low/full speed behind a high speed hub: the hub translating for it */
    dma_buffer_t    output, input;      /* device context, input context */
    dma_buffer_t    control_data;       /* data stage of control transfers */
    xhci_ring_t     rings[ENDPOINTS];
    usb_transfer_t *pending[ENDPOINTS];
    xhci_wait_t    *control_wait;
} xhci_slot_t;

typedef struct xhci {
    device_t          *device;
    volatile uint8_t  *base;
    volatile uint8_t  *op;
    volatile uint8_t  *interrupter;
    volatile uint32_t *doorbells;
    uint32_t           max_slots, max_ports, context_size;
    uint64_t           dma_limit;

    dma_buffer_t       dcbaa, scratchpad_array, scratchpads;
    xhci_ring_t        command;
    xhci_wait_t       *command_wait;
    dma_buffer_t       events, erst;
    uint32_t           event_dequeue, event_cycle;
    uint32_t           irq;

    mutex_t            lock;            /* command ring and control transfers */
    xhci_slot_t       *slots[MAX_SLOTS + 1];
    xhci_slot_t      **port_slots;      /* by port number */
    wait_queue_t       port_wakeup;
    bool               ports_changed;
} xhci_t;

/* --- Register access ------------------------------------------------------------- */

static uint32_t read32(volatile uint8_t *base, uint32_t offset)
{
    return *(volatile uint32_t *)(base + offset);
}

static void write32(volatile uint8_t *base, uint32_t offset, uint32_t value)
{
    *(volatile uint32_t *)(base + offset) = value;
}

static void write64(volatile uint8_t *base, uint32_t offset, uint64_t value)
{
    write32(base, offset, (uint32_t)value);
    write32(base, offset + 4, (uint32_t)(value >> 32));
}

/* Poll until (register & mask) == value. */
static bool wait_bits(volatile uint8_t *base, uint32_t offset, uint32_t mask, uint32_t value, uint32_t timeout_ms)
{
    for (uint32_t waited = 0;; waited++) {
        if ((read32(base, offset) & mask) == value)
            return true;
        if (waited >= timeout_ms)
            return false;
        thread_sleep(1000000);
    }
}

/* --- Rings ----------------------------------------------------------------------- */

static status_t ring_alloc(xhci_t *hc, xhci_ring_t *ring)
{
    status_t status = dma_alloc(hc->device, RING_TRBS * sizeof(xhci_trb_t), hc->dma_limit, &ring->memory);
    if (STATUS_IS_ERROR(status))
        return status;
    ring->trbs = ring->memory.virt;
    ring->enqueue = 0;
    ring->cycle = 1;
    return STATUS_SUCCESS;
}

static void ring_free(xhci_ring_t *ring)
{
    dma_free(&ring->memory);
    ring->trbs = NULL;
}

/* Queue a TRB; the controller owns it as soon as its cycle bit is written. */
static void ring_push(xhci_ring_t *ring, uint64_t parameter, uint32_t status, uint32_t control)
{
    xhci_trb_t *trb = &ring->trbs[ring->enqueue];
    trb->parameter = parameter;
    trb->status = status;
    trb->control = control | ring->cycle;

    if (++ring->enqueue == RING_TRBS - 1) {
        xhci_trb_t *link = &ring->trbs[RING_TRBS - 1];
        link->parameter = ring->memory.phys;
        link->status = 0;
        link->control = TRB_TYPE(TRB_LINK) | TRB_TOGGLE_CYCLE | ring->cycle;
        ring->enqueue = 0;
        ring->cycle ^= 1;
    }
}

static uint64_t ring_enqueue_address(const xhci_ring_t *ring)
{
    return ring->memory.phys + ring->enqueue * sizeof(xhci_trb_t);
}

/* --- Events ---------------------------------------------------------------------- */

static void wait_init(xhci_wait_t *wait)
{
    memset(wait, 0, sizeof(*wait));
    wait_queue_init(&wait->queue);
}

/* Interrupts disabled. */
static status_t wait_for(xhci_wait_t *wait, uint64_t timeout_ns)
{
    uint64_t deadline = wait_deadline(timeout_ns);
    status_t status = STATUS_SUCCESS;
    while (!wait->done && status == STATUS_SUCCESS)
        status = wait_queue_block_uninterruptible(&wait->queue, deadline);
    return wait->done ? STATUS_SUCCESS : STATUS_TIMEOUT;
}

static void wait_finish(xhci_wait_t *wait, uint32_t code, uint32_t slot)
{
    wait->code = code;
    wait->slot = slot;
    wait->done = true;
    wait_queue_wake_all(&wait->queue, STATUS_SUCCESS);
}

static void transfer_event(xhci_t *hc, const xhci_trb_t *event)
{
    uint32_t code = event->status >> 24, residual = event->status & 0xFFFFFF;
    uint32_t slot_id = event->control >> 24, endpoint = (event->control >> 16) & 0x1F;
    xhci_slot_t *slot = slot_id <= MAX_SLOTS ? hc->slots[slot_id] : NULL;

    if (!slot)
        return;
    if (endpoint == 1) {
        xhci_wait_t *wait = slot->control_wait;
        if (!wait)
            return;
        if (code == CODE_SHORT_PACKET)
            wait->residual = residual; /* the data stage; the status stage still follows */
        else
            wait_finish(wait, code, slot_id);
        return;
    }
    usb_transfer_t *transfer = slot->pending[endpoint];
    if (!transfer)
        return; /* cancelled, or the "stopped" notice of a Stop Endpoint command */
    slot->pending[endpoint] = NULL;
    if (code == CODE_SUCCESS || code == CODE_SHORT_PACKET)
        transfer->complete(transfer, STATUS_SUCCESS, residual <= transfer->length ? transfer->length - residual : 0);
    else
        transfer->complete(transfer, STATUS_IO_ERROR, 0);
}

static void xhci_interrupt(void *context)
{
    xhci_t *hc = context;
    xhci_trb_t *events = hc->events.virt;

    write32(hc->op, OP_USBSTS, STS_EINT);
    write32(hc->interrupter, IR_IMAN, read32(hc->interrupter, IR_IMAN) | IMAN_PENDING);

    for (;;) {
        xhci_trb_t *event = &events[hc->event_dequeue];
        if ((event->control & TRB_CYCLE) != hc->event_cycle)
            break;
        switch (TRB_TYPE_OF(event->control)) {
        case TRB_TRANSFER_EVENT:
            transfer_event(hc, event);
            break;
        case TRB_COMMAND_COMPLETION:
            if (hc->command_wait)
                wait_finish(hc->command_wait, event->status >> 24, event->control >> 24);
            break;
        case TRB_PORT_STATUS_CHANGE:
            hc->ports_changed = true;
            wait_queue_wake_all(&hc->port_wakeup, STATUS_SUCCESS);
            break;
        }
        if (++hc->event_dequeue == RING_TRBS) {
            hc->event_dequeue = 0;
            hc->event_cycle ^= 1;
        }
    }
    write64(hc->interrupter, IR_ERDP, (hc->events.phys + hc->event_dequeue * sizeof(xhci_trb_t)) | ERDP_BUSY);
}

/* --- Commands -------------------------------------------------------------------- */

/* Run one command and wait for its completion. hc->lock held. */
static status_t command(xhci_t *hc, uint64_t parameter, uint32_t control, uint32_t *slot)
{
    xhci_wait_t wait;
    wait_init(&wait);

    uint64_t flags = arch_interrupts_save();
    hc->command_wait = &wait;
    ring_push(&hc->command, parameter, 0, control);
    hc->doorbells[0] = 0;
    status_t status = wait_for(&wait, COMMAND_TIMEOUT_NS);
    hc->command_wait = NULL;
    arch_interrupts_restore(flags);

    if (STATUS_IS_ERROR(status)) {
        klog_warn("xhci: command %u timed out", TRB_TYPE_OF(control));
        return status;
    }
    if (wait.code != CODE_SUCCESS) {
        klog_debug("xhci: command %u failed with code %u", TRB_TYPE_OF(control), wait.code);
        return STATUS_DEVICE_ERROR;
    }
    if (slot)
        *slot = wait.slot;
    return STATUS_SUCCESS;
}

/* --- Contexts -------------------------------------------------------------------- */

/* Context `index` of the input context: 0 = input control, 1 = slot, 2 = endpoint 0, n + 1 = device context index n. */
static uint32_t *input_context(xhci_slot_t *slot, uint32_t index)
{
    return (uint32_t *)((uint8_t *)slot->input.virt + index * slot->hc->context_size);
}

static uint32_t *output_context(xhci_slot_t *slot, uint32_t index)
{
    return (uint32_t *)((uint8_t *)slot->output.virt + index * slot->hc->context_size);
}

static void fill_endpoint(xhci_slot_t *slot, uint32_t dci, uint32_t type, uint32_t max_packet, uint32_t interval)
{
    uint32_t *ep = input_context(slot, dci + 1);
    uint64_t dequeue = slot->rings[dci].memory.phys | 1; /* dequeue cycle state */
    bool periodic = (type & 3) == 1 || (type & 3) == 3;

    memset(ep, 0, slot->hc->context_size);
    ep[0] = interval << 16;
    ep[1] = (3u << 1) | (type << 3) | (max_packet << 16); /* 3 retries */
    ep[2] = (uint32_t)dequeue;
    ep[3] = (uint32_t)(dequeue >> 32);
    ep[4] = (type == 4 ? 8 : max_packet) | (periodic ? max_packet << 16 : 0); /* average TRB length, max ESIT payload */
}

static uint32_t endpoint_dci(uint8_t address)
{
    return (address & 0x0F) * 2 + ((address & USB_ENDPOINT_IN) ? 1 : 0);
}

/* The Interval field: the period is 125 us * 2^interval. */
static uint32_t endpoint_interval(uint8_t speed, const usb_endpoint_descriptor_t *e)
{
    uint32_t type = e->attributes & USB_ENDPOINT_TYPE_MASK, b = e->interval ? e->interval : 1;

    if (type == USB_ENDPOINT_CONTROL || type == USB_ENDPOINT_BULK)
        return 0;
    if (speed == USB_SPEED_HIGH || speed == USB_SPEED_SUPER)
        return b > 16 ? 15 : b - 1;
    /* Full and low speed count in frames of 1 ms. */
    uint32_t exponent = 3;
    while (exponent < 10 && (1u << (exponent + 1)) <= b * 8)
        exponent++;
    return exponent;
}

/* --- Host controller operations ---------------------------------------------------- */

/* A halted endpoint (stall): reset it and continue behind what was queued. hc->lock held. */
static void recover_endpoint(xhci_slot_t *slot, uint32_t dci)
{
    xhci_t *hc = slot->hc;
    uint32_t target = (uint32_t)slot->id << 24 | dci << 16;

    command(hc, 0, TRB_TYPE(TRB_RESET_ENDPOINT) | target, NULL);
    command(hc, ring_enqueue_address(&slot->rings[dci]) | slot->rings[dci].cycle, TRB_TYPE(TRB_SET_TR_DEQUEUE) | target,
            NULL);
}

static status_t xhci_control(usb_device_t *usb, const usb_setup_t *setup, void *data, uint32_t *transferred)
{
    xhci_slot_t *slot = usb->host_data;
    xhci_t *hc = slot->hc;
    xhci_ring_t *ring = &slot->rings[1];
    bool in = setup->request_type & USB_DIR_IN;
    uint32_t length = setup->length;
    uint64_t setup_raw;
    xhci_wait_t wait;

    if (length > slot->control_data.size || (length && !data))
        return STATUS_INVALID_ARGUMENT;
    mutex_lock(&hc->lock);
    if (slot->gone) {
        mutex_unlock(&hc->lock);
        return STATUS_NOT_FOUND;
    }
    if (length && !in)
        memcpy(slot->control_data.virt, data, length);
    memcpy(&setup_raw, setup, sizeof(setup_raw));
    wait_init(&wait);

    uint64_t flags = arch_interrupts_save();
    slot->control_wait = &wait;
    ring_push(ring, setup_raw, 8, TRB_TYPE(TRB_SETUP) | TRB_IDT | (length ? (in ? 3u : 2u) << 16 : 0));
    if (length)
        ring_push(ring, slot->control_data.phys, length, TRB_TYPE(TRB_DATA) | TRB_ISP | (in ? TRB_DIR_IN : 0));
    ring_push(ring, 0, 0, TRB_TYPE(TRB_STATUS) | TRB_IOC | ((length && in) ? 0 : TRB_DIR_IN));
    hc->doorbells[slot->id] = 1;
    status_t status = wait_for(&wait, CONTROL_TIMEOUT_NS);
    slot->control_wait = NULL;
    arch_interrupts_restore(flags);

    if (!STATUS_IS_ERROR(status) && wait.code != CODE_SUCCESS) {
        /* A stall is the device's way to say "request not supported". */
        status = wait.code == CODE_STALL ? STATUS_NOT_SUPPORTED : STATUS_IO_ERROR;
        recover_endpoint(slot, 1);
    }
    if (!STATUS_IS_ERROR(status)) {
        uint32_t done = wait.residual <= length ? length - wait.residual : 0;
        if (in && done)
            memcpy(data, slot->control_data.virt, done);
        if (transferred)
            *transferred = done;
    }
    mutex_unlock(&hc->lock);
    return status;
}

static status_t xhci_set_max_packet0(usb_device_t *usb, uint32_t max_packet)
{
    xhci_slot_t *slot = usb->host_data;
    xhci_t *hc = slot->hc;

    mutex_lock(&hc->lock);
    uint32_t *control = input_context(slot, 0);
    control[0] = 0;
    control[1] = 1u << 1; /* evaluate endpoint 0 */
    uint32_t *ep = input_context(slot, 2);
    memcpy(ep, output_context(slot, 1), hc->context_size);
    ep[1] = (ep[1] & 0xFFFF) | (max_packet << 16);
    status_t status = command(hc, slot->input.phys, TRB_TYPE(TRB_EVALUATE_CONTEXT) | (uint32_t)slot->id << 24, NULL);
    mutex_unlock(&hc->lock);
    return status;
}

static status_t xhci_configure(usb_device_t *usb, const usb_endpoint_descriptor_t *const *endpoints, uint32_t count)
{
    xhci_slot_t *slot = usb->host_data;
    xhci_t *hc = slot->hc;
    uint32_t add = 1, last = 1; /* the slot context is always evaluated */
    status_t status = STATUS_SUCCESS;

    if (count == 0)
        return STATUS_SUCCESS;
    mutex_lock(&hc->lock);
    for (uint32_t i = 0; i < count && !STATUS_IS_ERROR(status); i++) {
        const usb_endpoint_descriptor_t *e = endpoints[i];
        uint32_t dci = endpoint_dci(e->address), type = e->attributes & USB_ENDPOINT_TYPE_MASK;
        if (dci < 2 || slot->rings[dci].trbs)
            continue;
        status = ring_alloc(hc, &slot->rings[dci]);
        if (STATUS_IS_ERROR(status))
            break;
        /* Endpoint types: 1-3 = isochronous, bulk, interrupt OUT; 4 = control; 5-7 = the same IN */
        uint32_t xhci_type = type == USB_ENDPOINT_CONTROL ? 4 : type | ((e->address & USB_ENDPOINT_IN) ? 4 : 0);
        fill_endpoint(slot, dci, xhci_type, e->max_packet_size & 0x7FF, endpoint_interval(usb->speed, e));
        add |= 1u << dci;
        if (dci > last)
            last = dci;
    }
    if (!STATUS_IS_ERROR(status)) {
        uint32_t *control = input_context(slot, 0), *slot_context = input_context(slot, 1);
        control[0] = 0;
        control[1] = add;
        memcpy(slot_context, output_context(slot, 0), hc->context_size);
        slot_context[0] = (slot_context[0] & ~(0x1Fu << 27)) | (last << 27); /* context entries */
        status = command(hc, slot->input.phys, TRB_TYPE(TRB_CONFIGURE_ENDPOINT) | (uint32_t)slot->id << 24, NULL);
    }
    mutex_unlock(&hc->lock);
    return status;
}

static status_t xhci_submit(usb_device_t *usb, uint8_t endpoint_address, usb_transfer_t *transfer)
{
    xhci_slot_t *slot = usb->host_data;
    uint32_t dci = endpoint_dci(endpoint_address);
    status_t status = STATUS_SUCCESS;

    uint64_t flags = arch_interrupts_save();
    if (slot->gone || dci < 2 || !slot->rings[dci].trbs) {
        status = STATUS_NOT_FOUND;
    } else if (slot->pending[dci]) {
        status = STATUS_BUSY;
    } else {
        slot->pending[dci] = transfer;
        ring_push(&slot->rings[dci], transfer->buffer_phys, transfer->length, TRB_TYPE(TRB_NORMAL) | TRB_IOC | TRB_ISP);
        slot->hc->doorbells[slot->id] = dci;
    }
    arch_interrupts_restore(flags);
    return status;
}

/* Forget the endpoint's pending transfer and make the endpoint usable again, whether it was running or halted. */
static status_t xhci_reset_endpoint(usb_device_t *usb, uint8_t endpoint_address)
{
    xhci_slot_t *slot = usb->host_data;
    xhci_t *hc = slot->hc;
    uint32_t dci = endpoint_dci(endpoint_address);

    if (dci < 2 || !slot->rings[dci].trbs)
        return STATUS_NOT_FOUND;
    mutex_lock(&hc->lock);
    uint64_t flags = arch_interrupts_save();
    slot->pending[dci] = NULL;
    arch_interrupts_restore(flags);
    if (!slot->gone) {
        uint32_t target = (uint32_t)slot->id << 24 | dci << 16;
        /* One of the two applies (running: stop; halted: reset); the other fails harmlessly. */
        command(hc, 0, TRB_TYPE(TRB_STOP_ENDPOINT) | target, NULL);
        command(hc, 0, TRB_TYPE(TRB_RESET_ENDPOINT) | target, NULL);
        command(hc, ring_enqueue_address(&slot->rings[dci]) | slot->rings[dci].cycle,
                TRB_TYPE(TRB_SET_TR_DEQUEUE) | target, NULL);
    }
    mutex_unlock(&hc->lock);
    return STATUS_SUCCESS;
}

static status_t xhci_hub_configure(usb_device_t *usb, uint32_t ports, uint32_t think_time)
{
    xhci_slot_t *slot = usb->host_data;
    xhci_t *hc = slot->hc;

    mutex_lock(&hc->lock);
    uint32_t *control = input_context(slot, 0), *slot_context = input_context(slot, 1);
    control[0] = 0;
    control[1] = 1; /* the slot context only */
    memcpy(slot_context, output_context(slot, 0), hc->context_size);
    slot_context[0] |= 1u << 26; /* a hub */
    slot_context[1] = (slot_context[1] & 0x00FFFFFF) | ports << 24;
    if (usb->speed == USB_SPEED_HIGH)
        slot_context[2] = (slot_context[2] & ~(3u << 16)) | (think_time & 3) << 16;
    status_t status = command(hc, slot->input.phys, TRB_TYPE(TRB_CONFIGURE_ENDPOINT) | (uint32_t)slot->id << 24, NULL);
    mutex_unlock(&hc->lock);
    return status;
}

static status_t device_create(xhci_t *hc, xhci_slot_t *parent, uint32_t port, uint32_t speed, xhci_slot_t **result);
static void device_destroy(xhci_slot_t *slot);

static status_t xhci_child_attach(usb_device_t *hub, uint32_t port, uint8_t speed, usb_device_t **child)
{
    xhci_slot_t *parent = hub->host_data, *slot;

    if (parent->gone)
        return STATUS_NOT_FOUND;
    status_t status = device_create(parent->hc, parent, port, speed, &slot);
    if (!STATUS_IS_ERROR(status))
        *child = &slot->usb;
    return status;
}

static void xhci_child_detach(usb_device_t *child)
{
    device_destroy(child->host_data);
}

static const usb_host_ops_t xhci_ops = {
    .control = xhci_control,
    .set_max_packet0 = xhci_set_max_packet0,
    .configure = xhci_configure,
    .submit = xhci_submit,
    .reset_endpoint = xhci_reset_endpoint,
    .hub_configure = xhci_hub_configure,
    .child_attach = xhci_child_attach,
    .child_detach = xhci_child_detach,
};

/* --- Root hub ports -------------------------------------------------------------- */

static uint32_t port_read(xhci_t *hc, uint32_t port)
{
    return read32(hc->op, OP_PORTSC + 0x10 * (port - 1));
}

/* Write action bits without disabling the port or touching other changes. */
static void port_write(xhci_t *hc, uint32_t port, uint32_t bits)
{
    write32(hc->op, OP_PORTSC + 0x10 * (port - 1), (port_read(hc, port) & PORT_PRESERVE) | bits);
}

static void slot_free(xhci_slot_t *slot)
{
    for (uint32_t i = 0; i < ENDPOINTS; i++)
        ring_free(&slot->rings[i]);
    dma_free(&slot->output);
    dma_free(&slot->input);
    dma_free(&slot->control_data);
    kfree(slot);
}

/* The device is gone: end its transfers, give the slot back, unbind the drivers (a hub's children first). */
static void device_destroy(xhci_slot_t *slot)
{
    xhci_t *hc = slot->hc;
    uint64_t *dcbaa = hc->dcbaa.virt;
    usb_transfer_t *dead[ENDPOINTS];

    if (slot->usb.path[0])
        klog_info("usb: port %s: %s disconnected", slot->usb.path, slot->usb.product);
    mutex_lock(&hc->lock);
    slot->gone = true;
    if (slot->id)
        command(hc, 0, TRB_TYPE(TRB_DISABLE_SLOT) | (uint32_t)slot->id << 24, NULL);
    uint64_t flags = arch_interrupts_save();
    if (slot->id) {
        hc->slots[slot->id] = NULL;
        dcbaa[slot->id] = 0;
    }
    memcpy(dead, slot->pending, sizeof(dead));
    memset(slot->pending, 0, sizeof(slot->pending));
    arch_interrupts_restore(flags);
    mutex_unlock(&hc->lock);

    /* Whoever waits for a transfer learns that it will never come. */
    for (uint32_t i = 0; i < ENDPOINTS; i++) {
        if (dead[i])
            dead[i]->complete(dead[i], STATUS_NOT_FOUND, 0);
    }
    usb_device_detach(&slot->usb);
    slot_free(slot);
}

/*
 * A device was detected and reset on a root port (parent NULL) or on a
 * hub's port: enable a slot, set the address and let the USB core
 * enumerate it.
 */
static status_t device_create(xhci_t *hc, xhci_slot_t *parent, uint32_t port, uint32_t speed, xhci_slot_t **result)
{
    static const uint16_t default_packet[] = { [USB_SPEED_FULL] = 8, [USB_SPEED_LOW] = 8, [USB_SPEED_HIGH] = 64,
                                               [USB_SPEED_SUPER] = 512 };
    uint64_t *dcbaa = hc->dcbaa.virt;
    uint32_t id = 0;

    if (speed < USB_SPEED_FULL || speed > USB_SPEED_SUPER || port == 0 || port > 255 ||
        (parent && (parent->usb.depth >= USB_MAX_DEPTH || port > 15)))
        return STATUS_NOT_SUPPORTED;
    xhci_slot_t *slot = kcalloc(1, sizeof(*slot));
    if (!slot)
        return STATUS_OUT_OF_MEMORY;
    slot->hc = hc;
    slot->usb.host = &xhci_ops;
    slot->usb.host_data = slot;
    slot->usb.controller = hc->device;
    slot->usb.port = (uint8_t)port;
    slot->usb.speed = (uint8_t)speed;
    if (parent) {
        slot->usb.hub = &parent->usb;
        slot->usb.depth = (uint8_t)(parent->usb.depth + 1);
        slot->root_port = parent->root_port;
        slot->route = parent->route | port << (4 * parent->usb.depth);
        if (speed == USB_SPEED_LOW || speed == USB_SPEED_FULL) {
            if (parent->usb.speed == USB_SPEED_HIGH) {
                slot->tt_slot = parent->id;
                slot->tt_port = (uint8_t)port;
            } else {
                slot->tt_slot = parent->tt_slot;
                slot->tt_port = parent->tt_port;
            }
        }
    } else {
        slot->root_port = port;
    }

    mutex_lock(&hc->lock);
    status_t status = command(hc, 0, TRB_TYPE(TRB_ENABLE_SLOT), &id);
    if (!STATUS_IS_ERROR(status) && (id == 0 || id > hc->max_slots))
        status = STATUS_LIMIT_EXCEEDED;
    if (!STATUS_IS_ERROR(status))
        slot->id = (uint8_t)id;
    if (!STATUS_IS_ERROR(status))
        status = dma_alloc(hc->device, PAGE_SIZE, hc->dma_limit, &slot->output);
    if (!STATUS_IS_ERROR(status))
        status = dma_alloc(hc->device, PAGE_SIZE, hc->dma_limit, &slot->input);
    if (!STATUS_IS_ERROR(status))
        status = dma_alloc(hc->device, PAGE_SIZE, hc->dma_limit, &slot->control_data);
    if (!STATUS_IS_ERROR(status))
        status = ring_alloc(hc, &slot->rings[1]);
    if (!STATUS_IS_ERROR(status)) {
        uint32_t *control = input_context(slot, 0), *slot_context = input_context(slot, 1);
        control[1] = 3; /* slot and endpoint 0 */
        slot_context[0] = slot->route | (speed << 20) | (1u << 27); /* one context entry */
        slot_context[1] = slot->root_port << 16;
        slot_context[2] = slot->tt_slot | (uint32_t)slot->tt_port << 8;
        fill_endpoint(slot, 1, 4, default_packet[speed], 0);

        uint64_t flags = arch_interrupts_save();
        dcbaa[id] = slot->output.phys;
        hc->slots[id] = slot;
        arch_interrupts_restore(flags);
        status = command(hc, slot->input.phys, TRB_TYPE(TRB_ADDRESS_DEVICE) | id << 24, NULL);
    }
    mutex_unlock(&hc->lock);

    if (!STATUS_IS_ERROR(status))
        status = usb_device_attach(&slot->usb);
    if (STATUS_IS_ERROR(status)) {
        slot->usb.path[0] = '\0'; /* nothing was announced: leave quietly */
        device_destroy(slot);
        return status;
    }
    *result = slot;
    return STATUS_SUCCESS;
}

static void port_connect(xhci_t *hc, uint32_t port)
{
    /* USB 3 ports enable themselves; USB 2 ports need a reset. */
    if (!(port_read(hc, port) & PORT_ENABLED)) {
        port_write(hc, port, PORT_RESET);
        for (int i = 0; i < 50 && !(port_read(hc, port) & PORT_PRC); i++)
            thread_sleep(10000000);
        port_write(hc, port, PORT_PRC);
        if (!(port_read(hc, port) & PORT_ENABLED)) {
            klog_warn("usb: port %u: reset failed (status 0x%x)", port, port_read(hc, port));
            return;
        }
    }
    thread_sleep(20000000); /* reset recovery */

    status_t status = device_create(hc, NULL, port, PORT_SPEED(port_read(hc, port)), &hc->port_slots[port]);
    if (STATUS_IS_ERROR(status)) {
        hc->port_slots[port] = NULL;
        klog_warn("usb: port %u: cannot set up the device: %s", port, status_name(status));
    }
}

static void port_disconnect(xhci_t *hc, uint32_t port)
{
    xhci_slot_t *slot = hc->port_slots[port];
    hc->port_slots[port] = NULL;
    device_destroy(slot);
}

static void port_check(xhci_t *hc, uint32_t port)
{
    uint32_t value = port_read(hc, port);

    if (value & PORT_CHANGES)
        port_write(hc, port, value & PORT_CHANGES);
    /* A connect change with a device still there means it was replaced. */
    if (hc->port_slots[port] && (!(value & PORT_CONNECTED) || (value & PORT_CSC)))
        port_disconnect(hc, port);
    if ((value & PORT_CONNECTED) && !hc->port_slots[port])
        port_connect(hc, port);
}

static void xhci_thread(void *context)
{
    xhci_t *hc = context;

    for (;;) {
        uint64_t flags = arch_interrupts_save();
        while (!hc->ports_changed)
            wait_queue_block_uninterruptible(&hc->port_wakeup, WAIT_FOREVER);
        hc->ports_changed = false;
        arch_interrupts_restore(flags);

        for (uint32_t port = 1; port <= hc->max_ports; port++)
            port_check(hc, port);
    }
}

/* --- Controller start -------------------------------------------------------------- */

/* Take the controller from the firmware (USB legacy support capability). */
static void take_ownership(xhci_t *hc, uint32_t hccparams)
{
    uint32_t offset = (hccparams >> 16) << 2;

    while (offset) {
        uint32_t capability = read32(hc->base, offset);
        if ((capability & 0xFF) == 1) {
            write32(hc->base, offset, capability | (1u << 24)); /* OS owned */
            if (!wait_bits(hc->base, offset, 1u << 16, 0, 1000))
                klog_warn("xhci: the firmware does not release the controller");
            /* No SMIs for the firmware any more; the top bits are write-1-to-clear. */
            write32(hc->base, offset + 4, (read32(hc->base, offset + 4) & 0x1FFF1FEE) | 0xE0000000);
        }
        uint32_t next = (capability >> 8) & 0xFF;
        if (!next)
            break;
        offset += next << 2;
    }
}

static status_t xhci_start(xhci_t *hc, pci_device_t *pci)
{
    uint32_t hcs1 = read32(hc->base, CAP_HCSPARAMS1), hcs2 = read32(hc->base, CAP_HCSPARAMS2);
    uint32_t hcc = read32(hc->base, CAP_HCCPARAMS1);

    hc->op = hc->base + (read32(hc->base, CAP_CAPLENGTH) & 0xFF);
    hc->interrupter = hc->base + (read32(hc->base, CAP_RTSOFF) & ~0x1Fu) + 0x20;
    hc->doorbells = (volatile uint32_t *)(hc->base + (read32(hc->base, CAP_DBOFF) & ~3u));
    hc->max_ports = hcs1 >> 24;
    hc->max_slots = (hcs1 & 0xFF) < MAX_SLOTS ? (hcs1 & 0xFF) : MAX_SLOTS;
    hc->context_size = (hcc & HCC_CSZ) ? 64 : 32;
    hc->dma_limit = (hcc & HCC_AC64) ? ~0ull : 0xFFFFFFFFull;
    hc->event_cycle = 1;

    take_ownership(hc, hcc);

    /* Halt, reset, wait until the controller is ready. */
    write32(hc->op, OP_USBCMD, read32(hc->op, OP_USBCMD) & ~CMD_RUN);
    if (!wait_bits(hc->op, OP_USBSTS, STS_HALTED, STS_HALTED, 100))
        return STATUS_TIMEOUT;
    write32(hc->op, OP_USBCMD, CMD_RESET);
    if (!wait_bits(hc->op, OP_USBCMD, CMD_RESET, 0, 1000) || !wait_bits(hc->op, OP_USBSTS, STS_NOT_READY, 0, 1000))
        return STATUS_TIMEOUT;

    hc->port_slots = kcalloc(hc->max_ports + 1, sizeof(*hc->port_slots));
    if (!hc->port_slots)
        return STATUS_OUT_OF_MEMORY;
    status_t status = dma_alloc(hc->device, PAGE_SIZE, hc->dma_limit, &hc->dcbaa);
    if (!STATUS_IS_ERROR(status))
        status = ring_alloc(hc, &hc->command);
    if (!STATUS_IS_ERROR(status))
        status = dma_alloc(hc->device, RING_TRBS * sizeof(xhci_trb_t), hc->dma_limit, &hc->events);
    if (!STATUS_IS_ERROR(status))
        status = dma_alloc(hc->device, PAGE_SIZE, hc->dma_limit, &hc->erst);

    /* Pages the controller wants for itself */
    uint32_t scratchpads = ((hcs2 >> 21) & 0x1F) << 5 | ((hcs2 >> 27) & 0x1F);
    if (!STATUS_IS_ERROR(status) && scratchpads) {
        status = dma_alloc(hc->device, scratchpads * sizeof(uint64_t), hc->dma_limit, &hc->scratchpad_array);
        if (!STATUS_IS_ERROR(status))
            status = dma_alloc(hc->device, scratchpads * PAGE_SIZE, hc->dma_limit, &hc->scratchpads);
        if (!STATUS_IS_ERROR(status)) {
            uint64_t *array = hc->scratchpad_array.virt;
            for (uint32_t i = 0; i < scratchpads; i++)
                array[i] = hc->scratchpads.phys + i * PAGE_SIZE;
            *(uint64_t *)hc->dcbaa.virt = hc->scratchpad_array.phys;
        }
    }
    if (STATUS_IS_ERROR(status))
        return status;

    write32(hc->op, OP_CONFIG, hc->max_slots);
    write64(hc->op, OP_DCBAAP, hc->dcbaa.phys);
    write64(hc->op, OP_CRCR, hc->command.memory.phys | 1); /* ring cycle state */

    /* Event ring: one segment */
    uint64_t *segment = hc->erst.virt;
    segment[0] = hc->events.phys;
    segment[1] = RING_TRBS;
    write32(hc->interrupter, IR_ERSTSZ, 1);
    write64(hc->interrupter, IR_ERDP, hc->events.phys);
    write64(hc->interrupter, IR_ERSTBA, hc->erst.phys);
    write32(hc->interrupter, IR_IMOD, 400); /* at most one interrupt per 100 us */

    status = pci_enable_msix(pci, 0, xhci_interrupt, hc, &hc->irq);
    if (STATUS_IS_ERROR(status))
        status = pci_enable_msi(pci, xhci_interrupt, hc, &hc->irq);
    if (STATUS_IS_ERROR(status)) {
        klog_error("xhci: no MSI or MSI-X interrupt: %s", status_name(status));
        return status;
    }
    write32(hc->interrupter, IR_IMAN, IMAN_ENABLE | IMAN_PENDING);
    write32(hc->op, OP_USBCMD, CMD_RUN | CMD_INTE);
    if (!wait_bits(hc->op, OP_USBSTS, STS_HALTED, 0, 100))
        return STATUS_TIMEOUT;
    for (uint32_t port = 1; port <= hc->max_ports; port++) {
        if (!(port_read(hc, port) & PORT_POWER))
            port_write(hc, port, PORT_POWER);
    }
    return STATUS_SUCCESS;
}

static status_t xhci_probe(device_t *device)
{
    pci_device_t *pci = pci_from_device(device);
    thread_t *thread;

    if (device->id.prog_if != 0x30) /* 0x00 UHCI, 0x10 OHCI, 0x20 EHCI */
        return STATUS_NOT_SUPPORTED;
    xhci_t *hc = kcalloc(1, sizeof(*hc));
    if (!hc)
        return STATUS_OUT_OF_MEMORY;
    hc->device = device;
    mutex_init(&hc->lock);
    wait_queue_init(&hc->port_wakeup);

    status_t status = pci_enable_device(pci, true);
    if (!STATUS_IS_ERROR(status)) {
        hc->base = (volatile uint8_t *)pci_map_bar(pci, 0);
        status = hc->base ? xhci_start(hc, pci) : STATUS_DEVICE_ERROR;
    }
    if (!STATUS_IS_ERROR(status))
        status = thread_create_kernel("xhci", xhci_thread, hc, THREAD_PRIORITY_KERNEL, &thread);
    if (STATUS_IS_ERROR(status)) {
        if (hc->op)
            write32(hc->op, OP_USBCMD, 0);
        pci_disable_msix(pci);
        pci_disable_msi(pci);
        ring_free(&hc->command);
        dma_free(&hc->dcbaa);
        dma_free(&hc->events);
        dma_free(&hc->erst);
        dma_free(&hc->scratchpad_array);
        dma_free(&hc->scratchpads);
        kfree(hc->port_slots);
        kfree(hc);
        return status;
    }
    device->driver_data = hc;
    klog_info("xhci: %s: %u ports, %u device slots, %u-byte contexts", device->name, hc->max_ports, hc->max_slots,
              hc->context_size);

    hc->ports_changed = true; /* devices that are already plugged in */
    thread_start(thread);
    return STATUS_SUCCESS;
}

static const device_match_t xhci_ids[] = {
    DEVICE_MATCH_CLASS(0x0C, 0x03), /* serial bus, USB */
    DEVICE_MATCH_END,
};

static driver_t xhci_driver = {
    .name = "xhci",
    .bus_name = "pci",
    .version = 1,
    .capabilities = DRIVER_CAP_BUS,
    .ids = xhci_ids,
    .probe = xhci_probe,
};

static status_t xhci_module_init(void)
{
    return driver_register(&xhci_driver);
}

static const char *const xhci_dependencies[] = { "pci", "usb", NULL };

MODULE(.name = "xhci", .description = "xHCI USB host controllers", .version = 1,
       .min_kernel_version = KERNEL_VERSION(0, 11, 0), .dependencies = xhci_dependencies, .init = xhci_module_init);
