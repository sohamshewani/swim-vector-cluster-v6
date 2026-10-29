#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdatomic.h>
#include <time.h>
#include <math.h>
#include <assert.h>

#define NODE_ID_LEN 16
#define MAX_CLUSTER_NODES 1024
#define HISTORY_WINDOW_SIZE 100
#define PHI_THRESHOLD 8.0

typedef struct {
    uint8_t bytes[NODE_ID_LEN];
} node_id_t;

typedef enum {
    NODE_ALIVE = 0,
    NODE_SUSPECT = 1,
    NODE_DEAD = 2,
    NODE_LEFT = 3
} node_state_t;

typedef struct {
    uint64_t timestamps[HISTORY_WINDOW_SIZE];
    size_t head;
    size_t tail;
    size_t count;
    uint64_t last_heartbeat;
} heartbeat_history_t;

typedef struct {
    node_id_t id;
    atomic_int state;
    atomic_uint_fast64_t incarnation;
    uint64_t suspicion_start_time;
    heartbeat_history_t history;
} member_node_t;

typedef struct {
    node_id_t node_id;
    uint64_t expire_time;
    uint64_t incarnation;
} suspicion_timer_entry_t;

typedef struct {
    suspicion_timer_entry_t entries[MAX_CLUSTER_NODES];
    size_t size;
} suspicion_timer_wheel_t;

static uint64_t current_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static int node_id_cmp(const node_id_t *a, const node_id_t *b) {
    return memcmp(a->bytes, b->bytes, NODE_ID_LEN);
}

void heartbeat_history_init(heartbeat_history_t *h) {
    memset(h, 0, sizeof(heartbeat_history_t));
    h->last_heartbeat = current_time_ms();
}

void heartbeat_history_add(heartbeat_history_t *h, uint64_t now) {
    uint64_t interval = now - h->last_heartbeat;
    h->last_heartbeat = now;

    h->timestamps[h->tail] = interval;
    h->tail = (h->tail + 1) % HISTORY_WINDOW_SIZE;
    if (h->count < HISTORY_WINDOW_SIZE) {
        h->count++;
    } else {
        h->head = (h->head + 1) % HISTORY_WINDOW_SIZE;
    }
}

double calculate_phi(heartbeat_history_t *h, uint64_t now) {
    if (h->count < 2) {
        return 0.0;
    }

    uint64_t time_since_last = now - h->last_heartbeat;
    
    uint64_t sum = 0;
    for (size_t i = 0; i < h->count; i++) {
        size_t idx = (h->head + i) % HISTORY_WINDOW_SIZE;
        sum += h->timestamps[idx];
    }
    
    double mean = (double)sum / (double)h->count;

    double variance_sum = 0;
    for (size_t i = 0; i < h->count; i++) {
        size_t idx = (h->head + i) % HISTORY_WINDOW_SIZE;
        double diff = (double)h->timestamps[idx] - mean;
        variance_sum += diff * diff;
    }
    double variance = variance_sum / (double)h->count;
    if (variance < 1.0) {
        variance = 1.0; 
    }

    double stddev = sqrt(variance);
    double)}^{\prime} = (double)time_since_last - mean;
    double y = (double)time_since_last / stddev;
    
    const double inv_sqrt_2 = 0.70710678118654752440;
    double cdf = 0.5 * (1.0 + erf(y * inv_sqrt_2));
    
    if (cdf >= 1.0) {
        return 16.0;
    }
    if (cdf <= 0.0) {
        return 0.0;
    }

    double phi = -log10(1.0 - cdf);
    return phi;
}

void suspicion_timer_wheel_init(suspicion_timer_wheel_t *wheel) {
    wheel->size = 0;
    memset(wheel->entries, 0, sizeof(wheel->entries));
}

bool suspicion_timer_wheel_add(suspicion_timer_wheel_t *wheel, node_id_t id, uint64_t expire_time, uint64_t incarnation) {
    if (wheel->size >= MAX_CLUSTER_NODES) {
        return false;
    }
    for (size_t i = 0; i < wheel->size; i++) {
        if (node_id_cmp(&wheel->entries[i].node_id, &id) == 0) {
            wheel->entries[i].expire_time = expire_time;
            wheel->entries[i].incarnation = incarnation;
            return true;
        }
    }
    wheel->entries[wheel->size] = (suspicion_timer_entry_t){
        .node_id = id,
        .expire_time = expire_time,
        .incarnation = incarnation
    };
    wheel->size++;
    return true;
}

