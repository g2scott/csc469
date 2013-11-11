/*
 * CSC469 - Parallel Memory Allocator
 *
 * Daniel Bloemendal <d.bloemendal@gmail.com>
 * Simon Scott <simontupperscott@gmail.com>
 */

#include <stdint.h>
#include <stdlib.h>
#include <pthread.h>
#include <assert.h>

#include "memlib.h"
#include "mm_thread.h"

//
// Hoard parameters
//
#define ALLOC_HOARD_FULLNESS_GROUPS 4
#define ALLOC_HOARD_SIZE_CLASS_BASE 2
#define ALLOC_HOARD_SIZE_CLASS_MIN  2
#define ALLOC_HOARD_HEAP_CPU_FACTOR 2

//
// Hoard structures
//

// Forward declarations
struct SUPERBLOCK_T;
struct HEAP_T;
struct CONTEXT_T;

// Types
typedef struct SUPERBLOCK_T superblock_t;
typedef struct HEAP_T heap_t;
typedef struct CONTEXT_T context_t;

// Invalid block
enum E_BLOCK { BLOCK_INVALID = -1 };

// Block pointer type
typedef int32_t blockptr_t;

// Superblock structure
struct SUPERBLOCK_T {
    heap_t*         heap;
    int             size_class;
    size_t          block_size;
    size_t          block_count;
    size_t          block_used;
    blockptr_t      next_block;
    blockptr_t      next_free;
    superblock_t*   prev;
    superblock_t*   next;
};

// Heap
struct HEAP_T {
    pthread_mutex_t lock;
    int             index;
    size_t          mem_used;
    size_t          mem_allocated;
    superblock_t*   bins[ALLOC_HOARD_FULLNESS_GROUPS];
};

// Allocator context
struct CONTEXT_T {
    void*           blocks_base;
    int             heap_count;
    heap_t          heap_table[1];
};

//
// Utility functions
//

inline size_t util_pagealigned(size_t size) {
    size_t padding = size % mem_pagesize();
    if(padding > 0) padding = mem_pagesize() - padding;
    return size + padding;
}

inline int util_sizeclass(size_t size) {
    // Compute size class
    size_t sizeunit = 1; size_t x = size;
    int sizecls = 0; while(x >= ALLOC_HOARD_SIZE_CLASS_BASE) {
        x        /= ALLOC_HOARD_SIZE_CLASS_BASE;
        sizeunit *= ALLOC_HOARD_SIZE_CLASS_BASE;
        sizecls  += 1;
    }

    // Check for remainder and return size class
    if(size % sizeunit) sizecls += 1;
    return (sizecls < ALLOC_HOARD_SIZE_CLASS_MIN) ? ALLOC_HOARD_SIZE_CLASS_MIN : sizecls;
}

//
// Superblock functions
//

inline size_t superblock_size(void) {
    return mem_pagesize();
}

static void superblock_init(superblock_t* sb, heap_t* heap, int size_class) {
    // Set size class
    sb->size_class = size_class;

    // Compute block size
    sb->block_size = 1;
    int i; for(i = 0; i < sb->size_class; i++)
        sb->block_size *= ALLOC_HOARD_SIZE_CLASS_BASE;

    // Set initials
    sb->block_used  = 0;
    sb->block_count = (superblock_size() - sizeof(superblock_t)) / sb->block_size;
    sb->next_block  = 0;
    sb->next_free   = BLOCK_INVALID;
    
    // Link entries
    sb->heap = heap;
    sb->next = NULL;
    sb->prev = NULL;
}

inline void* superblock_block_data(superblock_t* sb, blockptr_t blk) {
    assert(blk < sb->block_count);
    return (void*) ((char*) sb + sizeof(superblock_t) + sb->block_size * blk);
}

static void superblock_freelist_push(superblock_t* sb, blockptr_t blk) {
    // Embed a pointer to the previous block
    blockptr_t* prev_free = (blockptr_t*) superblock_block_data(sb, blk);
    *prev_free = sb->next_free;

    // Set our new next free block
    sb->next_free = blk;
}

static blockptr_t superblock_freelist_pop(superblock_t* sb) {
    // See if there are no free blocks
    if(sb->next_free == BLOCK_INVALID)
        return BLOCK_INVALID;

    // Pop the block off the stack
    blockptr_t blk = sb->next_free;
    sb->next_free = *(blockptr_t*) superblock_block_data(sb, blk);
    return blk;
}

