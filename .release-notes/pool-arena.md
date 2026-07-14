## Replace the runtime allocator on Unix platforms

The runtime's pool allocator couldn't return freed memory to the operating system, reuse a large block on a thread that didn't free it, or re-carve memory from one size class for another. A program that passed large blocks between threads reserved fresh address space for every block it freed and grew without bound.

The Unix platforms now use a new allocator in which every piece of memory has an owning thread, following the design in [discussion #5735](https://github.com/ponylang/ponyc/discussions/5735). Memory comes from the operating system in large shared regions that threads carve into arenas; freed memory is reused across threads and size classes, and an emptied arena's physical memory goes back to the operating system while its address space is kept for reuse. Windows keeps the previous allocator until the new one's Windows support arrives.

Freed memory reaches its owning thread even when that thread's scheduler is suspended: the delivery wakes the sleeping thread just far enough to reclaim, without scheduling it any work. One cost is known: a message-passing microbenchmark runs at roughly 85% of the previous allocator's throughput.

The previous allocator stays available behind a new build option:

```bash
cmake --preset release -DPONY_USES=pool_classic
```

Building with `address_sanitizer`, `valgrind`, or `pooltrack` now requires pairing with `pool_classic` (or `pool_memalign` for AddressSanitizer), since none of them can observe the new allocator's memory; the build stops with an error saying so.
