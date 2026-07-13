## Add an experimental arena allocator behind use=pool_arena

The runtime's pool allocator cannot return freed memory to the operating system, reuse a large block on a thread that did not free it, or re-carve memory from one size class for another. A program that passes large blocks between threads reserves fresh address space for every block it frees and grows without bound.

`use=pool_arena` builds the runtime with a replacement allocator in which every region of memory has an owning thread, following the design in [discussion #5735](https://github.com/ponylang/ponyc/discussions/5735). Freed memory is reused across threads and size classes, and empty arenas go back to the operating system.

```bash
cmake --preset release -DPONY_USES=pool_arena
cmake --build --preset release
```

The backend is experimental and off by default, and the design's later steps are still to come: memory freed for another thread is reclaimed the next time that thread allocates past its cache — a thread that stops allocating, suspended or idle, holds its freed memory until then — and there is no Windows support yet, so configuring with it on Windows is rejected with an error. Combining it with `address_sanitizer`, `valgrind`, or `pooltrack` is also rejected, since none of them can observe this allocator's memory (see BUILD.md).
