# Comprehensive Error Report: Custom malloc Implementation

**Generated**: 2026-05-25  
**Status**: All tests failing at alignment check  
**Severity Summary**: 4 Critical, 6 High, 4 Medium

---

## Executive Summary

The custom malloc allocator has **14 documented errors** preventing basic functionality. The highest-priority issue is **incorrect pointer alignment** which causes all tests to abort immediately. Secondary issues include **undefined behavior in free()** due to conflating allocation origin detection with block size, and **integer overflow vulnerabilities** in critical paths. The allocator is not safe for production or as an LD_PRELOAD replacement in its current state.

---

## Critical Errors (Blocking All Tests)

### ❌ ERROR #1: Returned Pointers Are Not 16-Byte Aligned

**Status**: CRITICAL — All tests fail here  
**Test Evidence**:
```
test_basic: tests/test_basic.cpp:33: void test_malloc_alignment(): 
Assertion `is_aligned(ptr, 16)' failed.
```

**Root Cause**:

The `BlockHeader` struct is 24 bytes on this platform:
```cpp
struct BlockHeader {
    std::size_t  size;      // 8 bytes
    bool         is_free;   // 1 byte + 15 bytes compiler padding
    BlockHeader *next;      // 8 bytes
};                          // = 24 bytes total
```

When `malloc()` returns `(void*)(block + 1)`, it returns an address that is `block + 24`. If `block` is 16-byte aligned, `block + 24` is only 8 mod 16, not 0 mod 16.

**Diagnosis Output**:
```
User pointer alignment: 24 mod 16 = 8
Expected: 0 mod 16
Actual: 8 mod 16
```

**Impact**:
- **All four test suites abort immediately** on the first `malloc()` call that checks alignment (lines 33, 27, 29, 49 in tests)
- Makes impossible any further testing of coalescing, realloc, or stress patterns
- Breaks real programs that use SSE/AVX or require 16-byte aligned data (e.g., SIMD, aligned_alloc semantics)
- Violates the allocator's own advertised contract (see `allocator_internal.hpp:23`)

**Code Locations**:
- Struct definition: `src/allocator_internal.hpp:12-16`
- Return statement: `src/allocator.cpp:16, 27`
- Test assertions: `tests/test_basic.cpp:33`, `tests/test_coalesce.cpp:27`, `tests/test_realloc.cpp:29`, `tests/test_stress.cpp:49`

**Fix Suggestion** (outline only):
- Store the header **after** the user pointer, not before it
- Or adjust the user-facing pointer to skip padding bytes
- Or use over-alignment of the initial sbrk/mmap to guarantee alignment after the header

---

### ❌ ERROR #2: Real Programs Crash Under LD_PRELOAD

**Status**: CRITICAL — Exit code 139 (segfault)  
**Observed Failures**:
```bash
LD_PRELOAD=./mymalloc.so /usr/bin/cat /etc/hostname
# Exit code 139 (SIGSEGV)

LD_PRELOAD=./mymalloc.so /usr/bin/wc -l /etc/hostname
# Exit code 139 (SIGSEGV)

