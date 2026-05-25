# pragma once

# include "allocator.hpp"
# include <cstddef>
# include <cstdint>
# include <unistd.h>
# include <sys/mman.h>

//----- BLOCK HEADER-----
struct BlockHeader {
    std::size_t size;
    bool is_free;
    BlockHeader* next;
};

// ----- CONSTANTS ------
static constexpr std::size_t ALIGNMENT = 16;
static constexpr std::size_t HEADER_SIZE = sizeof(BlockHeader);
static constexpr std::size_t MIN_SPLIT_SIZE = HEADER_SIZE + ALIGNMENT;
static constexpr std::size_t MMAP_THRESHHOLD = 128UL * 1024; // 128KB

// ----- HEAP ANCHOR -----
extern BlockHeader *heap_start;

// ----- HELPERS -----
std::size_t align16(std::size_t size);
BlockHeader *find_free_block(BlockHeader *&last, std::size_t size);
void split_block(BlockHeader *block, std::size_t size);
BlockHeader *request_sbrk(BlockHeader *last, std::size_t size);
BlockHeader *request_mmap(std::size_t);