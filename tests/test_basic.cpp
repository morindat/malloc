#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <cstring>

// ----- HELPER -----
static bool is_aligned(void *ptr, std::size_t alignment) {
    return reinterpret_cast<std::uintptr_t>(ptr) % alignment == 0;
}

// ----- TESTS -----

static void test_malloc_zero () {
    void *ptr = malloc(0);
    assert(ptr == nullptr);
    std::cout << "PASS: malloc(0) returns nullptr\n";
}

static void test_malloc_basic() {
    void *ptr = malloc(64);
    assert(ptr != nullptr);
    free(ptr);
    std::cout << "PASS: malloc(64) returns non-null\n";
}

static void test_malloc_alignment() {
    // test multiple sizes to ensure alignment holds regardless of request
    std::size_t sizes[] = {1, 3, 7, 13, 16, 17, 64, 100, 255};
    for (auto size : sizes) {
        void *ptr = malloc(size);
        assert(ptr != nullptr);
        assert(is_aligned(ptr, 16));
        free(ptr);
    }
    std::cout << "PASS: all allocations are 16-byte aligned\n";
}

static void test_malloc_write() {
    // writing to the allocation should not corrupt adjacent blocks
    auto *ptr = static_cast<unsigned char *>(malloc(64));
    assert(ptr != nullptr);

    for (int i = 0; i < 64; i++) {
        ptr[i] = static_cast<unsigned char>(i);
    }

    for (int i = 0; i < 64; i++) {
        assert(ptr[i] == static_cast<unsigned char>(i));
    }

    free(ptr);

    std::cout << "PASS: write and read back 64 bytes correctly\n";
}

static void test_free_null() {
    // free(nullptr) must be a no-op — no crash, no error
    free(nullptr);
    std::cout << "PASS: free(nullptr) is a no-op\n";
}

static void test_multiple_allocs() {
    // Multiple live allocations must never overlap
    void *a = malloc(32);
    void *b = malloc(32);
    void *c = malloc(32);

    assert(a != nullptr);
    assert(b != nullptr);
    assert(c != nullptr);

    assert(a != b);
    assert(b != c);
    assert(a != c);

    memset(a, 0xAA, 32);
    memset(b, 0xBB, 32);
    memset(c, 0xCC, 32);

    auto *pa = static_cast<unsigned char *>(a);
    auto *pb = static_cast<unsigned char *>(b);
    auto *pc = static_cast<unsigned char *>(c);

    for (int i = 0; i < 32; i++) assert(pa[i] == 0xAA);
    for (int i = 0; i < 32; i++) assert(pb[i] == 0xBB);
    for (int i = 0; i < 32; i++) assert(pc[i] == 0xCC);

    free(a);
    free(b);
    free(c);

    std::cout << "PASS: multiple allocations do not overlap\n";
}

static void test_large_allocation() {
    std::size_t large = 256 * 1024; // 256KB
    void *ptr = malloc(large);
    assert(ptr != nullptr);
    assert(is_aligned(ptr, 16));
    memset(ptr, 0X42, large);
    free(ptr);
    
    std::cout << "PASS: large allocation (mmap path) works correctly\n";
}

// ----- MAIN -----
int main() {
    std::cout << "----- BASIC TESTS -----\n";
    test_malloc_zero();
    test_malloc_basic();
    test_malloc_alignment();
    test_malloc_write();
    test_free_null();
    test_multiple_allocs();
    test_large_allocation();
    std::cout << "----- ALL BASIC TESTS PASSED -----\n";
    return 0;
}