# Error Report for Custom malloc Project

Generated from the current working tree on 2026-05-25. I inspected the source,
Makefile, scripts, tests, README, and ran the build/test/preload/valgrind targets.
No source code was changed.

## Commands Run

```bash
make -B
make test
make valgrind
make preload
LD_PRELOAD=./mymalloc.so /usr/bin/cat /etc/hostname
LD_PRELOAD=./mymalloc.so /usr/bin/wc -l /etc/hostname
LD_PRELOAD=./mymalloc.so /usr/bin/find /tmp -maxdepth 1
LD_PRELOAD=./mymalloc.so ./tests/test_basic
```

## Current Runtime Failures

### 1. Allocated pointers are not 16-byte aligned

Severity: Critical

Observed failure:

```text
test_basic: tests/test_basic.cpp:33: void test_malloc_alignment(): Assertion `is_aligned(ptr, 16)' failed.
test_coalesce: tests/test_coalesce.cpp:27: void test_forward_coalesce(): Assertion `is_aligned(big, 16)' failed.
test_realloc: tests/test_realloc.cpp:29: void test_realloc_null_ptr(): Assertion `is_aligned(ptr, 16)' failed.
test_stress: tests/test_stress.cpp:49: void test_random_alloc_free(): Assertion `is_aligned(ptr, 16)' failed.
```

Relevant code:

- `src/allocator_internal.hpp:12-16`
- `src/allocator.cpp:16`
- `src/allocator.cpp:27`
- `src/utils.cpp:70-79`

Reason:

`BlockHeader` is probably 24 bytes on this platform:

```cpp
std::size_t size;   // 8 bytes
bool is_free;       // 1 byte + padding
BlockHeader *next;  // 8 bytes
```

The returned pointer is `block + 1`, so the user pointer begins immediately
after a 24-byte header. If `sbrk(0)` or `mmap()` returns a 16-byte aligned
address, adding 24 bytes gives an address that is only 8 mod 16, not 0 mod 16.

This means the allocator does not satisfy its own advertised 16-byte alignment
requirement. It can also break code that stores types needing 16-byte alignment.

### 2. Real programs crash under `LD_PRELOAD`

Severity: Critical

Observed failures:

```bash
LD_PRELOAD=./mymalloc.so /usr/bin/cat /etc/hostname
# exit code 139

LD_PRELOAD=./mymalloc.so /usr/bin/wc -l /etc/hostname
# exit code 139

make preload
# fails at the cat/wc area with Error 139
```

Relevant code:

- `scripts/preload_test.sh:101-104`
- allocator internals in `src/allocator.cpp` and `src/utils.cpp`

Reason:

Exit code 139 means segmentation fault. This is consistent with the allocator
not meeting ABI expectations such as alignment, and possibly with missing
production allocator behavior used by libc/coreutils. Some tools such as
`find` happened to run successfully, but `cat` and `wc` crashing means the
allocator is not yet safe as an `LD_PRELOAD` replacement.

### 3. `make test` fails all suites immediately

Severity: Critical

Observed failure:

```text
make: *** [Makefile:21: test] Error 134
```

All four test binaries abort because allocation alignment fails.

Relevant code:

- `Makefile:20-24`
- `tests/test_basic.cpp:33`
- `tests/test_coalesce.cpp:27`
- `tests/test_realloc.cpp:29`
- `tests/test_stress.cpp:49`

Primary cause:

Same as issue 1: user pointers returned from `malloc()` are not 16-byte aligned.

## Allocator Logic Errors and Risks

### 4. `free()` decides `munmap` vs `sbrk` only from block size

Severity: Critical

Relevant code:

- `src/allocator.cpp:36-39`
- `src/allocator.cpp:44-57`
- `src/utils.cpp:37-48`

Problem:

```cpp
if (block->size >= MMAP_THRESHOLD) {
    munmap(block, BLOCK_OVERHEAD + block->size);
    return;
}
```

This assumes every block with `size >= MMAP_THRESHOLD` came from `mmap()`.
That is not always true.

Ways this can become wrong:

- Two or more adjacent `sbrk` blocks below the threshold can coalesce into a
  free block whose total size is above the threshold.
- `find_free_block()` searches the `sbrk` free list before checking the mmap
  threshold, so a large request may be satisfied from a coalesced `sbrk` block.
- When that reused `sbrk` block is later freed, `free()` may call `munmap()` on
  memory that actually belongs to the program break.

Expected invariant:

The allocator needs a reliable way to know whether a block came from `sbrk` or
`mmap`. Size alone is not enough once coalescing exists.

### 5. `get_prev()` can read invalid memory if called on non-list or corrupted blocks

Severity: High

Relevant code:

- `src/utils.cpp:13-23`
- `src/allocator.cpp:52`

Problem:

`get_prev()` blindly reads the footer immediately before `block`:

```cpp
BlockFooter *prev_footer =
    reinterpret_cast<BlockFooter *>(reinterpret_cast<char *>(block) - FOOTER_SIZE);
```

This is only valid if:

- `block` is an `sbrk` block in the allocator's contiguous heap region.
- The previous physical block has a valid footer.
- The block is not the first heap block.

The `block == heap_start` guard handles only the first block. It does not verify
that the computed previous header is inside the allocator's heap, belongs to the
linked list, or has a sane size. A bad pointer passed to `free()`, a double free
after metadata damage, or an incorrect mmap/sbrk classification can make this
read arbitrary memory.