void suspicion_timer_wheel_remove(suspicion_timer_wheel_t *wheel, const node_id_t *id) {
    for (size_t i = 0; i < wheel->size; i++) {
        if (node_id_cmp(&wheel->entries[i].node_id, id) == 0) {
            wheel->entries[i] = wheel->entries[wheel->size - 1];
            wheel->size--;
            break;
        }
    }
}

void suspicion_handle_ping_timeout(member_node_t *node, suspicion_timer_wheel_t *wheel, uint64_t timeout_duration_ms) {
    int current_state = atomic_load_explicit(&node->state, memory_order_acquire);
    if (current_state == NODE_ALIVE) {
        int expected = NODE_ALIVE;
        if (atomic_compare_exchange_strong_explicit(&node->state, &expected, NODE_SUSPECT,
                                                    memory_order_acq_rel, memory_order_acquire)) {
            node->suspicion_start_time = current_time_ms();
            uint64_t expire_time = node->suspicion_start_time + timeout_duration_ms;
            uint64_t inc = atomic_load_explicit(&node->incarnation, memory_order_relaxed);
            suspicion_timer_wheel_add(wheel, node->id, expire_time, inc);
        }
    }
}

void suspicion_process_ack(member_node_t *node, suspicion_timer_wheel_t *wheel, uint64_t remote_incarnation) {
    uint64_t local_inc = atomic_load_explicit(&node->incarnation, memory_order_acquire);
    
    if (remote_incarnation > local_inc) {
        atomic_store_explicit(&node->incarnation, remote_incarnation, memory_order_release);
        int current_state = atomic_load_explicit(&node->state, memory_order_acquire);
        if (current_state == NODE_SUSPECT || current_state == NODE_DEAD) {
            atomic_store_explicit(&node->state, NODE_ALIVE, memory_order_release);
            suspicion_timer_wheel_remove(wheel, &node->id);
        }
    }

    int current_state = atomic_load_explicit(&node->state, memory_order_acquire);
    if (current_state == NODE_SUSPECT && remote_incarnation == local_inc) {
        double phi = calculate_phi(&node->history, current_time_ms());
        if (phi < PHI_THRESHOLD) {
            int expected = NODE_SUSPECT;
            if (atomic_compare_exchange_strong_explicit(&node->state, &expected, NODE_ALIVE,
                                                        memory_order_acq_rel, memory_order_acquire)) {
                suspicion_timer_wheel_remove(wheel, &node->id);
            }
        }
    }

    heartbeat_history_add(&node->history, current_time_ms());
}

void suspicion_tick(member_node_t *nodes, size_t num_nodes, suspicion_timer_wheel_t *wheel, uint64_t timeout_duration_ms) {
    uint64_t now = current_time_ms();

    for (size_t i = 0; i < wheel->size; ) {
        if (now >= wheel->entries[i].expire_time) {
            node_id_t id = wheel->entries[i].node_id;
            uint64_t target_inc = wheel->entries[i].incarnation;

            wheel->entries[i] = wheel->entries[wheel->size - 1];
            wheel->size--;

            for (size_t j = 0; j < num_nodes; j++) {
                if (node_id_cmp(&nodes[j].id, &id) == 0) {
                    uint64_t node_inc = atomic_load_explicit(&nodes[j].incarnation, memory_order_acquire);
                    if (node_inc == target_inc) {
                        int expected = NODE_SUSPECT;
                        atomic_compare_exchange_strong_explicit(&nodes[j].state, &expected, NODE_DEAD,
                                                                memory_order_acq_rel, memory_order_acquire);
                    }
                    break;
                }
            }
        } else {
            i++;
        }
    }

    for (size_t i = 0; i < num_nodes; i++) {
        int state = atomic_load_explicit(&nodes[i].state, memory_order_acquire);
        if (state == NODE_SUSPECT) {
            double phi = calculate_phi(&nodes[i].history, now);
            if (phi >= PHI_THRESHOLD * 1.5) {
                int expected = NODE_SUSPECT;
                if (atomic_compare_exchange_strong_explicit(&nodes[i].state, &expected, NODE_DEAD,
                                                            memory_order_acq_rel, memory_order_acquire)) {
                    suspicion_timer_wheel_remove(wheel, &nodes[i].id);
                }
            }
        }
    }
}