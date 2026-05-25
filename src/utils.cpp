# include "allocator_internal.hpp"
#include <sys/mman.h>

BlockHeader* heap_start = nullptr;

/**
 * size_t align (size_t size)
 *   -> Ensures that we are allocating a blocks of multiples of 16
 */

std::size_t align16(std::size_t size) {
    return (size + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
}

/**
 * BlockHeader *find_free_block(BlockHeader *&last, std::size_t size)
 *  -> Finds the next free block that can be used
 *  -> Notice for perfomance concerns we are using the first fit and not best fit
 *  -> Ensuring O(n) time complexity in the worst case
 */

BlockHeader *find_free_block(BlockHeader *&last, std::size_t size) {
    BlockHeader *current = heap_start;

    while (current) {
        if (current->is_free && current->size >= size) {
            return current;
        }

        last = current;
        current = current->next;
    }

    return nullptr;
}

/**
 * void split_block(BlockHeader *block, std::size_t size)
 * -> leftover block is only useful if it can hold a header plus at least ALIGNMENT bytes of payload
 * -> If the remainder would be smaller than that, we don't split — we just hand over the whole block.
 */

void split_block(BlockHeader *block, std::size_t size) {
    if (block->size < size + MIN_SPLIT_SIZE) return;

    auto *remainder = reinterpret_cast<BlockHeader*> (reinterpret_cast<char *>(block + 1) + size);

    remainder->size = block->size - size - HEADER_SIZE;
    remainder->is_free = true;
    remainder->next = block->next;

    block->size = size;
    block->next = remainder;
}

/**
 * BlockHeader *request_sbrk(BlockHeader *last, size_t size)
 * -> Extends the heap using sbrk and carves a new block out of the new memory.
 * ->
 */

BlockHeader *request_sbrk(BlockHeader *last, std::size_t size) {
    auto *block = reinterpret_cast<BlockHeader *>(sbrk(0));

    if (sbrk(static_cast<intptr_t>(HEADER_SIZE + size)) == reinterpret_cast<void *>(-1))
        return nullptr;

    block->size = size;
    block->is_free = false;
    block->next = nullptr;

    if (last) {
        last->next = block;
    } else {
        heap_start = block;
    }

    return block;
}

/**
 * BlockHeader *request_mmap(std::size_t size)
 * -> Allocates large blocks (≥ MMAP_THRESHOLD) directly from the OS via mmap
 * -> Key to note mmap blocks are not part of the sbrk block list
 */
BlockHeader *request_mmap(std::size_t size) {
    std::size_t total = HEADER_SIZE + size;

    void *ptr = mmap(
        nullptr, total,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS,
        -1, 0
    );

    if (ptr == MAP_FAILED) {
        return nullptr;
    }

    auto *block = reinterpret_cast<BlockHeader *>(ptr);
    block->size = size;
    block->is_free = false;
    block->next = nullptr;

    return block;
}