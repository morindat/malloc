#include "allocator_internal.hpp"

extern "C" void *malloc(std::size_t size) {
    if (size == 0)
        return nullptr;

    size = align16(size);

    BlockHeader *last  = nullptr;
    BlockHeader *block = find_free_block(last, size);

    if (block) {
        split_block(block, size);
        block->is_free = false;
        write_footer(block);
        return reinterpret_cast<void *>(block + 1);
    }

    if (size >= MMAP_THRESHOLD)
        block = request_mmap(size);
    else
        block = request_sbrk(last, size);

    if (!block)
        return nullptr;

    return reinterpret_cast<void *>(block + 1);
}

extern "C" void free(void *ptr) {
    if (!ptr)
        return;

    BlockHeader *block = reinterpret_cast<BlockHeader *>(ptr) - 1;

    if (block->size >= MMAP_THRESHOLD) {
        munmap(block, BLOCK_OVERHEAD + block->size);
        return;
    }

    block->is_free = true;
    write_footer(block);

    // forward coalesce
    if (block->next && block->next->is_free) {
        block->size += BLOCK_OVERHEAD + block->next->size;
        block->next  = block->next->next;
        write_footer(block);
    }

    // backward coalesce
    BlockHeader *prev = get_prev(block);
    if (prev && prev->is_free) {
        prev->size += BLOCK_OVERHEAD + block->size;
        prev->next  = block->next;
        write_footer(prev);
    }
}

extern "C" void *calloc(std::size_t nmemb, std::size_t size) {
    if (nmemb == 0 || size == 0)
        return nullptr;

    if (nmemb > SIZE_MAX / size)
        return nullptr;

    std::size_t total = nmemb * size;
    void *ptr = malloc(total);

    if (!ptr)
        return nullptr;

    memset(ptr, 0, total);
    return ptr;
}

extern "C" void *realloc(void *ptr, std::size_t size) {
    if (!ptr)
        return malloc(size);

    if (size == 0) {
        free(ptr);
        return nullptr;
    }

    BlockHeader *block       = reinterpret_cast<BlockHeader *>(ptr) - 1;
    std::size_t  aligned_size = align16(size);

    if (block->size >= aligned_size)
        return ptr;

    void *new_ptr = malloc(size);
    if (!new_ptr)
        return nullptr;

    memcpy(new_ptr, ptr, block->size);
    free(ptr);
    return new_ptr;
}