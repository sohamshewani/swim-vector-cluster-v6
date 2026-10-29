#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <assert.h>

#define SWIM_MAX_CLUSTER_NODES 256
#define UUID_SIZE 16

typedef struct {
    uint8_t bytes[UUID_SIZE];
} node_id_t;

typedef struct {
    node_id_t node_id;
    uint64_t counter;
} vc_entry_t;

typedef struct {
    uint32_t capacity;
    uint32_t count;
    uint64_t lamport_time;
    vc_entry_t entries[SWIM_MAX_CLUSTER_NODES];
} vc_clock_t;

typedef enum {
    VC_CMP_DOMINATES = 1,
    VC_CMP_DOMINATED_BY = -1,
    VC_CMP_CONCURRENT = 0,
    VC_CMP_EQUAL = 2
} vc_cmp_result_t;

static inline int node_id_compare(const node_id_t *a, const node_id_t *b) {
    return memcmp(a->bytes, b->bytes, UUID_SIZE);
}

void vc_init(vc_clock_t *clock, uint32_t initial_capacity) {
    if (!clock) return;
    if (initial_capacity > SWIM_MAX_CLUSTER_NODES) {
        initial_capacity = SWIM_MAX_CLUSTER_NODES;
    }
    clock->capacity = initial_capacity;
    clock->count = 0;
    clock->lamport_time = 0;
    memset(clock->entries, 0, sizeof(clock->entries));
}

vc_clock_t* vc_create(uint32_t initial_capacity) {
    vc_clock_t *clock = (vc_clock_t *)malloc(sizeof(vc_clock_t));
    if (!clock) return NULL;
    vc_init(clock, initial_capacity);
    return clock;
}

void vc_free(vc_clock_t *clock) {
    if (clock) {
        free(clock);
    }
}

vc_clock_t* vc_clone(const vc_clock_t *clock) {
    if (!clock) return NULL;
    vc_clock_t *clone = (vc_clock_t *)malloc(sizeof(vc_clock_t));
    if (!clone) return NULL;
    memcpy(clone, clock, sizeof(vc_clock_t));
    return clone;
}

static int find_entry_index(const vc_clock_t *clock, const node_id_t *node_id) {
    for (uint32_t i = 0; i < clock->count; i++) {
        if (node_id_compare(&clock->entries[i].node_id, node_id) == 0) {
            return (int)i;
        }
    }
    return -1;
}

uint64_t vc_get(const vc_clock_t *clock, const node_id_t *node_id) {
    if (!clock || !node_id) return 0;
    int idx = find_entry_index(clock, node_id);
    if (idx >= 0) {
        return clock->entries[idx].counter;
    }
    return 0;
}

bool vc_increment(vc_clock_t *clock, const node_id_t *node_id) {
    if (!clock || !node_id) return false;
    
    clock->lamport_time++;
    
    int idx = find_entry_index(clock, node_id);
    if (idx >= 0) {
        clock->entries[idx].counter++;
        return true;
    }
    
    if (clock->count >= clock->capacity || clock->count >= SWIM_MAX_CLUSTER_NODES) {
        return false;
    }
    
    clock->entries[clock->count].node_id = *node_id;
    clock->entries[clock->count].counter = 1;
    clock->count++;
    
    return true;
}

bool vc_set(vc_clock_t *clock, const node_id_t *node_id, uint64_t counter) {
    if (!clock || !node_id) return false;
    
    int idx = find_entry_index(clock, node_id);
    if (idx >= 0) {
        clock->entries[idx].counter = counter;
        if (counter > clock->lamport_time) {
            clock->lamport_time = counter;
        }
        return true;
    }
    
    if (clock->count >= clock->capacity || clock->count >= SWIM_MAX_CLUSTER_NODES) {
        return false;
    }
    
    clock->entries[clock->count].node_id = *node_id;
    clock->entries[clock->count].counter = counter;
    clock->count++;
    
    if (counter > clock->lamport_time) {
        clock->lamport_time = counter;
    }
    
    return true;
}

