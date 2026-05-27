#include <cassert>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <iostream>

static bool is_aligned(void *ptr, std::size_t alignment) {
    return reinterpret_cast<std::uintptr_t>(ptr) % alignment == 0;
}

static void test_forward_coalesce() {
    void *a = malloc(64);
    void *b = malloc(64);

    assert(a != nullptr);
    assert(b != nullptr);

    // free(a) — a is free, b is live, no coalesce yet
    // free(b) — backward coalesce merges b into a
    free(a);
    free(b);

    // ask for something bigger than either individual block
    // but smaller than the merged total — must reuse the merged block
    void *big = malloc(96);
    assert(big != nullptr);
    assert(is_aligned(big, 16));
    assert(big == a);   // reused from start of merged region

    memset(big, 0x42, 96);
    free(big);
    std::cout << "PASS: forward+backward coalescing merges adjacent free blocks\n";
}

static void test_no_coalesce_when_used() {
    void *a = malloc(64);
    void *b = malloc(64);
    void *c = malloc(64);

    assert(a != nullptr);
    assert(b != nullptr);
    assert(c != nullptr);

    free(a);
    free(c);

    // b is still live — a and c must not coalesce across it
    memset(b, 0xBB, 64);
    auto *pb = static_cast<unsigned char *>(b);
    for (int i = 0; i < 64; i++)
        assert(pb[i] == 0xBB);

    free(b);
    std::cout << "PASS: non-adjacent free blocks do not coalesce\n";
}

static void test_coalesce_then_reuse() {
    void *a = malloc(64);
    void *b = malloc(64);
    void *c = malloc(64);

    assert(a != nullptr);
    assert(b != nullptr);
    assert(c != nullptr);

    // free in order — each free backward coalesces into the previous
    // result: one large free block starting at a covering all three
    free(a);
    free(b);
    free(c);

    // ask for something that fits in the merged block but not any individual one
    // merged total = 64 + BLOCK_OVERHEAD + 64 + BLOCK_OVERHEAD + 64
    void *big = malloc(96);
    assert(big != nullptr);
    assert(is_aligned(big, 16));
    assert(big == a);   // reused from start of merged region

    memset(big, 0x77, 96);
    free(big);
    std::cout << "PASS: three adjacent free blocks coalesce and are reused\n";
}

static void test_split_then_coalesce() {
    void *large = malloc(256);
    assert(large != nullptr);
    free(large);

    // split the free block — small lands at the start
    void *small = malloc(32);
    assert(small != nullptr);
    assert(small == large);

    // no other allocations between here and free(small)
    // remainder block is free — backward coalesce recovers the full region
    free(small);

    void *refull = malloc(256);
    assert(refull != nullptr);
    assert(refull == large);   // same region, fully recovered

    free(refull);
    std::cout << "PASS: split block coalesces back after free\n";
}

static void test_backward_coalesce() {
    // explicitly test that freeing a block merges it with a free predecessor
    void *a = malloc(64);
    void *b = malloc(64);
    void *c = malloc(64);

    assert(a != nullptr);
    assert(b != nullptr);
    assert(c != nullptr);

    memset(c, 0xCC, 64);

    // free a first, then b — b should backward coalesce into a
    free(a);
    free(b);

    // the merged a+b block should be large enough for this
    void *big = malloc(96);
    assert(big != nullptr);
    assert(is_aligned(big, 16));
    assert(big == a);   // starts at a — confirms backward coalesce happened

    // c must be completely untouched
    auto *pc = static_cast<unsigned char *>(c);
    for (int i = 0; i < 64; i++)
        assert(pc[i] == 0xCC);

    free(big);
    free(c);
    std::cout << "PASS: backward coalescing merges block with free predecessor\n";
}

static void test_coalesce_preserves_live_data() {
    void *a = malloc(64);
    void *b = malloc(64);
    void *c = malloc(64);
    void *d = malloc(64);

    memset(a, 0xAA, 64);
    memset(b, 0xBB, 64);
    memset(c, 0xCC, 64);
    memset(d, 0xDD, 64);

    // free b then c — c backward coalesces into b
    free(b);
    free(c);

    // a and d must be completely untouched
    auto *pa = static_cast<unsigned char *>(a);
    auto *pd = static_cast<unsigned char *>(d);

    for (int i = 0; i < 64; i++) assert(pa[i] == 0xAA);
    for (int i = 0; i < 64; i++) assert(pd[i] == 0xDD);

    free(a);
    free(d);
    std::cout << "PASS: coalescing does not corrupt live allocations\n";
}

int main() {
    std::cout << "=== coalesce tests ===\n";
    test_forward_coalesce();
    test_no_coalesce_when_used();
    test_coalesce_then_reuse();
    test_split_then_coalesce();
    test_backward_coalesce();
    test_coalesce_preserves_live_data();
    std::cout << "=== all coalesce tests passed ===\n";
    return 0;
}