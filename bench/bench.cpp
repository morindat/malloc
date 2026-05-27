#include <iostream>
#include <vector>
#include <random>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <numeric>

// ── timing helper ──────────────────────────────────────────────────
using Clock     = std::chrono::high_resolution_clock;
using TimePoint = std::chrono::time_point<Clock>;
using Ms        = std::chrono::duration<double, std::milli>;

static double elapsed_ms(TimePoint start, TimePoint end) {
    return std::chrono::duration_cast<Ms>(end - start).count();
}

// ── constants ──────────────────────────────────────────────────────
static constexpr int    SEED             = 42;
static constexpr int    SMALL_ITERS      = 100000;
static constexpr int    LARGE_ITERS      = 1000;
static constexpr int    MIXED_ITERS      = 50000;
static constexpr int    CHURN_ITERS      = 100000;
static constexpr int    REALLOC_ITERS    = 10000;
static constexpr size_t SMALL_SIZE       = 64;
static constexpr size_t LARGE_SIZE       = 256 * 1024;    // 256 KB — mmap path
static constexpr size_t MMAP_THRESHOLD   = 128 * 1024;    // 128 KB

// ── result printer ─────────────────────────────────────────────────
static void print_result(
    const char *name,
    int         iterations,
    double      ms,
    bool        print_throughput = true
) {
    double per_op_ns = (ms * 1e6) / iterations;
    std::cout << "  " << name << "\n"
              << "    total:    " << ms       << " ms\n"
              << "    per op:   " << per_op_ns << " ns\n";
    if (print_throughput) {
        double ops_per_sec = iterations / (ms / 1000.0);
        std::cout << "    ops/sec:  " << static_cast<long>(ops_per_sec) << "\n";
    }
    std::cout << "\n";
}

// ── benchmarks ─────────────────────────────────────────────────────

static void bench_fixed_size_alloc_free() {
    // Measures the raw cost of malloc+free for a fixed small size
    // This is the best-case for your free list — same size reused every time
    std::cout << "── fixed size alloc/free (" << SMALL_ITERS << " iters, "
              << SMALL_SIZE << " bytes) ──\n";

    auto start = Clock::now();
    for (int i = 0; i < SMALL_ITERS; i++) {
        void *ptr = malloc(SMALL_SIZE);
        // Volatile write prevents the compiler optimising the alloc away
        *static_cast<volatile char *>(ptr) = 1;
        free(ptr);
    }
    auto end = Clock::now();

    print_result("malloc+free", SMALL_ITERS, elapsed_ms(start, end));
}

static void bench_large_alloc_free() {
    // Measures mmap path specifically
    // Each iteration is an mmap + munmap round trip
    std::cout << "── large alloc/free (" << LARGE_ITERS << " iters, "
              << LARGE_SIZE / 1024 << " KB, mmap path) ──\n";

    auto start = Clock::now();
    for (int i = 0; i < LARGE_ITERS; i++) {
        void *ptr = malloc(LARGE_SIZE);
        *static_cast<volatile char *>(ptr) = 1;
        free(ptr);
    }
    auto end = Clock::now();

    print_result("malloc+free (mmap)", LARGE_ITERS, elapsed_ms(start, end));
}

static void bench_mixed_sizes() {
    // Random sizes across both sbrk and mmap paths
    // Most realistic benchmark — closest to real application behaviour
    std::cout << "── mixed size alloc/free (" << MIXED_ITERS << " iters) ──\n";

    std::mt19937 rng(SEED);
    std::uniform_int_distribution<std::size_t> dist(1, 512 * 1024);

    std::vector<std::size_t> sizes(MIXED_ITERS);
    for (auto &s : sizes)
        s = dist(rng);

    auto start = Clock::now();
    for (int i = 0; i < MIXED_ITERS; i++) {
        void *ptr = malloc(sizes[i]);
        *static_cast<volatile char *>(ptr) = 1;
        free(ptr);
    }
    auto end = Clock::now();

    print_result("malloc+free (mixed)", MIXED_ITERS, elapsed_ms(start, end));
}

static void bench_high_churn() {
    // Rapidly alternates between allocating and freeing
    // Stresses free list reuse — measures how well first-fit performs
    // under pressure when blocks are constantly recycled
    std::cout << "── high churn (" << CHURN_ITERS << " iters) ──\n";

    std::mt19937 rng(SEED + 1);
    std::uniform_int_distribution<std::size_t> dist(8, 512);
    std::uniform_int_distribution<int>         action(0, 1);

    std::vector<void *> live;
    live.reserve(1000);

    auto start = Clock::now();
    for (int i = 0; i < CHURN_ITERS; i++) {
        if (live.empty() || action(rng) == 0) {
            void *ptr = malloc(dist(rng));
            *static_cast<volatile char *>(ptr) = 1;
            live.push_back(ptr);
        } else {
            std::uniform_int_distribution<std::size_t> idx(0, live.size() - 1);
            std::size_t i = idx(rng);
            free(live[i]);
            live.erase(live.begin() + i);
        }
    }
    for (void *ptr : live)
        free(ptr);
    auto end = Clock::now();

    print_result("high churn", CHURN_ITERS, elapsed_ms(start, end));
}

