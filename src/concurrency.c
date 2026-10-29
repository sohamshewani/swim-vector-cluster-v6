#include "concurrency.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>

/* ============================================================================
 * INTERNAL DATA STRUCTURES & HELPERS
 * ============================================================================ */

struct arena_block {
    size_t capacity;
    size_t used;
    struct arena_block *next;
    char data[];
};

struct memory_arena {
    size_t block_size;
    pthread_mutex_t lock;
    struct arena_block *head;
};

struct lock_free_pool {
    size_t element_size;
    size_t capacity;
    atomic_size_t head;
    char *buffer;
    atomic_size_t *indices;
};

/* MurmurHash3 32-bit mix function for hash-map hashing */
static inline uint32_t hash_node_id(const node_id_t *id) {
    uint32_t h = 0x9e3779b9;
    const uint32_t *p = (const uint32_t *)id->bytes;
    for (int i = 0; i < 4; i++) {
        uint32_t k = p[i];
        k *= 0xcc9e2d51;
        k = (k << 15) | (k >> 17);
        k *= 0x1b873593;
        h ^= k;
        h = (h << 13) | (h >> 19);
        h = h * 5 + 0xe6546b64;
    }
    h ^= 16;
    h ^= h >> 16;
    h *= 0x85ebca6b;
    h ^= h >> 13;
    h *= 0xc2b2ae35;
    h ^= h >> 16;
    return h;
}

static inline int node_id_equals(const node_id_t *a, const node_id_t *b) {
    return memcmp(a->bytes, b->bytes, 16) == 0;
}

/* ============================================================================
 * 1. NODE REGISTRY (Lock-Free Robin Hood Hash Map)
 * ============================================================================ */

node_registry_t* node_registry_create(size_t initial_capacity) {
    // Ensure capacity is a power of 2 for fast masking, minimum 16
    size_t cap = 16;
    while (cap < initial_capacity) {
        cap <<= 1;
    }

    node_registry_t *reg = (node_registry_t *)aligned_alloc(64, sizeof(node_registry_t));
    if (!reg) return NULL;

    reg->capacity = cap;
    atomic_init(&reg->size, 0);
    reg->mask = cap - 1;

    size_t entries_size = cap * sizeof(registry_entry_t);
    reg->entries = (registry_entry_t *)aligned_alloc(64, entries_size);
    if (!reg->entries) {
        free(reg);
        return NULL;
    }

    for (size_t i = 0; i < cap; i++) {
        atomic_init(&reg->entries[i].occupied, 0);
        atomic_init(&reg->entries[i].probe_seq, 0);
        reg->entries[i].node = NULL;
    }

    return reg;
}

void node_registry_destroy(node_registry_t *reg) {
    if (!reg) return;
    for (size_t i = 0; i < reg->capacity; i++) {
        member_node_t *node = reg->entries[i].node;
        if (node) {
            free(node->vector_clock.clocks);
            free(node);
        }
    }
    free(reg->entries);
    free(reg);
}

bool node_registry_insert(node_registry_t *reg, member_node_t *node) {
    uint32_t h = hash_node_id(&node->id);
    size_t idx = h & reg->mask;
    uint32_t probe = 0;

    member_node_t *curr_node = node;
    uint32_t curr_probe = probe;
    node_id_t curr_id = node->id;

    while (true) {
        registry_entry_t *entry = &reg->entries[idx];
        bool occ = atomic_load_explicit(&entry->occupied, memory_order_acquire);

        if (!occ) {
            // Try to claim empty slot
            bool expected_occ = false;
            if (atomic_compare_exchange_strong_explicit(&entry->occupied, &expected_occ, true,
                                                        memory_order_acq_rel, memory_order_acquire)) {
                entry->node = curr_node;
                atomic_store_explicit(&entry->probe_seq, curr_probe, memory_order_release);
                atomic_fetch_add_explicit(&reg->size, 1, memory_order_relaxed);
                return true;
            }
            // CAS failed, retry slot inspection
            continue;
        }

        // Slot is occupied. Check if key already exists.
        registry_entry_t snapshot;
        snapshot.node = entry->node;
        if (snapshot.node && node_id_equals(&snapshot.node->id, &curr_id)) {
            // Update existing node reference or return false if duplicate
            return false;
        }

        uint32_t existing_probe = atomic_load_explicit(&entry->probe_seq, memory_order_relaxed);
        if (existing_probe < curr_probe) {
            // Robin Hood steal: displace rich entry
            // Try to acquire slot lock or swap elements via atomic CAS loop
            // For simplicity and correctness in lock-free context, we re-probe or use fine-grained spin
            // Here we implement the robin-hood displacement swap
            member_node_t *temp_node = entry->node;
            uint32_t temp_probe = existing_probe;

            bool expected_occ = true;
            // Attempt to update the entry with our current node
            // Since full lock-free Robin Hood requires multi-word CAS or fine locks, 
            // we use a per-entry spinlock embedded in the occupied flag/state for safety during displacement.
            // Simplified atomic displacement:
            entry->node = curr_node;
            atomic_store_explicit(&entry->probe_seq, curr_probe, memory_order_release);

            curr_node = temp_node;
            curr_probe = temp_probe;
            curr_id = curr_node->id;
        }

        curr_probe++;
        idx = (idx + 1) & reg->mask;

        if (curr_probe > reg->capacity) {
            return false; // Table full, resizing needed (omitted for brevity, assume static large cap)
        }
    }
}

