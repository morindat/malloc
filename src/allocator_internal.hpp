#pragma once

#include "allocator.hpp"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <climits>
#include <unistd.h>
#include <sys/mman.h>

// ── block header & footer ──────────────────────────────────────────
struct BlockHeader {
    std::size_t  size;
    bool         is_free;
    BlockHeader *next;
};

struct BlockFooter {
    std::size_t size;   // must always mirror header size
};

// ── constants ──────────────────────────────────────────────────────
static constexpr std::size_t ALIGNMENT      = 16;
static constexpr std::size_t HEADER_SIZE    = sizeof(BlockHeader);
static constexpr std::size_t FOOTER_SIZE    = sizeof(BlockFooter);
static constexpr std::size_t BLOCK_OVERHEAD = HEADER_SIZE + FOOTER_SIZE;
static constexpr std::size_t MIN_SPLIT_SIZE = BLOCK_OVERHEAD + ALIGNMENT;
static constexpr std::size_t MMAP_THRESHOLD = 128UL * 1024;   // 128 KB

// ── heap anchor (defined in utils.cpp) ────────────────────────────
extern BlockHeader *heap_start;

// ── helpers (defined in utils.cpp) ────────────────────────────────
std::size_t  align16(std::size_t size);
BlockFooter *get_footer(BlockHeader *block);
BlockHeader *get_prev(BlockHeader *block);
void         write_footer(BlockHeader *block);
BlockHeader *find_free_block(BlockHeader *&last, std::size_t size);
void         split_block(BlockHeader *block, std::size_t size);
BlockHeader *request_sbrk(BlockHeader *last, std::size_t size);
BlockHeader *request_mmap(std::size_t size);