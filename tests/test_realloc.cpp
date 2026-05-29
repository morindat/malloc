#include <cassert>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <iostream>

// ── helpers ────────────────────────────────────────────────────────
static bool is_aligned(void *ptr, std::size_t alignment) {
    return reinterpret_cast<std::uintptr_t>(ptr) % alignment == 0;
}

static void fill_pattern(void *ptr, std::size_t size, unsigned char val) {
    memset(ptr, val, size);
}

static bool check_pattern(void *ptr, std::size_t size, unsigned char val) {
    auto *p = static_cast<unsigned char *>(ptr);
    for (std::size_t i = 0; i < size; i++)
        if (p[i] != val) return false;
    return true;
}

// ── tests ──────────────────────────────────────────────────────────

static void test_realloc_null_ptr() {
    // realloc(nullptr, size) must behave exactly like malloc(size)
    void *ptr = realloc(nullptr, 64);
    assert(ptr != nullptr);
    assert(is_aligned(ptr, 16));
    free(ptr);
    std::cout << "PASS: realloc(nullptr, size) behaves like malloc\n";
}

static void test_realloc_zero_size() {
    // realloc(ptr, 0) must behave exactly like free(ptr)
    void *ptr = malloc(64);
    assert(ptr != nullptr);
    void *result = realloc(ptr, 0);
    assert(result == nullptr);
    // ptr is now freed — do not touch it
    std::cout << "PASS: realloc(ptr, 0) behaves like free\n";
}

static void test_realloc_grow() {
    // Growing an allocation must preserve original data byte-for-byte
    void *ptr = malloc(64);
    assert(ptr != nullptr);
    fill_pattern(ptr, 64, 0xAA);

    void *new_ptr = realloc(ptr, 128);
    assert(new_ptr != nullptr);
    assert(is_aligned(new_ptr, 16));

    // First 64 bytes must be identical to what was written before
    assert(check_pattern(new_ptr, 64, 0xAA));

    free(new_ptr);
    std::cout << "PASS: realloc grow preserves original data\n";
}

static void test_realloc_shrink() {
    // Shrinking must return the same pointer and preserve data
    // up to the new smaller size
    void *ptr = malloc(128);
    assert(ptr != nullptr);
    fill_pattern(ptr, 128, 0xBB);

    void *new_ptr = realloc(ptr, 64);
    assert(new_ptr != nullptr);
    assert(is_aligned(new_ptr, 16));

    // Shrinking should return the same pointer — no new allocation needed
    assert(new_ptr == ptr);

    // Data up to new size must be intact
    assert(check_pattern(new_ptr, 64, 0xBB));

    free(new_ptr);
    std::cout << "PASS: realloc shrink returns same pointer, data intact\n";
}

static void test_realloc_same_size() {
    // Reallocating to the same size must be a no-op
    void *ptr = malloc(64);
    assert(ptr != nullptr);
    fill_pattern(ptr, 64, 0xCC);

    void *new_ptr = realloc(ptr, 64);
    assert(new_ptr != nullptr);
    assert(new_ptr == ptr);
    assert(check_pattern(new_ptr, 64, 0xCC));

    free(new_ptr);
    std::cout << "PASS: realloc same size is a no-op\n";
}

static void test_realloc_data_integrity_large() {
    // Grow across the mmap threshold — data must survive the copy
    // from an sbrk block into an mmap block
    void *ptr = malloc(64);
    assert(ptr != nullptr);
    fill_pattern(ptr, 64, 0xDD);

    // 256KB — forces the new allocation onto the mmap path
    void *new_ptr = realloc(ptr, 256 * 1024);
    assert(new_ptr != nullptr);
    assert(is_aligned(new_ptr, 16));

    // Original 64 bytes must have survived the cross-path copy
    assert(check_pattern(new_ptr, 64, 0xDD));

    free(new_ptr);
    std::cout << "PASS: realloc across sbrk->mmap boundary preserves data\n";
}

static void test_realloc_multiple_grows() {
    // Repeatedly grow the same allocation — data must survive each step
    void *ptr = malloc(16);
    assert(ptr != nullptr);
    fill_pattern(ptr, 16, 0xEE);

    std::size_t sizes[] = {32, 64, 128, 256, 512};
    for (auto size : sizes) {
        void *new_ptr = realloc(ptr, size);
        assert(new_ptr != nullptr);
        assert(is_aligned(new_ptr, 16));

        // Original pattern must survive every grow
        assert(check_pattern(new_ptr, 16, 0xEE));
        ptr = new_ptr;
    }

    free(ptr);
    std::cout << "PASS: repeated realloc grows preserve original data\n";
}

static void test_realloc_neighbours_intact() {
    void *before = malloc(64);
    void *ptr    = malloc(64);
    void *after  = malloc(64);
    void *guard  = malloc(64);   // keeps after from being the heap tail

    assert(before != nullptr);
    assert(ptr    != nullptr);
    assert(after  != nullptr);
    assert(guard  != nullptr);

    fill_pattern(before, 64, 0x11);
    fill_pattern(ptr,    64, 0x22);
    fill_pattern(after,  64, 0x33);
    fill_pattern(guard,  64, 0x44);

    void *new_ptr = realloc(ptr, 128);
    assert(new_ptr != nullptr);

    // new_ptr must not overlap before, after, or guard
    assert(new_ptr != before);
    assert(new_ptr != after);
    assert(new_ptr != guard);

    // neighbours must be completely untouched
    assert(check_pattern(before, 64, 0x11));
    assert(check_pattern(after,  64, 0x33));
    assert(check_pattern(guard,  64, 0x44));

    free(before);
    free(new_ptr);
    free(after);
    free(guard);
    std::cout << "PASS: realloc does not corrupt neighbouring allocations\n";
}

// ── main ───────────────────────────────────────────────────────────
int main() {
    std::cout << "=== realloc tests ===\n";
    test_realloc_null_ptr();
    test_realloc_zero_size();
    test_realloc_grow();
    test_realloc_shrink();
    test_realloc_same_size();
    test_realloc_data_integrity_large();
    test_realloc_multiple_grows();
    test_realloc_neighbours_intact();
    std::cout << "=== ALL REALLOC TESTS PASSED ===\n\n";
    return 0;
}