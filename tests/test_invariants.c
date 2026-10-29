#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

/* ============================================================================
 * MOCKED SWIM VECTOR CLUSTER IMPLEMENTATION
 * ============================================================================ */

#define MAX_NODES 64
#define MAX_VECTOR_DIM 16

typedef enum {
    NODE_STATE_DEAD = 0,
    NODE_STATE_ALIVE,
    NODE_STATE_SUSPECT
} node_state_t;

typedef struct {
    uint32_t id;
    node_state_t state;
    uint64_t incarnation;
    float coords[MAX_VECTOR_DIM];
    size_t dim;
    time_t last_heartbeat;
} cluster_node_t;

typedef struct {
    cluster_node_t nodes[MAX_NODES];
    size_t capacity;
    size_t dim;
    pthread_rwlock_t lock;
    uint64_t global_incarnation;
} swim_cluster_t;

static swim_cluster_t* swim_cluster_create(size_t capacity, size_t dim) {
    if (capacity == 0 || capacity > MAX_NODES || dim == 0 || dim > MAX_VECTOR_DIM) {
        return NULL;
    }
    swim_cluster_t *cluster = (swim_cluster_t*)malloc(sizeof(swim_cluster_t));
    if (!cluster) return NULL;

    cluster->capacity = capacity;
    cluster->dim = dim;
    cluster->global_incarnation = 0;
    if (pthread_rwlock_init(&cluster->lock, NULL) != 0) {
        free(cluster);
        return NULL;
    }

    for (size_t i = 0; i < MAX_NODES; i++) {
        cluster->nodes[i].id = 0;
        cluster->nodes[i].state = NODE_STATE_DEAD;
        cluster->nodes[i].incarnation = 0;
        cluster->nodes[i].dim = 0;
        cluster->nodes[i].last_heartbeat = 0;
        memset(cluster->nodes[i].coords, 0, sizeof(cluster->nodes[i].coords));
    }
    return cluster;
}

static void swim_cluster_destroy(swim_cluster_t *cluster) {
    if (!cluster) return;
    pthread_rwlock_destroy(&cluster->lock);
    free(cluster);
}

static int swim_cluster_add_node(swim_cluster_t *cluster, uint32_t id, const float *coords) {
    if (!cluster || id == 0 || !coords) return EINVAL;

    pthread_rwlock_wrlock(&cluster->lock);

    /* Check existence & find slot */
    size_t free_slot = (size_t)-1;
    size_t active_count = 0;

    for (size_t i = 0; i < cluster->capacity; i++) {
        if (cluster->nodes[i].state != NODE_STATE_DEAD) {
            active_count++;
            if (cluster->nodes[i].id == id) {
                pthread_rwlock_unlock(&cluster->lock);
                return EEXIST;
            }
        } else if (free_slot == (size_t)-1) {
            free_slot = i;
        }
    }

    if (active_count >= cluster->capacity || free_slot == (size_t)-1) {
        pthread_rwlock_unlock(&cluster->lock);
        return ENOMEM;
    }

    cluster->nodes[free_slot].id = id;
    cluster->nodes[free_slot].state = NODE_STATE_ALIVE;
    cluster->nodes[free_slot].incarnation = ++cluster->global_incarnation;
    cluster->nodes[free_slot].dim = cluster->dim;
    cluster->nodes[free_slot].last_heartbeat = time(NULL);
    memcpy(cluster->nodes[free_slot].coords, coords, cluster->dim * sizeof(float));

    pthread_rwlock_unlock(&cluster->lock);
    return 0;
}

static int swim_cluster_remove_node(swim_cluster_t *cluster, uint32_t id) {
    if (!cluster || id == 0) return EINVAL;

    pthread_rwlock_wrlock(&cluster->lock);
    for (size_t i = 0; i < cluster->capacity; i++) {
        if (cluster->nodes[i].state != NODE_STATE_DEAD && cluster->nodes[i].id == id) {
            cluster->nodes[i].state = NODE_STATE_DEAD;
            cluster->nodes[i].id = 0;
            cluster->nodes[i].incarnation = 0;
            pthread_rwlock_unlock(&cluster->lock);
            return 0;
        }
    }
    pthread_rwlock_unlock(&cluster->lock);
    return ENOENT;
}

static int swim_cluster_update_state(swim_cluster_t *cluster, uint32_t id, node_state_t new_state, uint64_t incarnation) {
    if (!cluster || id == 0) return EINVAL;

    pthread_rwlock_wrlock(&cluster->lock);
    for (size_t i = 0; i < cluster->capacity; i++) {
        if (cluster->nodes[i].state != NODE_STATE_DEAD && cluster->nodes[i].id == id) {
            /* Vector-consensus style incarnation check */
            if (incarnation >= cluster->nodes[i].incarnation) {
                cluster->nodes[i].state = new_state;
                cluster->nodes[i].incarnation = incarnation;
                cluster->nodes[i].last_heartbeat = time(NULL);
                pthread_rwlock_unlock(&cluster->lock);
                return 0;
            } else {
                pthread_rwlock_unlock(&cluster->lock);
                return EACCES; /* Stale incarnation */
            }
        }
    }
    pthread_rwlock_unlock(&cluster->lock);
    return ENOENT;
}

