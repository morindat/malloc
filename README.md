# malloc

A custom memory allocator implemented in C++17, replacing `malloc`, `free`,
`calloc`, and `realloc` with a from-scratch implementation using `sbrk` for
small allocations and `mmap` for large ones.

## How it works

Every allocation is preceded by a block header stored just before the pointer
returned to the caller. The header tracks the block size, whether it is free,
and a pointer to the next block — forming an intrusive singly linked list
across the entire heap.

**Small allocations (< 128 KB)** extend the heap via `sbrk`. Freed blocks are
marked and coalesced forward with adjacent free neighbours to prevent
fragmentation. New allocations walk the free list using first-fit search before
requesting memory from the OS.

**Large allocations (≥ 128 KB)** are served via `mmap(MAP_ANONYMOUS)` and
returned directly to the OS on `free` via `munmap`, bypassing the free list
entirely.

## Build

```bash
make          # builds mymalloc.so
make test     # builds and runs all test binaries
make bench    # builds and runs benchmarks
make valgrind # runs all tests under valgrind
make preload  # injects allocator into real programs via LD_PRELOAD
make clean    # removes all build artifacts
```

## Requirements

- Linux (uses `sbrk` and `mmap`)
- g++ with C++17 support
- valgrind (optional, for `make valgrind`)

## Design decisions

| Decision | Choice | Reason |
|---|---|---|
| `malloc(0)` | returns `nullptr` | unambiguous, mirrors glibc strict mode |
| Alignment | 16 bytes | satisfies all ABI alignment requirements |
| Search strategy | first-fit | simpler, exits early, good practical performance |
| Split threshold | header + 16 bytes | avoids creating unusable slivers |
| sbrk/mmap threshold | 128 KB | mirrors glibc `MMAP_THRESHOLD` |
| Coalescing | forward and backward | singly linked list; backward implemented with boundary tags |

## Limitations

- **Not thread safe** — no locking around the free list walk
- **No size classes** — one free list for all sizes; glibc uses per-size bins
- **First-fit only** — no best-fit or segregated fit strategies