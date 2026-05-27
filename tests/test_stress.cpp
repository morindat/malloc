#include <cassert>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>
#include <random>
#include <algorithm>

// ── constants ──────────────────────────────────────────────────────
static constexpr int    SEED           = 42;
static constexpr int    NUM_ALLOCS     = 10000;
static constexpr int    NUM_ITERATIONS = 5;
static constexpr size_t MIN_SIZE       = 1;
static constexpr size_t MAX_SIZE       = 512 * 1024;   // 512 KB

// ── helpers ────────────────────────────────────────────────────────
static bool is_aligned(void *ptr, std::size_t alignment) {
    return reinterpret_cast<std::uintptr_t>(ptr) % alignment == 0;
}

struct Allocation {
    void        *ptr;
    std::size_t  size;
    unsigned char pattern;   // what we wrote — lets us verify later
};

// ── tests ──────────────────────────────────────────────────────────

static void test_random_alloc_free() {
    // Randomly allocate and free blocks in mixed order
    // Verifies no corruption under realistic usage patterns
    std::mt19937 rng(SEED);
    std::uniform_int_distribution<std::size_t> size_dist(MIN_SIZE, MAX_SIZE);
    std::uniform_int_distribution<int>         action_dist(0, 1);
    std::uniform_int_distribution<unsigned char> pattern_dist(1, 255);

    std::vector<Allocation> live;
    live.reserve(NUM_ALLOCS);

    for (int i = 0; i < NUM_ALLOCS; i++) {
        // Randomly decide to allocate or free
        if (live.empty() || action_dist(rng) == 0) {
            std::size_t   size    = size_dist(rng);
            unsigned char pattern = pattern_dist(rng);

            void *ptr = malloc(size);
            assert(ptr != nullptr);
            assert(is_aligned(ptr, 16));

            memset(ptr, pattern, size);
            live.push_back({ptr, size, pattern});
        } else {
            // Pick a random live allocation to free
            std::uniform_int_distribution<std::size_t> idx_dist(0, live.size() - 1);
            std::size_t idx = idx_dist(rng);

            Allocation &alloc = live[idx];

            // Verify the pattern is still intact before freeing
            auto *p = static_cast<unsigned char *>(alloc.ptr);
            for (std::size_t j = 0; j < alloc.size; j++)
                assert(p[j] == alloc.pattern);

            free(alloc.ptr);
            live.erase(live.begin() + idx);
        }
    }

    // Free everything still alive
    for (auto &alloc : live) {
        auto *p = static_cast<unsigned char *>(alloc.ptr);
        for (std::size_t j = 0; j < alloc.size; j++)
            assert(p[j] == alloc.pattern);
        free(alloc.ptr);
    }

    std::cout << "PASS: random alloc/free with pattern verification\n";
}

static void test_high_churn() {
    // Repeatedly allocate and immediately free the same size
    // Exercises the free list reuse path heavily
    // If coalescing or the free list walk is broken this fragments fast
    std::mt19937 rng(SEED + 1);
    std::uniform_int_distribution<std::size_t> size_dist(16, 256);

    for (int i = 0; i < NUM_ALLOCS; i++) {
        std::size_t size = size_dist(rng);
        void *ptr = malloc(size);
        assert(ptr != nullptr);
        assert(is_aligned(ptr, 16));
        memset(ptr, 0xAB, size);
        free(ptr);
    }

    std::cout << "PASS: high churn alloc/free reuses blocks correctly\n";
}

static void test_many_live_allocs() {
    // Hold many allocations live simultaneously
    // Stresses the free list length and header chain integrity
    std::vector<Allocation> live;
    live.reserve(NUM_ALLOCS);

    std::mt19937 rng(SEED + 2);
    std::uniform_int_distribution<std::size_t>   size_dist(16, 4096);
    std::uniform_int_distribution<unsigned char> pattern_dist(1, 255);

    // Allocate all at once
    for (int i = 0; i < NUM_ALLOCS; i++) {
        std::size_t   size    = size_dist(rng);
        unsigned char pattern = pattern_dist(rng);

        void *ptr = malloc(size);
        assert(ptr != nullptr);
        assert(is_aligned(ptr, 16));

        memset(ptr, pattern, size);
        live.push_back({ptr, size, pattern});
    }

    // Verify all patterns intact while everything is still live
    for (auto &alloc : live) {
        auto *p = static_cast<unsigned char *>(alloc.ptr);
        for (std::size_t j = 0; j < alloc.size; j++)
            assert(p[j] == alloc.pattern);
    }

    // Free in reverse order — exercises backward pressure on the free list
    for (auto it = live.rbegin(); it != live.rend(); ++it)
        free(it->ptr);

    std::cout << "PASS: " << NUM_ALLOCS << " simultaneous live allocations verified\n";
}