static int swim_cluster_get_node(swim_cluster_t *cluster, uint32_t id, cluster_node_t *out_node) {
    if (!cluster || !out_node || id == 0) return EINVAL;

    pthread_rwlock_rdlock(&cluster->lock);
    for (size_t i = 0; i < cluster->capacity; i++) {
        if (cluster->nodes[i].state != NODE_STATE_DEAD && cluster->nodes[i].id == id) {
            memcpy(out_node, &cluster->nodes[i], sizeof(cluster_node_t));
            pthread_rwlock_unlock(&cluster->lock);
            return 0;
        }
    }
    pthread_rwlock_unlock(&cluster->lock);
    return ENOENT;
}

static int swim_cluster_find_nearest(swim_cluster_t *cluster, const float *query, uint32_t *out_id) {
    if (!cluster || !query || !out_id) return EINVAL;

    pthread_rwlock_rdlock(&cluster->lock);
    int found = 0;
    float min_dist = 1e30f;
    uint32_t best_id = 0;

    for (size_t i = 0; i < cluster->capacity; i++) {
        if (cluster->nodes[i].state == NODE_STATE_ALIVE) {
            float dist = 0.0f;
            for (size_t d = 0; d < cluster->dim; d++) {
                float diff = cluster->nodes[i].coords[d] - query[d];
                dist += diff * diff;
            }
            if (dist < min_dist) {
                min_dist = dist;
                best_id = cluster->nodes[i].id;
                found = 1;
            }
        }
    }
    pthread_rwlock_unlock(&cluster->lock);

    if (found) {
        *out_id = best_id;
        return 0;
    }
    return ENOENT;
}

/* ============================================================================
 * TEST SUITE
 * ============================================================================ */

#define TEST_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "\033[31m[FAIL]\033[0m %s:%d - Assertion Failed: %s\n", __FILE__, __LINE__, msg); \
        exit(EXIT_FAILURE); \
    } \
} while(0)

#define RUN_TEST(test_func) do { \
    printf("[RUNNING] %s...\n", #test_func); \
    test_func(); \
    printf("\033[32m[PASSED]\033[0m %s\n", #test_func); \
} while(0)

/* 1. Boundary and Edge-Case Tests */
static void test_boundaries_and_edges(void) {
    /* Invalid initializations */
    TEST_ASSERT(swim_cluster_create(0, 4) == NULL, "Zero capacity should fail");
    TEST_ASSERT(swim_cluster_create(MAX_NODES + 1, 4) == NULL, "Exceeding max capacity should fail");
    TEST_ASSERT(swim_cluster_create(10, 0) == NULL, "Zero dimension should fail");
    TEST_ASSERT(swim_cluster_create(10, MAX_VECTOR_DIM + 1) == NULL, "Exceeding max dim should fail");

    swim_cluster_t *cluster = swim_cluster_create(4, 2);
    TEST_ASSERT(cluster != NULL, "Valid cluster creation failed");

    float coords1[2] = {1.0f, 2.0f};
    float coords2[2] = {3.0f, 4.0f};
    float coords3[2] = {5.0f, 6.0f};
    float coords4[2] = {7.0f, 8.0f};
    float coords_extra[2] = {9.0f, 10.0f};

    /* Basic mutations */
    TEST_ASSERT(swim_cluster_add_node(cluster, 0, coords1) == EINVAL, "Adding node with ID 0 should fail");
    TEST_ASSERT(swim_cluster_add_node(cluster, 1, NULL) == EINVAL, "Adding node with NULL coords should fail");
    
    TEST_ASSERT(swim_cluster_add_node(cluster, 1, coords1) == 0, "Adding node 1 failed");
    TEST_ASSERT(swim_cluster_add_node(cluster, 1, coords1) == EEXIST, "Adding duplicate node 1 should fail");

    TEST_ASSERT(swim_cluster_add_node(cluster, 2, coords2) == 0, "Adding node 2 failed");
    TEST_ASSERT(swim_cluster_add_node(cluster, 3, coords3) == 0, "Adding node 3 failed");
    TEST_ASSERT(swim_cluster_add_node(cluster, 4, coords4) == 0, "Adding node 4 failed");

    /* Capacity boundary */
    TEST_ASSERT(swim_cluster_add_node(cluster, 5, coords_extra) == ENOMEM, "Adding beyond capacity should fail");

    /* Removal and Re-addition */
    TEST_ASSERT(swim_cluster_remove_node(cluster, 99) == ENOENT, "Removing non-existent node should fail");
    TEST_ASSERT(swim_cluster_remove_node(cluster, 2) == 0, "Removing node 2 failed");
    
    /* Slot reuse */
    TEST_ASSERT(swim_cluster_add_node(cluster, 5, coords_extra) == 0, "Slot reuse after removal failed");

    swim_cluster_destroy(cluster);
}

/* 2. Invariant and Failure Recovery Tests */
static void test_invariants_and_recovery(void) {
    swim_cluster_t *cluster = swim_cluster_create(8, 3);
    TEST_ASSERT(cluster != NULL, "Cluster creation failed");

    float c1[3] = {0.0f, 0.0f, 0.0f};
    float