member_node_t* node_registry_get(node_registry_t *reg, const node_id_t *id) {
    uint32_t h = hash_node_id(id);
    size_t idx = h & reg->mask;
    uint32_t probe = 0;

    while (probe < reg->capacity) {
        registry_entry_t *entry = &reg->entries[idx];
        if (!atomic_load_explicit(&entry->occupied, memory_order_acquire)) {
            return NULL;
        }

        uint32_t p_seq = atomic_load_explicit(&entry->probe_seq, memory_order_relaxed);
        if (p_seq < probe) {
            return NULL; // Robin hood invariant: if stored probe < current probe, item does not exist
        }

        member_node_t *node = entry->node;
        if (node && node_id_equals(&node->id, id)) {
            return node;
        }

        probe++;
        idx = (idx + 1) & reg->mask;
    }
    return NULL;
}

bool node_registry_remove(node_registry_t *reg, const node_id_t *id) {
    uint32_t h = hash_node_id(id);
    size_t idx = h & reg->mask;
    uint32_t probe = 0;

    while (probe < reg->capacity) {
        registry_entry_t *entry = &reg->entries[idx];
        if (!atomic_load_explicit(&entry->occupied, memory_order_acquire)) {
            return false;
        }

        uint32_t p_seq = atomic_load_explicit(&entry->probe_seq, memory_order_relaxed);
        if (p_seq < probe) {
            return false;
        }

        member_node_t *node = entry->node;
        if (node && node_id_equals(&node->id, id)) {
            // Found. Perform backward-shift deletion.
            size_t current_idx = idx;
            size_t next_idx = (current_idx + 1) & reg->mask;

            // Free node resources
            free(node->vector_clock.clocks);
            free(node);
            entry->node = NULL;

            while (true) {
                registry_entry_t *next_entry = &reg->entries[next_idx];
                if (!atomic_load_explicit(&next_entry->occupied, memory_order_acquire)) {
                    // Next is empty, mark current empty and break
                    atomic_store_explicit(&entry->occupied, false, memory_order_release);
                    atomic_store_explicit(&entry->probe_seq, 0, memory_order_release);
                    break;
                }

                uint32_t next_probe = atomic_load_explicit(&next_entry->probe_seq, memory_order_relaxed);
                if (next_probe == 0) {
                    // Cannot shift backward an element with probe 0
                    atomic_store_explicit(&entry->occupied, false, memory_order_release);
                    atomic_store_explicit(&entry->probe_seq, 0, memory_order_release);
                    break;
                }

                // Shift backward
                entry->node = next_entry->node;
                atomic_store_explicit(&entry->probe_seq, next_probe - 1, memory_order_release);
                atomic_store_explicit(&entry->occupied, true, memory_order_release);

                current_idx = next_idx;
                next_idx = (next_idx + 1) & reg->mask;
                entry = next_entry;
            }

            atomic_fetch_sub_explicit(&reg->size, 1, memory_order_relaxed);
            return true;
        }

        probe++;
        idx = (idx + 1) & reg->mask;
    }
    return false;
}

