/*
 * Resource management: every MMIO range, I/O port range, interrupt or DMA
 * window a running device uses is claimed here, so two drivers can never
 * own overlapping hardware.
 */

#include "drivers/core/device.h"

#include "core/arch.h"
#include "core/export.h"
#include "memory/heap.h"

typedef struct {
    list_node_t     node;
    resource_t      resource;
    const device_t *owner;
} claim_t;

static list_t claims = { { &claims.head, &claims.head } };

static bool overlaps(const resource_t *a, const resource_t *b)
{
    return a->type == b->type && a->start < b->start + b->size && b->start < a->start + a->size;
}

status_t resource_claim(const resource_t *resource, const device_t *owner)
{
    if (resource->size == 0)
        return STATUS_SUCCESS;

    list_for_each(node, &claims) {
        claim_t *c = container_of(node, claim_t, node);
        if (overlaps(&c->resource, resource))
            return c->owner == owner ? STATUS_SUCCESS : STATUS_BUSY;
    }

    claim_t *c = kmalloc(sizeof(*c));
    if (!c)
        return STATUS_OUT_OF_MEMORY;
    c->resource = *resource;
    c->owner = owner;
    list_push_back(&claims, &c->node);
    return STATUS_SUCCESS;
}

void resource_release_all(const device_t *owner)
{
    list_for_each_safe(node, &claims) {
        claim_t *c = container_of(node, claim_t, node);
        if (c->owner == owner) {
            list_remove(node);
            kfree(c);
        }
    }
}

EXPORT_SYMBOL(resource_claim);
