## Replace the runtime allocator on Unix platforms

The runtime's pool allocator couldn't return freed memory to the operating system, reuse a large block on a thread that didn't free it, or re-carve memory from one size class for another. A program that passed large blocks between threads reserved fresh address space for every block it freed and grew without bound.

The Unix platforms now use a new allocator in which every region of memory has an owning thread, following the design in [discussion #5735](https://github.com/ponylang/ponyc/discussions/5735). Freed memory is reused across threads and size classes, and empty arenas go back to the operating system. Windows keeps the previous allocator until the new one's Windows support arrives.

Two of the design's pieces are still to come, and one cost is known: memory freed for a thread that stops allocating is reclaimed when that thread next allocates past its cache, and a message-passing microbenchmark runs at roughly 80% of the previous allocator's throughput, with tuning scheduled as the design's final step.

The previous allocator stays available behind a new build option:

```bash
cmake --preset release -DPONY_USES=pool_classic
```

Building with `address_sanitizer`, `valgrind`, or `pooltrack` now requires pairing with `pool_classic` (or `pool_memalign` for AddressSanitizer), since none of them can observe the new allocator's memory; the build stops with an error saying so.