make preload
# Fails at cat/wc tests with Error 139
```

**Root Cause**:

Exit code 139 = 128 + 11 (SIGSEGV). This is caused by violations of ABI/libc expectations:

1. **Alignment violation**: As documented in Error #1, returned pointers don't meet alignment guarantees
2. **Missing allocator semantics**: libc and coreutils depend on malloc providing:
   - Correct alignment
   - Proper response to allocation limits
   - Valid metadata layout for malloc_usable_size() calls (if present)
   - Thread-safety (if multithreaded)

**Impact**:
- Allocator cannot be used as a drop-in replacement with real binaries
- Programs using SSE/AVX, SIMD, or type-aligned data will crash
- Debugging is difficult because error happens deep in libc calls

**Code Locations**:
- Core allocator: `src/allocator.cpp:3-28`
- All platform calls: `src/utils.cpp:70-108`

---

### ❌ ERROR #3: All Test Suites Abort Immediately

**Status**: CRITICAL — All tests fail  
**Test Output**:
```
make test
── running tests/test_basic ──
test_basic: tests/test_basic.cpp:33: void test_malloc_alignment(): 
Assertion `is_aligned(ptr, 16)' failed.
Aborted (core dumped)
── running tests/test_coalesce ──
test_coalesce: tests/test_coalesce.cpp:27: void test_forward_coalesce(): 
Assertion `is_aligned(big, 16)' failed.
Aborted (core dumped)
── running tests/test_realloc ──
test_realloc: tests/test_realloc.cpp:29: void test_realloc_null_ptr(): 
Assertion `is_aligned(ptr, 16)' failed.
Aborted (core dumped)
── running tests/test_stress ──
test_stress: tests/test_stress.cpp:49: void test_random_alloc_free(): 
Assertion `is_aligned(ptr, 16)' failed.
Aborted (core dumped)
make: *** [Makefile:21: test] Error 134
```

**Root Cause**: Error #1 — alignment failure

**Impact**:
- No tests reach meaningful functionality checks
- Coalescing behavior is untested
- Realloc correctness is untested
- Stress patterns are untested
- No confidence in any allocator invariants

---

### ❌ ERROR #4: free() Misclassifies Block Origin Using Size Alone

**Status**: CRITICAL — Latent corruption risk  
**Problem Code** (`src/allocator.cpp:36-39`):
```cpp
if (block->size >= MMAP_THRESHOLD) {
    munmap(block, BLOCK_OVERHEAD + block->size);
    return;
}
```

**The Problem**:

`free()` assumes: **if `block->size >= 128 KB`, then the block came from `mmap()`, else from `sbrk()`**

This is **incorrect** when coalescing exists:

1. Two `sbrk` blocks of 64 KB each are allocated  
2. Both are freed, backward-coalescing into one 128+ KB block  
3. Later, that merged block (originally from `sbrk`) is freed  
4. `free()` sees `size >= MMAP_THRESHOLD` and calls `munmap()` on `sbrk` memory
5. **Corrupts the program break and heap state**
6. Next allocation fails or crashes

**Scenario Walkthrough**:

```
1. malloc(64KB)  → sbrk block A, size=64KB
2. malloc(64KB)  → sbrk block B, size=64KB
3. free(A)       → block A marked free
4. free(B)       → backward coalesce: A->size = 64KB + 32 (overhead) + 64KB = 160KB
                   A->next = nullptr
5. malloc(x)     → no fit, requests new sbrk
6. free(A)       → size >= 128KB? YES
                → calls munmap(A, 192)  ← WRONG! This is sbrk memory!
                → munmap succeeds (or fails silently)
                → Heap is now corrupted
```

**Why This Matters**:

- `sbrk()` memory cannot be unmapped with `munmap()` safely
- `mmap()` memory should not be left on the free list (should be returned to OS immediately)
- Coalescing invalidates the size-based classification assumption

**Impact**:
- If coalescing crosses the 128 KB boundary, `munmap()` will be called on heap memory
- Heap corruption, segfaults, or silent memory loss
- Latent — may not surface until stress test with large blocks

**Code Locations**:
- Classification logic: `src/allocator.cpp:36-39`
- Coalesce merging: `src/allocator.cpp:44-57`
- Allocation thresholding: `src/allocator.cpp:19`

**What's Needed**:

The allocator must **track the origin of each block independently**. Possible solutions:
- Add a `source` enum field to `BlockHeader` (mmap vs sbrk)
- Use an out-of-band map to record which blocks came from mmap
- Do not coalesce across mmap/sbrk boundaries

---

## High-Severity Errors (Will Corrupt Heap or Violate Invariants)

### ⚠️ ERROR #5: get_prev() Reads Arbitrary Memory Without Bounds

**Status**: HIGH — Corrupts or crashes during backward coalesce  
**Problem Code** (`src/utils.cpp:13-23`):
```cpp
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
```

**The Problem**:

1. **No validation** that the computed address is within the allocator's heap
2. **Assumes** the previous block's footer is valid and contains a real size
3. **Reads potentially invalid memory** if:
   - A bad pointer is passed to `free()`
   - Metadata is corrupted by a buffer overflow
   - The block is not part of the heap list
   - An mmap block is passed (it has no physical predecessor in the heap)

**Bad Input Examples**:

```cpp
// Scenario 1: Stack address passed to free()
int x = 42;
free(&x);
// get_prev() reads invalid memory on the stack