vc_cmp_result_t vc_compare(const vc_clock_t *a, const vc_clock_t *b) {
    if (!a || !b) return VC_CMP_CONCURRENT;

    bool a_greater_or_equal = true;
    bool b_greater_or_equal = true;
    bool strictly_greater = false;
    bool strictly_less = false;

    // Collect all unique node IDs present in both clocks
    node_id_t all_nodes[SWIM_MAX_CLUSTER_NODES * 2];
    uint32_t total_nodes = 0;

    for (uint32_t i = 0; i < a->count; i++) {
        bool found = false;
        for (uint32_t k = 0; k < total_nodes; k++) {
            if (node_id_compare(&all_nodes[k], &a->entries[i].node_id) == 0) {
                found = true;
                break;
            }
        }
        if (!found && total_nodes < SWIM_MAX_CLUSTER_NODES * 2) {
            all_nodes[total_nodes++] = a->entries[i].node_id;
        }
    }

    for (uint32_t i = 0; i < b->count; i++) {
        bool found = false;
        for (uint32_t k = 0; k < total_nodes; k++) {
            if (node_id_compare(&all_nodes[k], &b->entries[i].node_id) == 0) {
                found = true;
                break;
            }
        }
        if (!found && total_nodes < SWIM_MAX_CLUSTER_NODES * 2) {
            all_nodes[total_nodes++] = b->entries[i].node_id;
        }
    }

    for (uint32_t i = 0; i < total_nodes; i++) {
        const node_id_t *nid = &all_nodes[i];
        uint64_t val_a = vc_get(a, nid);
        uint64_t val_b = vc_get(b, nid);

        if (val_a > val_b) {
            strictly_greater = true;
            b_greater_or_equal = false;
        } else if (val_a < val_b) {
            strictly_less = true;
            a_greater_or_equal = false;
        }
    }

    if (a_greater_or_equal && b_greater_or_equal) {
        return VC_CMP_EQUAL;
    }
    if (a_greater_or_equal && strictly_greater) {
        return VC_CMP_DOMINATES;
    }
    if (b_greater_or_equal && strictly_less) {
        return VC_CMP_DOMINATED_BY;
    }

    return VC_CMP_CONCURRENT;
}

void vc_merge(vc_clock_t *dest, const vc_clock_t *src) {
    if (!dest || !src) return;

    if (src->lamport_time > dest->lamport_time) {
        dest->lamport_time = src->lamport_time;
    }

    for (uint32_t i = 0; i < src->count; i++) {
        const node_id_t *nid = &src->entries[i].node_id;
        uint64_t src_val = src->entries[i].counter;
        uint64_t dest_val = vc_get(dest, nid);

        if (src_val > dest_val) {
            vc_set(dest, nid, src_val);
        }
    }
}

bool vc_resolve_conflict(const vc_clock_t *a, const node_id_t *id_a,
                         const vc_clock_t *b, const node_id_t *id_b,
                         const node_id_t **winning_id) {
    if (!a || !b || !id_a || !id_b || !winning_id) return false;

    vc_cmp_result_t cmp = vc_compare(a, b);

    if (cmp == VC_CMP_DOMINATES) {
        *winning_id = id_a;
        return true;
    }
    if (cmp == VC_CMP_DOMINATED_BY) {
        *winning_id = id_b;
        return true;
    }
    if (cmp == VC_CMP_EQUAL) {
        int tie = node_id_compare(id_a, id_b);
        if (tie <= 0) {
            *winning_id = id_a;
        } else {
            *winning_id = id_b;
        }
        return true;
    }

    // Concurrent: apply deterministic tie-breaking (lowest node_id_t wins)
    if (a->lamport_time > b->lamport_time) {
        *winning_id = id_a;
    } else if (b->lamport_time > a->lamport_time) {
        *winning_id = id_b;
    } else {
        int tie = node_id_compare(id_a, id_b);
        if (tie <= 0) {
            *winning_id = id_a;
        } else {
            *winning_id = id_b;
        }
    }

    return true;
}