For a learning allocator this may be acceptable, but it is still an important
error surface to understand.

### 6. Coalescing does not update all links in all cases

Severity: High

Relevant code:

- `src/allocator.cpp:44-57`

Problem:

Forward coalescing updates `block->next`, and backward coalescing updates
`prev->next`. That handles the local pair, but there is no explicit cleanup of
the absorbed block's metadata, and no validation that `block->next` really is
physically adjacent before merging.

Current code assumes that `next` always means "physically next block". That is
currently true for the simple `sbrk` list, but it is a fragile invariant. If the
list ever changes to store only free blocks, sort differently, or include other
metadata nodes, this coalescing would merge non-adjacent memory.

### 7. `align16()` can overflow for very large sizes

Severity: Medium

Relevant code:

- `src/utils.cpp:31-34`
- `src/allocator.cpp:7`
- `src/allocator.cpp:87`

Problem:

```cpp
return (size + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
```

If `size` is close to `SIZE_MAX`, `size + 15` wraps around to a small number.
That can lead to allocating much less memory than requested.

`calloc()` checks multiplication overflow before calling `malloc()`, but plain
`malloc()` and `realloc()` do not check alignment overflow.

### 8. `request_sbrk()` can overflow when adding overhead

Severity: Medium

Relevant code:

- `src/utils.cpp:73`

Problem:

```cpp
sbrk(static_cast<intptr_t>(BLOCK_OVERHEAD + size))
```

`BLOCK_OVERHEAD + size` can overflow as `std::size_t`. It is then cast to
`intptr_t`, which can also produce an invalid negative or truncated increment.

This matters for huge allocation requests.

### 9. `realloc()` does not split blocks when shrinking

Severity: Medium

Relevant code:

- `src/allocator.cpp:89-90`

Problem:

When the current block is already large enough, `realloc()` returns the same
pointer without splitting:

```cpp
if (block->size >= aligned_size)
    return ptr;
```

This is correct for data preservation, but it means shrinking a large block
does not return the unused tail to the free list. It can cause avoidable memory
waste and fragmentation. Your tests currently expect same-pointer shrink
behavior, but the allocator can still improve by splitting when the leftover is
large enough.

### 10. No double-free detection

Severity: Medium

Relevant code:

- `src/allocator.cpp:30-58`

Problem:

Calling `free()` twice on the same pointer will mark an already free block free
again and may coalesce using stale metadata. Production `free()` has undefined
behavior for double free, so this is not a standards violation by itself, but
for a custom allocator project it is a major debugging hazard.

## Test and Tooling Issues

### 11. Valgrind results are distorted by Valgrind's malloc replacement

Severity: High

Relevant code:

- `Makefile:31-36`
- `scripts/valgrind_check.sh:86-88`

Observed behavior:

Under `make valgrind`, `test_basic` failed at `malloc(0)` even though your
implementation returns `nullptr` for `malloc(0)`. Valgrind also printed calls
such as:

```text
at 0x4850858: malloc (vg_replace_malloc.c:447)
at 0x485807F: realloc (vg_replace_malloc.c:1804)
```

This means Valgrind's replacement allocator is intercepting allocations, so the
tests are not consistently exercising `mymalloc.so`.

Impact:

`make valgrind` is not a reliable check of your allocator behavior in its
current form. Some failures come from Valgrind's allocator semantics, not your
code. Some passes may also be Valgrind testing itself rather than your code.

### 12. `make valgrind` may appear cleaner than it really is

Severity: Medium

Relevant code:

- `Makefile:33-36`

Problem:

The `valgrind` target runs a shell `for` loop without an explicit failure
accumulator. In my run, earlier tests aborted, but the loop continued and the
last stress test completed successfully under Valgrind. Depending on shell and
final command behavior, this can hide earlier failures.

### 13. `preload_test.sh` uses `eval` and command strings

Severity: Medium

Relevant code:

- `scripts/preload_test.sh:55-75`
- `scripts/preload_test.sh:101-121`
- `scripts/preload_test.sh:146-160`

Problem:

`run_with_preload()` takes a string and runs:

```bash
output=$(LD_PRELOAD="$LIB" eval "$cmd" 2>&1)
```

This makes quoting and shell behavior part of the test. It is useful for quick
testing pipelines, but it can blur whether a failure came from the target
program, the shell, command substitution, or the allocator.

For allocator diagnostics, direct command execution is clearer.

### 14. README is out of date

Severity: Low

Relevant code/docs:

- `README.md:14-16`
- `README.md:49`
- `README.md:53-54`
- `src/allocator.cpp:51-57`
- `src/allocator_internal.hpp:18-20`

Problem:

The README says coalescing is forward-only and says backward coalescing would
require boundary tags. The current code already has footers and backward
coalescing.

Impact:

The documentation no longer matches the implementation, which makes debugging
and future changes harder.

## Priority Order I Would Fix

1. Fix the alignment invariant first. Until returned pointers are 16-byte
   aligned, most other test results are noisy.
2. Add a reliable block-origin flag or equivalent metadata so `free()` can tell
   `sbrk` blocks from `mmap` blocks without using size.
3. Re-check coalescing after the origin fix, especially coalesced `sbrk` blocks
   whose size crosses `MMAP_THRESHOLD`.
4. Rework the Valgrind target so it actually tests this allocator, or document
   that Valgrind's replacement malloc changes behavior.
5. After correctness, improve shrink `realloc()`, overflow handling, and
   diagnostic checks such as double-free detection.