static void bench_many_live() {
    // Allocates many blocks and holds them all live simultaneously
    // Measures allocation time when the free list is long and mostly used
    // This is the worst case for first-fit — long walk, few hits
    std::cout << "── many live allocs (" << SMALL_ITERS << " allocs held live) ──\n";

    std::vector<void *> ptrs;
    ptrs.reserve(SMALL_ITERS);

    auto start = Clock::now();
    for (int i = 0; i < SMALL_ITERS; i++) {
        void *ptr = malloc(SMALL_SIZE);
        *static_cast<volatile char *>(ptr) = 1;
        ptrs.push_back(ptr);
    }
    auto mid = Clock::now();

    for (void *ptr : ptrs)
        free(ptr);
    auto end = Clock::now();

    double alloc_ms = elapsed_ms(start, mid);
    double free_ms  = elapsed_ms(mid,   end);

    std::cout << "  alloc phase\n";
    print_result("malloc", SMALL_ITERS, alloc_ms);
    std::cout << "  free phase\n";
    print_result("free",   SMALL_ITERS, free_ms);
}

static void bench_realloc() {
    // Measures realloc growing an allocation in steps
    // Exercises the malloc+memcpy+free path repeatedly
    std::cout << "── realloc grow chain (" << REALLOC_ITERS << " chains) ──\n";

    std::size_t steps[] = {16, 32, 64, 128, 256, 512, 1024};
    int         nsteps  = sizeof(steps) / sizeof(steps[0]);

    auto start = Clock::now();
    for (int i = 0; i < REALLOC_ITERS; i++) {
        void *ptr = malloc(8);
        for (int s = 0; s < nsteps; s++)
            ptr = realloc(ptr, steps[s]);
        free(ptr);
    }
    auto end = Clock::now();

    int total_ops = REALLOC_ITERS * (nsteps + 1);
    print_result("realloc chain", total_ops, elapsed_ms(start, end));
}

static void bench_calloc() {
    // Measures calloc specifically — the zeroing cost matters
    // Compare against malloc to see how much memset actually costs
    std::cout << "── calloc vs malloc (" << SMALL_ITERS << " iters, "
              << SMALL_SIZE << " bytes) ──\n";

    // calloc
    auto start = Clock::now();
    for (int i = 0; i < SMALL_ITERS; i++) {
        void *ptr = calloc(1, SMALL_SIZE);
        *static_cast<volatile char *>(ptr) = 1;
        free(ptr);
    }
    auto mid = Clock::now();

    // malloc — baseline for comparison
    for (int i = 0; i < SMALL_ITERS; i++) {
        void *ptr = malloc(SMALL_SIZE);
        *static_cast<volatile char *>(ptr) = 1;
        free(ptr);
    }
    auto end = Clock::now();

    std::cout << "  calloc\n";
    print_result("calloc+free", SMALL_ITERS, elapsed_ms(start, mid));
    std::cout << "  malloc (baseline)\n";
    print_result("malloc+free", SMALL_ITERS, elapsed_ms(mid, end));
}

static void bench_fragmentation() {
    // Allocates blocks of alternating sizes and frees every other one
    // Measures how much heap space is wasted due to fragmentation
    // A good allocator should coalesce and reuse most of it
    std::cout << "── fragmentation ──\n";

    static constexpr int N = 10000;
    std::vector<void *> ptrs(N);

    // Allocate N blocks alternating between small and medium
    for (int i = 0; i < N; i++)
        ptrs[i] = malloc(i % 2 == 0 ? 32 : 128);

    // Free every other block — creates a checkerboard of free/live
    for (int i = 0; i < N; i += 2)
        free(ptrs[i]);

    // Try to allocate something that requires coalescing to satisfy
    // If fragmentation is bad this will go to sbrk unnecessarily
    int reused = 0;
    for (int i = 0; i < N / 2; i++) {
        void *ptr = malloc(32);
        // Check if it landed in the freed region (rough heuristic)
        bool in_range = ptr >= ptrs[1] && ptr <= ptrs[N - 1];
        if (in_range) reused++;
        free(ptr);
    }

    // Free remaining live blocks
    for (int i = 1; i < N; i += 2)
        free(ptrs[i]);

    double reuse_rate = (reused * 100.0) / (N / 2);
    std::cout << "  free list reuse rate: " << reuse_rate << "%\n\n";
}

// ── main ───────────────────────────────────────────────────────────
int main() {
    std::cout << "==============================\n"
              << "  allocator benchmark\n"
              << "==============================\n\n";

    bench_fixed_size_alloc_free();
    bench_large_alloc_free();
    bench_mixed_sizes();
    bench_high_churn();
    bench_many_live();
    bench_realloc();
    bench_calloc();
    bench_fragmentation();

    std::cout << "==============================\n"
              << "  done\n"
              << "==============================\n";
    return 0;
}