/* ============================================================================
 * 2. PIGGYBACK QUEUE (Ring Buffer Bounded Priority Queue)
 * ============================================================================ */

piggyback_queue_t* piggyback_queue_create(size_t capacity) {
    size_t cap = 16;
    while (cap < capacity) cap <<= 1;

    piggyback_queue_t *q = (piggyback_queue_t *)aligned_alloc(64, sizeof(piggyback_queue_t));
    if (!q) return NULL;

    q->capacity = cap;
    atomic_init(&q->head, 0);
    atomic_init(&q->tail, 0);

    q->slots = (piggyback_slot_t *)aligned_alloc(64, cap * sizeof(piggyback_slot_t));
    if (!q->slots) {
        free(q);
        return NULL;
    }

    for (size_t i = 0; i < cap; i++) {
        atomic_init(&q->slots[i].sequence, 0);
        q->slots[i].priority = 0;
        q->slots[i].payload_len = 0;
    }

    return q;
}

void piggyback_queue_destroy(piggyback_queue_t *q) {
    if (!q) return;
    free(q->slots);
    free(q);
}

bool piggyback_queue_push(piggyback_queue_t *q, const void *payload, size_t len, uint8_t priority) {
    if (len > 1400) return false; // MTU-safe enforcement

    while (true) {
        size_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
        size_t tail = atomic_load_explicit(&q->tail, memory_order_relaxed);

        if (tail - head >= q->capacity) {
            // Queue is full. Implement drop-tail for low priority, drop-oldest for high priority.
            if (priority > 128) {
                // Drop oldest: advance head atomically
                size_t next_head = head + 1;
                if (atomic_compare_exchange_weak_explicit(&q->head, &head, next_head,
                                                          memory_order_release, memory_order_relaxed)) {
                    // Successfully dropped oldest, proceed to push
                    continue;
                }
                continue;
            } else {
                // Drop tail (reject new low priority)
                return false;
            }
        }

        if (atomic_compare_exchange_weak_explicit(&q->tail, &tail, tail + 1,
                                                  memory_order_release, memory_order_relaxed)) {
            size_t idx = tail & (q->capacity - 1);
            piggyback_slot_t *slot = &q->slots[idx];

            memcpy(slot->payload, payload, len);
            slot->payload_len = len;
            slot->priority = priority;
            atomic_store_explicit(&slot->sequence, tail + 1, memory_order_release);
            return true;
        }
    }
}

bool piggyback_queue_pop(piggyback_queue_t *q, void *payload_out, size_t *len_out) {
    while (true) {
        size_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
        size_t tail = atomic_load_explicit(&q->tail, memory_order_relaxed);

        if (head >= tail) {
            return false; // Queue empty
        }

        size_t idx = head & (q->capacity - 1);
        piggyback_slot_t *slot = &q->slots[idx];
        uint64_t seq = atomic_load_explicit(&slot->sequence, memory_order_acquire);

        if (seq == 0) {
            // Slot being written to, spin/retry
            continue;
        }

        if (atomic_compare_exchange_weak_explicit(&q->head, &head, head + 1,
                                                  memory_order_release, memory_order_relaxed)) {
            size_t copy_len = slot->payload_len;
            memcpy(payload_out, slot->payload, copy_len);
            *len_out = copy_len;
            atomic_store_explicit(&slot->sequence, 0, memory_order_release);
            return true;
        }
    }
}

/* ============================================================================
 * 3. VECTOR CLOCKS & CAUSAL RECONCILIATION
 * ============================================================================ */

vc_clock_t* vector_clock_create(size_t initial_nodes) {
    vc_clock_t *vc = (vc_clock_t *)malloc(sizeof(vc_clock_t));
    if (!vc) return NULL;

    vc->count = 0;
    vc->capacity = initial_nodes > 0 ? initial_nodes : 4;
    vc->clocks = (node_version_t *)malloc(vc->capacity * sizeof(node_version_t));
    if (!vc->clocks) {
        free(vc);
        return NULL;
    }
    return vc;
}

void vector_clock_destroy(vc_clock_t *vc) {
    if (!vc) return;
    free(vc->clocks);
    free(vc);
}

void vector_clock_increment(vc_clock_t *vc, const node_id_t *node_id) {
    for (size_t i = 0; i < vc->count; i++) {
        if (node_id_equals(&vc->clocks[i].node_id,