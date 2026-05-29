#include "allocator_internal.hpp"

// defined here, declared extern in allocator_internal.hpp
BlockHeader *heap_start = nullptr;

// ── footer helpers ─────
BlockFooter *get_footer(BlockHeader *block) {
    return reinterpret_cast<BlockFooter *>(
        reinterpret_cast<char *>(block) + HEADER_SIZE + block->size
    );
}

BlockHeader *get_prev(BlockHeader *block) {
    if (block == heap_start)
        return nullptr;

    // Get pointer to the footer of previous block
    BlockFooter *prev_footer = reinterpret_cast<BlockFooter *>(
        reinterpret_cast<char*>(block) - FOOTER_SIZE
    );

    // Calculate where previous header starts
    return reinterpret_cast<BlockHeader *>(
        reinterpret_cast<char*>(prev_footer) - prev_footer->size - HEADER_SIZE
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

void* align16_addr(void* raw_ptr) {
    // Align the address to 16-byte boundary
    uintptr_t addr = reinterpret_cast<uintptr_t>(raw_ptr);
    uintptr_t aligned = (addr + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
    return reinterpret_cast<void*>(aligned);
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

    char* remainder_start = reinterpret_cast<char*>(block) + HEADER_SIZE + size + FOOTER_SIZE;
    BlockHeader* remainder = reinterpret_cast<BlockHeader*>(remainder_start);

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
    std::size_t total_chunk = BLOCK_OVERHEAD + size;
    std::size_t total_aligned = align16(total_chunk);
    
    void* raw = sbrk(total_aligned);  // No extra, no alignment needed
    if (raw == (void*)-1) return nullptr;
    
    // Assume sbrk returns 16-byte aligned address (true on all modern systems)
    BlockHeader* block = (BlockHeader*)raw;
    block->size = total_aligned - BLOCK_OVERHEAD;
    block->is_free = false;
    block->next = nullptr;
    write_footer(block);
    
    if (last) {
        last->next = block;
    } else if (!heap_start) {
        heap_start = block;
    }
    
    return block;
}

BlockHeader *request_mmap(std::size_t size) {
    std::size_t total_chunk = BLOCK_OVERHEAD + size;

    std::size_t total_aligned = align16(total_chunk);

    void *ptr = mmap(
        nullptr, total_aligned,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS,
        -1, 0
    );

    if (ptr == MAP_FAILED)
        return nullptr;

    BlockHeader *block    = (BlockHeader*)ptr;
    block->size    = total_aligned - BLOCK_OVERHEAD;
    block->is_free = false;
    block->next    = nullptr;
    write_footer(block);

    return block;
}