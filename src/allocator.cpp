# include "allocator_internal.hpp"
#include <cstddef>
#include <cstdint>
# include <cstring>
# include <climits>

/**
 * void *malloc(size_t size)
 * -> Deals with multiple possible scenarios
 * -> The zero size case, smaller allocs by sbrk and large ones by mmap
 * -> mmap alloc is independent of the sbrk list
 */
extern "C" void* malloc(std::size_t size) {
    // 1. 0 case
    if (size == 0) return nullptr;

    // 2. Align otherwise
    size = align16(size);

    // 3. Walk the free list for a first-fit block
    BlockHeader *last = nullptr;
    BlockHeader *block = find_free_block(last, size);

    if (block) {
        // 4a. Reuse: split if the block is significantly oversized
        split_block(block, size);
        block->is_free = false;
        return reinterpret_cast<void *>(block + 1);
    }

    // 4b. No free block — decide which path to take
    if (size >= MMAP_THRESHHOLD) {
        block = request_mmap(size);
    } else {
        block = request_sbrk(last, size);
    }
    
    // 5. Both paths return nullptr on failure
    if (!block) 
        return nullptr;

    // 6. Return pointer past the header — caller never sees it
    return reinterpret_cast<void *>(block + 1);
};

extern "C" void free(void *ptr) {
    // 1. null guard
    if (!ptr)
        return;

    // 2. Walk back one header behind the user ptr
    BlockHeader *block = reinterpret_cast<BlockHeader*>(ptr) - 1;

    // 3. mmap blocks are handled completely differently
    if (block->size >= MMAP_THRESHHOLD) {
        munmap(block, HEADER_SIZE + block->size);
        return;
    }

    // 4. Mark the block as free
    block->is_free = true;

    // 5. Forward coalescing — merge with next block if it's also free
    if (block->next && block->next->is_free) {
        block->size += HEADER_SIZE + block->next->size;
        block->next = block->next->next;
    }
}

extern "C" void *calloc(std::size_t nmemb, std::size_t size) {
    // 1. Zero case — either argument being zero returns nullptr
    if (nmemb == 0 || size == 0)
        return nullptr;

    // 2. Overflow check — nmemb * size must not wrap around
    if (nmemb > SIZE_MAX / size) 
        return nullptr;

    // 3. Allocate the total bytes
    std::size_t total = nmemb * size;
    void *ptr = malloc(total);

    // 4. malloc failed
    if (!ptr)
        return nullptr;

    // 5. Zero the memory — this is the only thing calloc does differently
    memset(ptr, 0, total);

    return ptr;
}

