#include "allocator_internal.hpp"

// defined here, declared extern in allocator_internal.hpp
BlockHeader *heap_start = nullptr;

// ── footer helpers ─────
BlockFooter *get_footer(BlockHeader *block) {
    return reinterpret_cast<BlockFooter *>(
        reinterpret_cast<char *>(block + 1) + block->size
    );
}

BlockHeader *get_prev(BlockHeader *block) {
    if (block == heap_start)
        return nullptr;

    BlockFooter *prev_footer = reinterpret_cast<BlockFooter *>(
        reinterpret_cast<char *>(block) - FOOTER_SIZE
    );

    return reinterpret_cast<BlockHeader *>(
        reinterpret_cast<char *>(block) - FOOTER_SIZE - prev_footer->size - HEADER_SIZE
    );
}

void write_footer(BlockHeader *block) {
    BlockFooter *footer = get_footer(block);
    footer->size = block->size;
}

// ── alignment ────
std::size_t align16(std::size_t size) {
    return (size + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
}

// ── free list walk ──────
BlockHeader *find_free_block(BlockHeader *&last, std::size_t size) {
    BlockHeader *cur = heap_start;

    while (cur) {
        if (cur->is_free && cur->size >= size)
            return cur;
        last = cur;
        cur  = cur->next;
    }

    return nullptr;
}

// ── splitting ──────────
void split_block(BlockHeader *block, std::size_t size) {
    if (block->size < size + MIN_SPLIT_SIZE)
        return;

    auto *remainder = reinterpret_cast<BlockHeader *>(
        reinterpret_cast<char *>(block + 1) + size + FOOTER_SIZE
    );

    remainder->size    = block->size - size - BLOCK_OVERHEAD;
    remainder->is_free = true;
    remainder->next    = block->next;
    write_footer(remainder);

    block->size = size;
    block->next = remainder;
    write_footer(block);
}

// ── OS requests ─────────────
BlockHeader *request_sbrk(BlockHeader *last, std::size_t size) {
    auto *block = reinterpret_cast<BlockHeader *>(sbrk(0));

    if (sbrk(static_cast<intptr_t>(BLOCK_OVERHEAD + size)) == reinterpret_cast<void *>(-1))
        return nullptr;

    block->size    = size;
    block->is_free = false;
    block->next    = nullptr;
    write_footer(block);

    if (last)
        last->next = block;
    else
        heap_start = block;

    return block;
}

BlockHeader *request_mmap(std::size_t size) {
    std::size_t total = BLOCK_OVERHEAD + size;

    void *ptr = mmap(
        nullptr, total,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS,
        -1, 0
    );

    if (ptr == MAP_FAILED)
        return nullptr;

    auto *block    = reinterpret_cast<BlockHeader *>(ptr);
    block->size    = size;
    block->is_free = false;
    block->next    = nullptr;
    write_footer(block);

    return block;
}