static void test_mixed_sizes() {
    // Mix tiny, medium, and large allocations in the same session
    // Exercises both sbrk and mmap paths simultaneously
    std::vector<Allocation> live;

    struct SizeClass {
        std::size_t   size;
        unsigned char pattern;
    };

    SizeClass classes[] = {
        {1,              0x11},   // tiny   — sbrk, gets aligned to 16
        {16,             0x22},   // small  — sbrk
        {256,            0x33},   // medium — sbrk
        {4096,           0x44},   // large  — sbrk
        {64  * 1024,     0x55},   // near threshold — sbrk
        {256 * 1024,     0x66},   // above threshold — mmap
        {1024 * 1024,    0x77},   // large mmap
    };

    for (auto &cls : classes) {
        void *ptr = malloc(cls.size);
        assert(ptr != nullptr);
        assert(is_aligned(ptr, 16));
        memset(ptr, cls.pattern, cls.size);
        live.push_back({ptr, cls.size, cls.pattern});
    }

    // Verify all patterns
    for (auto &alloc : live) {
        auto *p = static_cast<unsigned char *>(alloc.ptr);
        for (std::size_t j = 0; j < alloc.size; j++)
            assert(p[j] == alloc.pattern);
    }

    for (auto &alloc : live)
        free(alloc.ptr);

    std::cout << "PASS: mixed size classes (sbrk + mmap) verified\n";
}

static void test_repeated_cycles() {
    // Run multiple full alloc/free cycles back to back
    // Checks that the allocator recovers cleanly after each cycle
    // A fragmented or corrupted heap will fail on the second or third cycle
    std::mt19937 rng(SEED + 3);
    std::uniform_int_distribution<std::size_t>   size_dist(16, 1024);
    std::uniform_int_distribution<unsigned char> pattern_dist(1, 255);

    for (int cycle = 0; cycle < NUM_ITERATIONS; cycle++) {
        std::vector<Allocation> live;
        live.reserve(1000);

        for (int i = 0; i < 1000; i++) {
            std::size_t   size    = size_dist(rng);
            unsigned char pattern = pattern_dist(rng);

            void *ptr = malloc(size);
            assert(ptr != nullptr);
            assert(is_aligned(ptr, 16));

            memset(ptr, pattern, size);
            live.push_back({ptr, size, pattern});
        }

        for (auto &alloc : live) {
            auto *p = static_cast<unsigned char *>(alloc.ptr);
            for (std::size_t j = 0; j < alloc.size; j++)
                assert(p[j] == alloc.pattern);
            free(alloc.ptr);
        }

        std::cout << "  cycle " << cycle + 1 << "/" << NUM_ITERATIONS << " clean\n";
    }

    std::cout << "PASS: " << NUM_ITERATIONS << " repeated alloc/free cycles\n";
}

static void test_calloc_stress() {
    // Stress calloc specifically — every byte must be zero
    std::mt19937 rng(SEED + 4);
    std::uniform_int_distribution<std::size_t> nmemb_dist(1, 100);
    std::uniform_int_distribution<std::size_t> size_dist(1, 256);

    for (int i = 0; i < 1000; i++) {
        std::size_t nmemb = nmemb_dist(rng);
        std::size_t size  = size_dist(rng);

        void *ptr = calloc(nmemb, size);
        assert(ptr != nullptr);
        assert(is_aligned(ptr, 16));

        // Every single byte must be zero
        auto *p = static_cast<unsigned char *>(ptr);
        for (std::size_t j = 0; j < nmemb * size; j++)
            assert(p[j] == 0x00);

        free(ptr);
    }

    std::cout << "PASS: calloc stress — all bytes zero verified\n";
}

static void test_realloc_stress() {
    // Chain reallocs of growing then shrinking sizes
    // Data integrity must hold across every resize
    std::mt19937 rng(SEED + 5);

    for (int i = 0; i < 500; i++) {
        std::size_t   initial = 16;
        unsigned char pattern = static_cast<unsigned char>(i % 255 + 1);

        void *ptr = malloc(initial);
        assert(ptr != nullptr);
        memset(ptr, pattern, initial);

        // Grow in steps
        std::size_t sizes[] = {32, 64, 128, 256, 512, 1024, 2048};
        for (auto size : sizes) {
            void *new_ptr = realloc(ptr, size);
            assert(new_ptr != nullptr);
            assert(is_aligned(new_ptr, 16));

            // Original bytes must survive every grow
            auto *p = static_cast<unsigned char *>(new_ptr);
            for (std::size_t j = 0; j < initial; j++)
                assert(p[j] == pattern);

            ptr = new_ptr;
        }

        // Shrink back down
        std::size_t shrink_sizes[] = {512, 128, 32, 16};
        for (auto size : shrink_sizes) {
            void *new_ptr = realloc(ptr, size);
            assert(new_ptr != nullptr);
            assert(is_aligned(new_ptr, 16));
            ptr = new_ptr;
        }

        free(ptr);
    }

    std::cout << "PASS: realloc stress — grow and shrink chains verified\n";
}

// ── main ───────────────────────────────────────────────────────────
int main() {
    std::cout << "=== stress tests ===\n";
    test_random_alloc_free();
    test_high_churn();
    test_many_live_allocs();
    test_mixed_sizes();
    test_repeated_cycles();
    test_calloc_stress();
    test_realloc_stress();
    std::cout << "=== all stress tests passed ===\n";
    return 0;
}