static blockptr_t superblock_block_allocate(superblock_t* sb) {
    // See if we are filled to capacity
    if(sb->block_used >= sb->block_count)
        return BLOCK_INVALID;
    sb->block_used += 1;

    // Update heap statistics
    sb->heap->mem_used += sb->block_size;

    // Return a new or freed block
    blockptr_t blk = superblock_freelist_pop(sb);
    return (blk != BLOCK_INVALID) ? blk : sb->next_block++;
}

static void superblock_block_free(superblock_t* sb, blockptr_t blk) {
    // Add block to free list and adjust stats
    assert(sb->block_used > 0);
    superblock_freelist_push(sb, blk);
    sb->block_used -= 1;

    // Update heap statistics
    sb->heap->mem_used -= sb->block_size;
}

//
// Heap functions
//

static void heap_init(heap_t* heap) {
    pthread_mutex_init(&heap->lock, NULL);
    heap->mem_used = 0;
    heap->mem_allocated = 0;
    int i; for(i = 0; i < ALLOC_HOARD_FULLNESS_GROUPS; i++) {
        heap->index = i;
        heap->bins[i] = NULL;
    }
}

static int heap_full(heap_t* heap) {
    return heap->mem_used >= heap->mem_allocated;
}

//
// Context functions
//

inline static size_t context_size() {
    size_t size = sizeof(context_t) + sizeof(heap_t) * getNumProcessors() * ALLOC_HOARD_HEAP_CPU_FACTOR;
    return util_pagealigned(size);
}

static void context_init(context_t* ctx) {
    ctx->blocks_base = (char*) ctx + context_size();
    ctx->heap_count = getNumProcessors() * ALLOC_HOARD_HEAP_CPU_FACTOR;
    heap_init(&ctx->heap_table[0]);
    int i; for(i = 1; i < ctx->heap_count; i++)
        heap_init(&ctx->heap_table[i]);
}

static heap_t* context_globalheap(context_t* ctx) {
    return &ctx->heap_table[0];
}

static heap_t* context_heap(context_t* ctx, uint32_t threadid) {
    return &ctx->heap_table[1 + (threadid % ctx->heap_count)];
}

static void* context_malloc(context_t* ctx, heap_t* heap, size_t sz) {
    // Fetch global heap
    heap_t* glob = context_globalheap(ctx);

    // Compute size class
    int sizecls = util_sizeclass(sz);

    // Scan heap for appropriate superblock
    superblock_t* sb = NULL; int group;
    for(group = ALLOC_HOARD_FULLNESS_GROUPS; group >= 1; group--) {
        for(sb = heap->bins[group]; sb; sb = sb->next)
            if(sb->size_class == sizecls &&
               sb->block_used <  sb->block_count) break;
        // If block was found break out
        if(sb) break;
    }

    // If an appropriate superblock was found
    if(sb) {
        // Allocate a new block
        blockptr_t blk = superblock_block_allocate(sb);

        // See if we need to move the superblock to a new fullness group
        // ...

        // Return the address of the block in memory
        return superblock_block_data(sb, blk);
    }
    // If there are blocks available in the global heap
    else if(!heap_full(glob)) {
        // ...
    }
    // Otherwise allocate a new superblock
    else {
        // ...
    }
}


//
// Implementation
//

inline context_t* get_context(void) {
    return (context_t*) dseg_lo;
}

void *mm_malloc(size_t sz)
{
    // See if we are allocating from system
    if(sz > superblock_size()/2)
        return malloc(sz);

    // The result
    void* mem = NULL;

    // Get context
    context_t* ctx = get_context();

    // Find current heap and lock it & allocate memory
    heap_t* heap = context_heap(ctx, (uint32_t) pthread_self());
    pthread_mutex_lock(&heap->lock);
        mem = context_malloc(ctx, heap, sz);
    pthread_mutex_unlock(&heap->lock);
    return mem;
}

void mm_free(void *ptr)
{
    (void)ptr; /* Avoid warning about unused variable */
}


int mm_init(void)
{
    // See if we need to initialize the memory
    if(dseg_hi <= dseg_lo) mem_init();

    // Allocate memory for the context and initialize it
    context_t* ctx = mem_sbrk(context_size());
    if(ctx == NULL) return -1;
    context_init(ctx);
    return 0;
}