// Scenario 2: Metadata corruption
char *ptr = malloc(100);
*(ptr - 10) = 0xFF;  // Overwrite header
free(ptr);           // get_prev() reads corrupted footer

// Scenario 3: Dangling pointer
char *p = malloc(100);
free(p);
free(p);             // Double free — get_prev() reads freed footer
```

**Impact**:
- **Silent corruption** or **crash** depending on what memory is read
- Backward coalescing can corrupt arbitrary heap regions
- Makes double-free detection impossible
- Production allocators have hardened checks here (canaries, pointer validation)

**Code Locations**:
- Function: `src/utils.cpp:13-23`
- Callers: `src/allocator.cpp:52`

**What's Needed**:
- Validate that `block >= heap_start` and is within known heap range
- Optionally add canary bytes to footer to detect corruption
- Consider maintaining a doubly-linked list to eliminate footer reads

---

### ⚠️ ERROR #6: Coalescing Does Not Validate Physical Adjacency

**Status**: HIGH — Can merge non-adjacent blocks  
**Problem Code** (`src/allocator.cpp:44-57`):
```cpp
// forward coalesce
if (block->next && block->next->is_free) {
    block->size += BLOCK_OVERHEAD + block->next->size;
    block->next  = block->next->next;
    write_footer(block);
}

// backward coalesce (via get_prev)
BlockHeader *prev = get_prev(block);
if (prev && prev->is_free) {
    prev->size += BLOCK_OVERHEAD + block->size;
    prev->next  = block->next;
    write_footer(prev);
}
```

**The Problem**:

The code assumes:
- **`block->next` always points to the physically next block** in memory
- **`get_prev(block)` always returns the physically previous block**

In the current design this is true (singly-linked list of all blocks). But:

1. If the list ever changes (e.g., to store only free blocks), this breaks
2. If the list is ever sorted or reordered, this creates invalid coalesces
3. If mmap blocks are mixed with sbrk blocks, physical adjacency cannot be guaranteed

**Example Corruption Scenario**:

Imagine a hypothetical future design where the free list is rearranged:
```
Memory layout:     [A: 64] [B: 64] [C: 64]
Free list order:   B -> C -> A  (rearranged by allocator)

When freeing C and A:
  - C is freed
  - A is freed, get_prev(A) returns B
  - B is free, so: A->size += B->size + overhead
  - But B is NOT physically adjacent to A!
  - Result: invalid merged block spanning non-contiguous memory
```

**Impact**:
- **Latent but severe**: Only surfaces if list structure changes
- Future maintainers might not understand the invariant
- Corrupts the heap in subtle ways

**Code Locations**:
- Coalesce logic: `src/allocator.cpp:44-57`
- List structure: `src/allocator_internal.hpp:12-16`

**What's Needed**:
- Add explicit validation: `assert physical_adjacency(prev, block)`
- Or add a `previous` pointer to BlockHeader and maintain it explicitly
- Document the invariant clearly

---

### ⚠️ ERROR #7: realloc() Doesn't Split Blocks When Shrinking

**Status**: HIGH — Memory waste and fragmentation  
**Problem Code** (`src/allocator.cpp:89-90`):
```cpp
if (block->size >= aligned_size)
    return ptr;  // Same pointer, even if shrinking from 256 to 64
