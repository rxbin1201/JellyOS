/*
 * Intrusive doubly linked lists.
 *
 * A node that is not in any list has next == NULL; list_remove() restores
 * that state, so membership can be tested with list_linked().
 */

#ifndef CORE_LIST_H
#define CORE_LIST_H

#include <stdbool.h>
#include <stddef.h>

typedef struct list_node {
    struct list_node *prev;
    struct list_node *next;
} list_node_t;

typedef struct {
    list_node_t head;
} list_t;

#define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))

static inline void list_init(list_t *list)
{
    list->head.prev = list->head.next = &list->head;
}

static inline bool list_empty(const list_t *list)
{
    return list->head.next == &list->head;
}

static inline bool list_linked(const list_node_t *node)
{
    return node->next != NULL;
}

static inline void list_insert_before(list_node_t *position, list_node_t *node)
{
    node->prev = position->prev;
    node->next = position;
    position->prev->next = node;
    position->prev = node;
}

static inline void list_push_back(list_t *list, list_node_t *node)
{
    list_insert_before(&list->head, node);
}

static inline void list_remove(list_node_t *node)
{
    node->prev->next = node->next;
    node->next->prev = node->prev;
    node->prev = node->next = NULL;
}

static inline list_node_t *list_front(const list_t *list)
{
    return list_empty(list) ? NULL : list->head.next;
}

static inline list_node_t *list_pop_front(list_t *list)
{
    list_node_t *node = list_front(list);
    if (node)
        list_remove(node);
    return node;
}

#define list_for_each(node, list) \
    for (list_node_t *node = (list)->head.next; node != &(list)->head; node = node->next)

/* Safe against removal of the current node. */
#define list_for_each_safe(node, list)                                                     \
    for (list_node_t *node = (list)->head.next, *node##_next = node->next;                 \
         node != &(list)->head; node = node##_next, node##_next = node->next)

#endif
