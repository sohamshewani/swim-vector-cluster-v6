#ifndef SWIM_ENGINE_H
#define SWIM_ENGINE_H

#define _GNU_SOURCE
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

/* ============================================================================
 * CONSTANTS & CONFIGURATION
 * ============================================================================ */

#define SWIM_UUID_BYTES         16
#define SWIM_MAX_PACKET_SIZE    1400
#define SWIM_DEFAULT_PORT       7946
#define SWIM_HASH_CAPACITY      4096
#define SWIM_RING_BUFFER_SIZE   1024
#define SWIM_MAX_VECTOR_NODES   256
#define SWIM_CACHE_LINE         64

/* ============================================================================
 * TYPE DEFINITIONS
 * ============================================================================ */

typedef uint8_t node_id_t[SWIM_UUID_BYTES];

typedef enum {
    NODE_STATE_ALIVE    = 0,
    NODE_STATE_SUSPECT  = 1,
    NODE_STATE_DEAD     = 2,
    NODE_STATE_LEFT     = 3
} __attribute__((packed)) node_state_t;

typedef enum {
    MSG_TYPE_PING       = 1,
    MSG_TYPE_ACK        = 2,
    MSG_TYPE_PING_REQ   = 3,
    MSG_TYPE_GOSSIP     = 4
} __attribute__((packed)) msg_type_t;

/* ============================================================================
 * VECTOR CLOCKS & CAUSAL METADATA
 * ============================================================================ */

typedef struct {
    node_id_t node_id;
    uint64_t counter;
} __attribute__((aligned(8))) vc_entry_t;

typedef struct {
    uint32_t count;
    uint32_t capacity;
    vc_entry_t entries[SWIM_MAX_VECTOR_NODES];
} __attribute__((aligned(SWIM_CACHE_LINE))) vc_clock_t;

/* ============================================================================
 * MEMBER NODE & REGISTRY STRUCTS
 * ============================================================================ */

typedef struct {
    node_id_t id;
    struct sockaddr_in endpoint;
    _Atomic node_state_t state;
    _Atomic uint64_t incarnation;
    uint64_t suspect_since_ms;
    vc_clock_t vector_clock;
    uint64_t last_updated_ms;
    uint32_t hash_probe_len;
} __attribute__((aligned(SWIM_CACHE_LINE))) member_node_t;

typedef struct {
    _Atomic uint32_t occupied;
    node_id_t key;
    member_node_t value;
} __attribute__((aligned(SWIM_CACHE_LINE))) hash_slot_t;

typedef struct {
    hash_slot_t slots[SWIM_HASH_CAPACITY];
    _Atomic uint64_t size;
} __attribute__((aligned(SWIM_CACHE_LINE))) node_registry_t;

/* ============================================================================
 * PIGGYBACK QUEUE & RING BUFFER
 * ============================================================================ */

typedef enum {
    PIGGY_PRIO_LOW      = 0,
    PIGGY_PRIO_HIGH     = 1
} piggy_prio_t;

typedef struct {
    uint32_t length;
    piggy_prio_t priority;
    uint8_t data[SWIM_MAX_PACKET_SIZE];
} __attribute__((aligned(SWIM_CACHE_LINE))) piggy_slot_t;

typedef struct {
    piggy_slot_t slots[SWIM_RING_BUFFER_SIZE];
    _Atomic uint64_t head;
    _Atomic uint64_t tail;
} __attribute__((aligned(SWIM_CACHE_LINE))) piggy_queue_t;

/* ============================================================================
 * NETWORK & PROTOCOL PACKETS
 * ============================================================================ */

typedef struct {
    uint8_t magic;
    uint8_t version;
    msg_type_t type;
    uint8_t reserved;
    uint32_t sequence;
    node_id_t sender_id;
    uint64_t epoch;
    uint32_t payload_len;
    uint8_t payload[SWIM_MAX_PACKET_SIZE - 40];
} __attribute__((packed, aligned(SWIM_CACHE_LINE))) swim_packet_t;

/* ============================================================================
 * ARENA ALLOCATOR
 * ============================================================================ */

typedef struct arena_block {
    struct arena_block *next;
    size_t capacity;
    size_t offset;
    uint8_t data[];
} arena_block_t;

typedef struct {
    arena_block_t *current;
    size_t block_size;
} arena_t;

/* ============================================================================
 * OBJECT POOL
 * ============================================================================ */

typedef struct pool_node {
    struct pool_node *next;
} pool_node_t;

typedef struct {
    uint8_t *storage;
    pool_node_t *free_list;
    size_t element_size;
    size_t capacity;
    _Atomic int lock;
} object_pool_t;

/* ============================================================================
 * SWIM ENGINE CONTROLLER
 * ============================================================================ */

typedef struct {
    node_id_t self_id;
    struct sockaddr_in self_endpoint;
    int sockfd;
    _Atomic bool running;
    
    node_registry_t registry;
    piggy_queue_t piggy_queue;
    
    int epoll_fd;
    pthread_t io_thread;
    pthread_t timer_thread;
    
    arena_t tick_arena;
    object_pool_t packet_pool;
    
    uint64_t current_epoch;
    uint32_t protocol_period_ms;
} __attribute__((aligned(SWIM_CACHE_LINE))) swim_engine_t;

/* ============================================================================
 * FUNCTION PROTOTYPES
 * ============================================================================ */

/* Engine Lifecycle */
bool swim_engine_init(swim_engine_t *engine, const node_id_t id, uint16_t port);
void swim_engine_start(swim_engine_t *engine);
void swim_engine_stop(swim_engine_t *engine);
void swim_engine_destroy(swim_engine_t *engine);

/* Node Registry (Lock-Free Robin Hood Hashing) */
bool swim_registry_insert(node_registry_t *registry, const member_node_t *node);
bool swim_registry_remove(node_registry_t *registry, const node_id_t node_id, member_node_t *out_node);
bool swim_registry_get(node_registry_t *registry, const node_id_t node_id, member_node_t *out_node);
bool swim_registry_update_state(node_registry_t *registry, const node_id_t node_id, node_state_t new_state, uint64_t incarnation);

/* Piggyback Ring Buffer */
bool swim_piggy_enqueue(piggy_queue_t *queue, const uint8_t *data, uint32_t len, piggy_prio_t prio);
bool swim_piggy_dequeue(piggy_queue_t *queue, uint8_t *data, uint32_t *len);

/* Vector Clocks */
void swim_vc_init(vc_clock_t *vc);
void swim_vc_increment(vc_clock_t *vc, const node_id_t node_id);
void swim_vc_merge(vc_clock_t *dest, const vc_clock_t *src);
int swim_vc_compare(const vc_clock_t *a, const vc_clock_t *b);

/* Memory Management */
void swim_arena_init(arena_t *arena, size_t block_size);
void *swim_arena_alloc(arena_t *arena, size_t size);
void swim_arena_reset(arena_t *arena);
void swim_arena_destroy(arena_t *arena);

bool swim_pool_init(object_pool_t *pool, size_t element_size, size_t capacity);
void *swim_pool_alloc(object_pool_t *pool);
void swim_pool_free(object_pool_t *pool, void *ptr);
void swim_pool_destroy(object_pool_t *pool);

/* Protocol & Network */
void *swim_io_loop(void *arg);
void *swim_timer_loop(void *arg);
bool swim_send_packet(int sockfd, const struct sockaddr_in *dest, const swim_packet_t *packet);

#endif /* SWIM_ENGINE_H */