```

**The Problem**:

When `realloc(ptr, smaller_size)` is called on a block that's already large enough:
- The allocator returns the same pointer without splitting
- The excess memory remains allocated but unused
- This wastes memory and increases fragmentation

**Example**:

```cpp
char *p = malloc(256);
p = realloc(p, 64);  // Shrinking to 64 bytes
// But the block still consumes 256 bytes
// The remaining 192 bytes are wasted
```

**Test Expectation** (from `tests/test_realloc.cpp:73`):
```cpp
assert(new_ptr == ptr);  // Same pointer — test expects no split
```

The test **expects** this behavior. However, it's not optimal:
- Real allocators (libc malloc, jemalloc, mimalloc) may split large shrinks
- This design prioritizes speed over space efficiency

**Impact**:
- **Memory waste**: Unused tail memory not returned to free list
- **Fragmentation**: Long-lived shrunken blocks fragment the heap
- Medium priority because tests expect this behavior

**Code Locations**:
- realloc implementation: `src/allocator.cpp:77-99`
- Test expectation: `tests/test_realloc.cpp:61-79`

**Current Design Rationale**: Probably for simplicity — tests confirm this is intentional

---

### ⚠️ ERROR #8: No Double-Free Detection

**Status**: HIGH — Silent corruption  
**Problem Code** (`src/allocator.cpp:30-58`):

No check for whether `block->is_free` is already true when `free()` is called:

```cpp
extern "C" void free(void *ptr) {
    if (!ptr)
        return;

    BlockHeader *block = reinterpret_cast<BlockHeader *>(ptr) - 1;

    // No check: if (block->is_free) return;  ← MISSING!

    block->is_free = true;
    write_footer(block);
    
    // Now coalesces using potentially stale metadata...
}
```

**The Problem**:

```cpp
char *p = malloc(100);
free(p);
free(p);  // ← Undefined behavior, but should be detected
```

After first `free(p)`:
- `block->is_free = true`
- Block is placed on free list

After second `free(p)`:
- Same `block` is accessed again
- `block->is_free` is already true, but no check
- Coalescing runs again with stale pointers
- May corrupt the free list by linking already-linked blocks

**Impact**:
- Double-free bugs are hard to debug (no immediate crash)
- Heap corruption spreads to subsequent allocations
- Production allocators detect and trap this

**Code Locations**:
- free() function: `src/allocator.cpp:30-58`

**What's Needed**:
- Add early exit: `if (block->is_free) { warn_or_abort(); return; }`
- Or use a canary byte to detect freed blocks

---

## Medium-Severity Errors (Correctness & Edge Cases)

### ⚠️ ERROR #9: align16() Function Overflows for Large Sizes

**Status**: MEDIUM — Edge case, but silent corruption possible  
**Problem Code** (`src/utils.cpp:32-34`):
```cpp
std::size_t align16(std::size_t size) {
    return (size + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
}
```

**The Problem**:

If `size` is close to `SIZE_MAX`, the addition `size + 15` wraps around:

```
SIZE_MAX = 18446744073709551615
align16(SIZE_MAX - 5):
  size + 15 = 18446744073709551620 → wraps to 4 (mod 2^64)
  Result: 0  ← Returns 0 instead of error!
```

**Diagnostic Output**:
```
align16(18446744073709551610) = 0
This is MUCH SMALLER than input — overflow occurred!
```

**Impact**:
- `malloc(SIZE_MAX - 5)` returns a **0-byte allocation**
- User code may allocate "huge_buffer" expecting SIZE_MAX bytes
- Gets buffer of size 0
- Buffer overflow when user writes to it

**Affected Calls**:
- `malloc()`: calls `align16()` at line 7
- `realloc()`: calls `align16()` at line 87

**Also affects**:
- `request_sbrk()` at line 73: `BLOCK_OVERHEAD + size` can overflow when adding overhead to huge sizes

**Code Locations**:
- align16 function: `src/utils.cpp:32-34`
- Callers: `src/allocator.cpp:7`, `src/allocator.cpp:87`
- request_sbrk: `src/utils.cpp:73`

**What's Needed**:
- Check for overflow before arithmetic: `if (size > SIZE_MAX - 15) return error`
- Or use checked arithmetic

---

### ⚠️ ERROR #10: request_sbrk() Can Overflow When Adding Overhead

**Status**: MEDIUM — Latent integer overflow  
**Problem Code** (`src/utils.cpp:70-86`):
```cpp
BlockHeader *request_sbrk(BlockHeader *last, std::size_t size) {
    auto *block = reinterpret_cast<BlockHeader *>(sbrk(0));

    if (sbrk(static_cast<intptr_t>(BLOCK_OVERHEAD + size)) == reinterpret_cast<void *>(-1))
        return nullptr;
    // ... rest of function
}
```

**The Problem**:

When allocating huge sizes:
1. `size` is already aligned (by `align16()`)
2. `BLOCK_OVERHEAD + size` can overflow as `std::size_t`
3. Overflow wraps to a small number
4. Cast to `intptr_t` (signed) can be negative
5. `sbrk()` with negative argument shrinks the heap

**Example**:
```
size = 18446744073709551584  (SIZE_MAX - 31)
BLOCK_OVERHEAD = 32
size + BLOCK_OVERHEAD = 32  (wrapped!)
sbrk(32) → allocates 32 bytes instead of SIZE_MAX
```

**Impact**:
- Huge allocation requests silently become tiny allocations
- User gets minuscule buffer when expecting huge one
- Buffer overflow when user writes

**Code Locations**:
- request_sbrk: `src/utils.cpp:70-86`, specifically line 73

**What's Needed**:
- Check: `if (size > SIZE_MAX - BLOCK_OVERHEAD) return nullptr`

---

### ⚠️ ERROR #11: Valgrind Test Target Is Not Testing Your Allocator

**Status**: MEDIUM — False negatives in testing  
**Problem Code** (`Makefile:31-36`):
```makefile
valgrind: $(LIB) $(TEST_BINS)
	@for t in $(TEST_BINS); do \
		echo "── valgrind $$t ──"; \
		LD_PRELOAD=./$(LIB) valgrind --leak-check=full --error-exitcode=1 $$t; \
	done
```

**The Problem**:

When you run:
```bash
make valgrind
```

Valgrind's own replacement malloc is intercepting allocations:

```
at 0x4850858: malloc (vg_replace_malloc.c:447)
at 0x485807F: realloc (vg_replace_malloc.c:1804)
```

**Why**:

Valgrind has its own malloc replacement that takes priority. Even with `LD_PRELOAD`, Valgrind's memory tracking wraps your allocator, diluting the test.

**Impact**:
- Tests may pass under Valgrind but fail in normal execution (false positives)
- Tests may fail under Valgrind due to Valgrind's semantics, not your code
- You can't reliably debug memory issues with Valgrind in this setup

**Code Locations**:
- Makefile target: `Makefile:31-36`

**What's Needed**:
- Run tests WITHOUT Valgrind to verify functionality first
- Use Valgrind's `--soname-synonyms` or `--suppressions` to control interception
- Or accept Valgrind results are not reliable for custom allocators

---

### ⚠️ ERROR #12: preload_test.sh Uses eval() and Loses Signal Clarity

**Status**: MEDIUM — Testing methodology issue  
**Problem Code** (`scripts/preload_test.sh:55-75`):
```bash
run_with_preload() {
    local cmd="$1"
    output=$(LD_PRELOAD="$LIB" eval "$cmd" 2>&1)
    exit_code=$?
    # ...
}
```

**The Problem**:

- Uses `eval` to run command strings (fragile to quoting)
- Mixes shell errors with allocator failures
- Exit codes from pipes and subshells can mask failures

**Example**:
```bash
run_with_preload "cat /etc/hostname | wc -l"
# If cat fails: exit code 139
# If wc fails: exit code 139
# If the preload fails: exit code 139
# You can't tell which component failed!
```

**Impact**:
- Harder to debug preload test failures
- Shell syntax errors can hide allocator bugs
- Real executable failures mix with allocator failures

**Code Locations**:
- Test framework: `scripts/preload_test.sh:55-75`, `101-121`, `146-160`

---

## Low-Severity Issues (Documentation & Hygiene)

### ℹ️ ERROR #13: README.md Is Out of Date

**Status**: LOW — Documentation drift  
**Problem Code** (README.md):

The README claims:
- "Forward coalescing only (backward coalescing would require boundary tags)"
- But the code already has both backward coalescing and boundary tags (footers)

**Current Code** (`src/allocator.cpp:44-57`):
```cpp
// forward coalesce
if (block->next && block->next->is_free) { ... }

// backward coalesce  ← This exists!
BlockHeader *prev = get_prev(block);
if (prev && prev->is_free) { ... }
```

**Impact**:
- Misleads future readers about implementation status
- Makes debugging harder (documentation doesn't match code)
- Can lead to duplicate feature implementations

**Code Locations**:
- README: statements about coalescing strategy
- Actual implementation: `src/allocator.cpp:44-57`, `src/allocator_internal.hpp:18-20`

**What's Needed**:
- Update README to reflect current implementation
- Document that both forward and backward coalescing exist
- Document footer layout and backward coalescing algorithm

---

### ℹ️ ERROR #14: Makefile Does Not Exit on First Test Failure

**Status**: LOW — CI/CD hygiene  
**Problem Code** (`Makefile:20-24`):
```makefile
test: $(LIB) $(TEST_BINS)
	@for t in $(TEST_BINS); do \
		echo "── running $$t ──"; \
		LD_PRELOAD=./$(LIB) $$t; \
	done
```

**The Problem**:

The `for` loop does not exit on first failure. If test 1 aborts, tests 2, 3, 4 still run:

```
── running tests/test_basic ──
test_basic: tests/test_basic.cpp:33: Assertion `is_aligned(ptr, 16)' failed.
Aborted (core dumped)
── running tests/test_coalesce ──
test_coalesce: tests/test_coalesce.cpp:27: Assertion `is_aligned(big, 16)' failed.
Aborted (core dumped)
── running tests/test_realloc ──
test_realloc: tests/test_realloc.cpp:29: Assertion `is_aligned(ptr, 16)' failed.
Aborted (core dumped)
── running tests/test_stress ──
test_stress: tests/test_stress.cpp:49: Assertion `is_aligned(ptr, 16)' failed.
Aborted (core dumped)
make: *** [Makefile:21: test] Error 134
```

All tests run and all fail on alignment. This obscures that they're all failing on the same root cause.

**Impact**:
- Harder to focus on first error
- Noise in CI/CD output
- Minor (but real usability issue)

**Code Locations**:
- Makefile: lines 20-24

**What's Needed**:
```makefile
test: $(LIB) $(TEST_BINS)
	@for t in $(TEST_BINS); do \
		echo "── running $$t ──"; \
		LD_PRELOAD=./$(LIB) $$t || exit 1; \
	done
```

---

## Priority Fix Order

Based on the dependency chain and severity:

### Phase 1: Foundation (Must-fix for any progress)
1. **ERROR #1** (alignment): Fix pointer alignment so tests can run
   - This unblocks all other testing
   - No progress possible without this

### Phase 2: Safety (Before stress testing)
2. **ERROR #4** (mmap/sbrk classification): Add `source` field to BlockHeader
   - Prevents heap corruption from misclassified blocks
   - Enables safe coalescing
3. **ERROR #5** (get_prev bounds): Add validation to prevent reading invalid memory

### Phase 3: Correctness (Before production use)
4. **ERROR #6** (adjacency): Add assertion that blocks are physically adjacent
5. **ERROR #8** (double-free): Add `is_free` check to detect double-frees
6. **ERROR #9 & #10** (overflow): Add checked arithmetic for large sizes

### Phase 4: Quality (Nice-to-have improvements)
7. **ERROR #7** (shrink splitting): Split blocks when realloc shrinks below threshold
8. **ERROR #11** (valgrind): Fix or document valgrind limitations
9. **ERROR #12** (preload testing): Improve test script clarity
10. **ERROR #13** (README): Update documentation
11. **ERROR #14** (Makefile): Add early exit on test failure

---

## Summary Statistics

| Severity | Count | Status |
|----------|-------|--------|
| Critical | 4 | All tests abort |
| High | 4 | Heap corruption risk |
| Medium | 4 | Edge cases & testing |
| Low | 2 | Documentation |
| **Total** | **14** | |

---

## Testing Status

```
Build:        ✅ Compiles without warnings
Execution:    ❌ All 4 test suites abort on alignment check
Valgrind:     ⚠️  Results unreliable (Valgrind malloc interference)
Preload:      ❌ Real programs crash (exit code 139 = SIGSEGV)
Stress:       ❌ Not reached (blocked by alignment)
Correctness:  ❌ Not verified (blocked by alignment)
```

---

## Next Steps for You

1. **Confirm** you understand the alignment problem (simplest to diagnose)
2. **Fix alignment** first (allows tests to progress)
3. **Re-run tests** to see what other issues surface
4. **Fix the origin tracking** (ERROR #4) to prevent heap corruption
5. **Add validation** (ERROR #5, #6, #8) for robustness

All code locations are marked with file paths and line numbers for easy navigation.
