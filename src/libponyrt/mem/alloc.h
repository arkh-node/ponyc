#ifndef mem_alloc_h
#define mem_alloc_h

#include <stddef.h>

#include <platform.h>

PONY_EXTERN_C_BEGIN

/**
 * Allocates memory in the virtual address space.
 */
void* ponyint_virt_alloc(size_t bytes);

#ifndef PLATFORM_IS_WINDOWS
/**
 * Reserves bytes of address space aligned to its own size. The size must be
 * a power of two that is a multiple of the page size. The reservation is
 * lazily committed: pages consume physical memory only once touched.
 * Returns NULL on failure where ponyint_virt_alloc aborts: an allocator
 * holding freed memory in reserve can respond to exhaustion by giving some
 * back and retrying, which an abort inside the primitive would forbid.
 */
void* ponyint_virt_reserve_aligned(size_t size);

/**
 * Makes a range of a reservation from ponyint_virt_reserve_aligned usable.
 * On POSIX this is a no-op — reserved pages commit on first touch — but
 * callers mark their commit points with it so that platforms with explicit
 * commit (Windows) have a place to perform it.
 */
void ponyint_virt_commit(void* p, size_t bytes);
#endif

/**
 * Deallocates a chunk of memory that was previously allocated with
 * ponyint_virt_alloc or ponyint_virt_reserve_aligned.
 */
void ponyint_virt_free(void* p, size_t bytes);

/**
 * Advises the OS that the given memory range is no longer needed, allowing
 * it to reclaim the physical pages. The virtual address range remains valid
 * and accessible — pages are transparently faulted back in on next access.
 * Silently continues on failure since this is a memory optimization, not a
 * correctness requirement.
 */
void ponyint_virt_decommit(void* p, size_t bytes);

/**
 * Returns the system page size in bytes. The value is cached after the first
 * call.
 */
size_t ponyint_page_size();

PONY_EXTERN_C_END

#endif
