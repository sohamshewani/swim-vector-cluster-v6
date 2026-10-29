#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <math.h>

#define WARMUP_ITERATIONS 100000
#define MEASURE_ITERATIONS 1000000
#define NUM_THREADS 1

// Mocking 'swim-vector-cluster' data structures and operations
typedef struct {
    uint64_t vector_id;
    float coordinates[128];
    uint32_t metadata;
} SwimVector;

typedef struct {
    SwimVector *vectors;
    size_t capacity;
    size_t size;
} SwimVectorCluster;

static SwimVectorCluster* swim_cluster_create(size_t capacity) {
    SwimVectorCluster *cluster = (SwimVectorCluster*)malloc(sizeof(SwimVectorCluster));
    cluster->capacity = capacity;
    cluster->size = 0;
    cluster->vectors = (SwimVector*)malloc(capacity * sizeof(SwimVector));
    return cluster;
}

static void swim_cluster_destroy(SwimVectorCluster *cluster) {
    if (cluster) {
        free(cluster->vectors);
        free(cluster);
    }
}

static inline int swim_cluster_insert(SwimVectorCluster *cluster, const SwimVector *vec) {
    if (cluster->size >= cluster->capacity) return -1;
    cluster->vectors[cluster->size++] = *vec;
    return 0;
}

static inline int swim_cluster_search(SwimVectorCluster *cluster, uint64_t vector_id, SwimVector *out_vec) {
    // Simulated vector similarity index lookup
    for (size_t i = 0; i < cluster->size; i++) {
        if (cluster->vectors[i].vector_id == vector_id) {
            *out_vec = cluster->vectors[i];
            return 0;
        }
    }
    return -1;
}

// Timing utilities
static inline uint64_t get_nanos(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static int compare_uint64(const void *a, const void *b) {
    uint64_t arg1 = *(const uint64_t*)a;
    uint64_t arg2 = *(const uint64_t*)b;
    if (arg1 < arg2) return -1;
    if (arg1 > arg2) return 1;
    return 0;
}

typedef struct {
    uint64_t *latencies;
    size_t count;
    double ops_sec;
} BenchmarkResult;

void* run_benchmark(void *arg) {
    (void)arg;
    SwimVectorCluster *cluster = swim_cluster_create(MEASURE_ITERATIONS + WARMUP_ITERATIONS);
    
    // Pre-populate a baseline vector
    SwimVector sample_vec;
    sample_vec.vector_id = 42;
    for(int i=0; i<128; i++) sample_vec.coordinates[i] = 0.1f * i;
    sample_vec.metadata = 0xDEADBEEF;
    swim_cluster_insert(cluster, &sample_vec);

    uint64_t *latencies = (uint64_t*)malloc(MEASURE_ITERATIONS * sizeof(uint64_t));
    
    // Warmup phase
    for (size_t i = 0; i < WARMUP_ITERATIONS; i++) {
        SwimVector dummy;
        swim_cluster_search(cluster, 42, &dummy);
    }

    // Measurement phase
    uint64_t start_global = get_nanos();
    
    for (size_t i = 0; i < MEASURE_ITERATIONS; i++) {
        uint64_t start_op = get_nanos();
        
        SwimVector res;
        volatile int found = swim_cluster_search(cluster, 42, &res);
        (void)found;
        
        uint64_t end_op = get_nanos();
        latencies[i] = end_op - start_op;
    }
    
    uint64_t end_global = get_nanos();
    uint64_t total_time_ns = end_global - start_global;
    
    double total_time_sec = (double)total_time_ns / 1000000000.0;
    double ops_sec = (double)MEASURE_ITERATIONS / total_time_sec;

    BenchmarkResult *res = (BenchmarkResult*)malloc(sizeof(BenchmarkResult));
    res->latencies = latencies;
    res->count = MEASURE_ITERATIONS;
    res->ops_sec = ops_sec;

    swim_cluster_destroy(cluster);
    return (void*)res;
}

int main(void) {
    pthread_t thread;
    BenchmarkResult *result;

    pthread_create(&thread, NULL, run_benchmark, NULL);
    pthread_join(thread, (void**)&result);

    qsort(result->latencies, result->count, sizeof(uint64_t), compare_uint64);

    uint64_t p50 = result->latencies[(size_t)(result->count * 0.50)];
    uint64_t p90 = result->latencies[(size_t)(result->count * 0.90)];
    uint64_t p99 = result->latencies[(size_t)(result->count * 0.99)];
    uint64_t p999 = result->latencies[(size_t)(result->count * 0.999)];

    printf("\n");
    printf("+---------------------------------------------------------+\n");
    printf("|       swim-vector-cluster Microbenchmark Results        |\n");
    printf("+-------------------------+-------------------------------+\n");
    printf("| Metric                  | Value                         |\n");
    printf("+-------------------------+-------------------------------+\n");
    printf("| Total Operations        | %-29zu |\n", result->count);
    printf("| Throughput              | %-22.2f ops/sec |\n", result->ops_sec);
    printf("| Latency P50             | %-22.2f ns    |\n", (double)p50);
    printf("| Latency P90             | %-22.2f ns    |\n", (double)p90);
    printf("| Latency P99             | %-22.2f ns    |\n", (double)p99);
    printf("| Latency P99.9           | %-22.2f ns    |\n", (double)p999);
    printf("+-------------------------+-------------------------------+\n");
    printf("\n");

    free(result->latencies);
    free(result);

    return 0;
}