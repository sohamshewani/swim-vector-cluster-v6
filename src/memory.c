#include "memory.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

#define CACHE_LINE_SIZE 64

typedef struct arena_block {
    size_t capacity;
    size_t used;
    struct arena_block* next;
    alignas(64) char data[];
} arena_block_t;

struct arena {
    size_t block_size;
    arena_block_t* current;
    arena_block_t* head;
};

struct object_pool {
    size_t element_size;
    size_t capacity;
    atomic_size_t head_index;
    char* memory;
    void** free_stack;
    atomic_size_t stack_top;
};

arena_t* arena_create(size_t initial_block_size) {
    size_t bsize = initial_block_size > 4096 ? initial_block_size : 4096;
    arena_t* arena = (arena_t*)malloc(sizeof(arena_t));
    if (!arena) return NULL;

    arena_block_t* block = (arena_block_t*)malloc(sizeof(arena_block_t) + bsize);
    if (!block) {
        free(arena);
        return NULL;
    }

    block->capacity = bsize;
    block->used = 0;
    block->next = NULL;

    arena->block_size = bsize;
    arena->current = block;
    arena->head = block;

    return arena;
}

void* arena_alloc(arena_t* arena, size_t size) {
    if (!arena || size == 0) return NULL;

    // Align size to 8 bytes
    size_t aligned_size = (size + 7) & ~((size_t)7);

    arena_block_t* curr = arena->current;
    if (curr->used + aligned_size <= curr->capacity) {
        void* ptr = (void*)&curr->data[curr->used];
        curr->used += aligned_size;
        return ptr;
    }

    // Allocate a new block, at least double the size or large enough for the request
    size_t new_capacity = arena->block_size;
    if (aligned_size > new_capacity) {
        new_capacity = aligned_size;
    }

    arena_block_t* new_block = (arena_block_t*)malloc(sizeof(arena_block_t) + new_capacity);
    if (!new_block) return NULL;

    new_block->capacity = new_capacity;
    new_block->used = aligned_size;
    new_block->next = NULL;

    curr->next = new_block;
    arena->current = new_block;

    return (void*)&new_block->data[0];
}

void arena_reset(arena_t* arena) {
    if (!arena) return;
    arena_block_t* curr = arena->head;
    while (curr) {
        curr->used = 0;
        curr = curr->next;
    }
    arena->current = arena->head;
}

void arena_destroy(arena_t* arena) {
    if (!arena) return;
    arena_block_t* curr = arena->head;
    while (curr) {
        arena_block_t* next = curr->next;
        free(curr);
        curr = next;
    }
    free(arena);
}

object_pool_t* object_pool_create(size_t element_size, size_t capacity) {
    if (element_size == 0 || capacity == 0) return NULL;

    // Align element size to cache line or at least 8 bytes
    size_t aligned_elem_size = (element_size + 7) & ~((size_t)7);

    object_pool_t* pool = (object_pool_t*)malloc(sizeof(object_pool_t));
    if (!pool) return NULL;

    pool->memory = (char*)malloc(aligned_elem_size * capacity);
    if (!pool->memory) {
        free(pool);
        return NULL;
    }

    pool->free_stack = (void**)malloc(sizeof(void*) * capacity);
    if (!pool->free_stack) {
        free(pool->memory);
        free(pool);
        return NULL;
    }

    pool->element_size = aligned_elem_size;
    pool->capacity = capacity;
    atomic_init(&pool->stack_top, capacity);

    for (size_t i = 0; i < capacity; ++i) {
        pool->free_stack[i] = pool->memory + (i * aligned_elem_size);
    }

    return pool;
}

void* object_pool_alloc(object_pool_t* pool) {
    if (!pool) return NULL;

    size_t top = atomic_load_explicit(&pool->stack_top, memory_order_relaxed);
    while (top > 0) {
        if (atomic_compare_exchange_weak_explicit(&pool->stack_top, &top, top - 1,
                                                 memory_order_acquire, memory_order_relaxed)) {
            return pool->free_stack[top - 1];
        }
    }
    return NULL; // Pool exhausted
}

void object_pool_free(object_pool_t* pool, void* ptr) {
    if (!pool || !ptr) return;

    size_t top = atomic_load_explicit(&pool->stack_top, memory_order_relaxed);
    while (top < pool->capacity) {
        if (atomic_compare_exchange_weak_explicit(&pool->stack_top, &top, top + 1,
                                                 memory_order_release, memory_order_relaxed)) {
            pool->free_stack[top] = ptr;
            return;
        }
    }
    // Should never happen unless double free or corruption occurs
    assert(0 && "Object pool free stack overflow");
}

void object_pool_destroy(object_pool_t* pool) {
    if (!pool) return;
    free(pool->memory);
    free(pool->free_stack);
    free(pool);
}