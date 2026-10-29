#ifndef SWIM_TYPES_H
#define SWIM_TYPES_H

#define _GNU_SOURCE
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>
#include <time.h>
#include <sys/types.h>

#define SWIM_UUID_SIZE 16
#define SWIM_MAX_VECTOR_DIM 256
#define SWIM_MTU_PAYLOAD 1400
#define SWIM_CACHE_LINE 64
#define SWIM_MAX_HOSTNAME 256

typedef enum {
    SWIM_NODE_ALIVE = 0,
    SWIM_NODE_SUSPECT = 1,
    SWIM_NODE_DEAD = 2,
    SWIM_NODE_LEFT = 3,
    SWIM_NODE_JOINING = 4
} __attribute__((packed)) swim_node_state_t;

typedef enum {
    SWIM_MSG_PING = 1,
    SWIM_MSG_PING_REQ = 2,
    SWIM_MSG_ACK = 3,
    SWIM_MSG_GOSSIP = 4,
    SWIM_MSG_SYNC_REQ = 5,
    SWIM_MSG_SYNC_RESP = 6
} __attribute__((packed)) swim_msg_type_t;

typedef enum {
    SWIM_PRIO_LOW = 0,
    SWIM_PRIO_NORMAL = 1,
    SWIM_PRIO_HIGH = 2,
    SWIM_PRIO_CRITICAL = 3
} __attribute__((packed)) swim_priority_t;

typedef struct {
    uint8_t bytes[SWIM_UUID_SIZE];
} __attribute__((aligned(8))) node_id_t;

typedef struct {
    uint32_t count;
    uint32_t entries[SWIM_MAX_VECTOR_DIM];
    uint64_t lamport_time;
    uint64_t node_versions[SWIM_MAX_VECTOR_DIM];
} __attribute__((aligned(SWIM_CACHE_LINE))) vc_clock_t;

typedef struct {
    node_id_t node_id;
    char hostname[SWIM_MAX_HOSTNAME];
    uint16_t port;
    uint16_t vector_dim;
    float embedding[SWIM_MAX_VECTOR_DIM];
} __attribute__((aligned(SWIM_CACHE_LINE))) vector_payload_t;

typedef struct {
    node_id_t id;
    char hostname[SWIM_MAX_HOSTNAME];
    uint16_t port;
    _Atomic swim_node_state_t state;
    _Atomic uint64_t incarnation;
    uint64_t last_state_change_ns;
    double phi_score;
    vc_clock_t clock;
    vector_payload_t vector_data;
    uint32_t robin_hash;
    bool is_occupied;
    uint8_t pad[3];
} __attribute__((aligned(SWIM_CACHE_LINE))) member_node_t;

typedef struct {
    size_t capacity;
    _Atomic size_t head;
    _Atomic size_t tail;
    uint8_t *buffer;
    size_t slot_size;
} __attribute__((aligned(SWIM_CACHE_LINE))) ring_buffer_t;

typedef struct {
    uint8_t payload[SWIM_MTU_PAYLOAD];
    uint16_t length;
    swim_priority_t priority;
    uint64_t timestamp_ns;
    uint32_t epoch;
} __attribute__((aligned(SWIM_CACHE_LINE))) piggyback_slot_t;

typedef struct {
    piggyback_slot_t slots[256];
    _Atomic uint32_t head;
    _Atomic uint32_t tail;
    _Atomic uint32_t count;
} __attribute__((aligned(SWIM_CACHE_LINE))) piggyback_queue_t;

typedef struct {
    member_node_t *buckets;
    _Atomic size_t capacity;
    _Atomic size_t size;
    uint64_t seed;
} __attribute__((aligned(SWIM_CACHE_LINE))) node_registry_t;

typedef struct {
    uint8_t *arena_base;
    size_t arena_size;
    _Atomic size_t offset;
} __attribute__((aligned(SWIM_CACHE_LINE))) arena_allocator_t;

typedef struct {
    void *free_list;
    size_t block_size;
    size_t total_blocks;
    _Atomic size_t available_blocks;
    uint8_t *storage_memory;
} __attribute__((aligned(SWIM_CACHE_LINE))) object_pool_t;

static inline uint64_t swim_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static inline bool swim_node_id_equal(const node_id_t *a, const node_id_t *b) {
    const uint64_t *a64 = (const uint64_t *)a->bytes;
    const uint64_t *b64 = (const uint64_t *)b->bytes;
    return (a64[0] == b64[0]) && (a64[1] == b64[1]);
}

static inline uint32_t swim_hash_node_id(const node_id_t *id, uint64_t seed) {
    const uint64_t *data = (const uint64_t *)id->bytes;
    uint64_t h = seed ^ data[0];
    h ^= data[1] + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    return (uint32_t)(h ^ (h >> 32));
}

static inline void swim_vc_increment(vc_clock_t *clock, uint32_t node_index) {
    atomic_thread_fence(memory_order_seq_cst);
    clock->lamport_time++;
    if (node_index < SWIM_MAX_VECTOR_DIM) {
        clock->node_versions[node_index]++;
    }
}

static inline bool swim_vc_happens_before(const vc_clock_t *a, const vc_clock_t *b) {
    bool strictly_less = false;
    for (uint32_t i = 0; i < SWIM_MAX_VECTOR_DIM; i++) {
        if (a->node_versions[i] > b->node_versions[i]) {
            return false;
        }
        if (a->node_versions[i] < b->node_versions[i]) {
            strictly_less = true;
        }
    }
    return strictly_less || (a->lamport_time < b->lamport_time);
}

static inline int swim_node_id_compare(const node_id_t *a, const node_id_t *b) {
    for (int i = 0; i < SWIM_UUID_SIZE; i++) {
        if (a->bytes[i] < b->bytes[i]) return -1;
        if (a->bytes[i] > b->bytes[i]) return 1;
    }
    return 0;
}

#endif /* SWIM_TYPES_H */