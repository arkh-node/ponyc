#define PONY_WANT_ATOMIC_DEFS

#include "pool.h"
#include "alloc.h"
#include "ponyassert.h"
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <platform.h>
#include <pony/detail/atomics.h>

#ifdef POOL_USE_ARENA

/* An allocator in which every region of memory has an owner, so that freed
 * memory can be merged, reused across threads, and returned to the operating
 * system. The design is laid out in ponylang/ponyc discussion #5735.
 *
 * Memory comes from the operating system in arenas: single mappings whose
 * starting address is a multiple of their size, owned by the thread that
 * reserved them. An arena is divided into units; a slab is a run of units
 * all serving one size class. Masking a freed pointer's low bits yields its
 * arena, and the arena's header holds all bookkeeping: a free/used bit per
 * unit and a record per unit. Nothing sits in front of an object, and no
 * allocator metadata lives in freed object memory except the free-list link
 * in the object's first word.
 *
 * A span of free units is a span of zero bits in the bitmap, so freeing
 * never needs to merge blocks: adjacent free spans are already one span.
 */

/// Arena size. Must be a power of two; an arena is aligned to its size.
/// The geometry here (arena size, unit size, sweep threshold) is mirrored
/// by the PoolArena tests' TEST_* constants; change both together.
#ifdef PLATFORM_IS_ILP32
#  define ARENA_SIZE ((size_t)16 * 1024 * 1024)
#else
#  define ARENA_SIZE ((size_t)128 * 1024 * 1024)
#endif

#define ARENA_MASK (ARENA_SIZE - 1)

/// Unit size: the granularity of slabs and of returning memory.
#define UNIT_BITS 14
#define UNIT_SIZE ((size_t)1 << UNIT_BITS)
#define ARENA_UNITS (ARENA_SIZE / UNIT_SIZE)
#define BITMAP_WORDS (ARENA_UNITS / 64)

/// Per-unit record states.
#define UNIT_STATE_FREE 0
/// The first unit of a slab; holds the slab's bookkeeping.
#define UNIT_STATE_HEAD 1
/// A later unit of a multi-unit slab; head_offset points back.
#define UNIT_STATE_CONT 2

/// size_class value for a slab holding one block of arbitrary adjusted size
/// rather than a size-class object.
#define SLAB_CLASS_BLOCK 0xFF

/// Mapping kinds, for the header a free finds by masking.
#define MAPPING_ARENA 1
#define MAPPING_OVERSIZED 2

/// Debug builds write this canary into the first word of each unit of a
/// released slab and, when re-reserving a unit whose pages were never
/// dropped in between, assert the word still holds it. A program that
/// wrote to a freed unit's first word in that window fails the assert;
/// writes elsewhere in the unit are not caught. (The window ends at a
/// sweep because a dropped page's refault content is undefined.)
#define POISON ((uint64_t)0xDEADFA11DEADFA11)

typedef struct pool_item_t
{
  struct pool_item_t* next;
} pool_item_t;

/* Cross-thread frees do not touch the owning thread's bookkeeping. The
 * freeing thread accumulates foreign objects in per-owner chains; at
 * BATCH_SIZE, or when the thread hands its pending work over before it
 * exits, it sorts the chain into runs — one run per unit, which is one run
 * per slab, since multi-unit slabs hold a single object at their base —
 * and pushes the linked runs onto the owner's inbox with one atomic
 * compare-and-swap. The owner takes the whole inbox with one atomic
 * exchange on its allocation slow path and credits each run to its slab.
 *
 * A run's objects are linked through their first words, except the last
 * object, which carries the run's header instead: the next run, the run's
 * first object, and its length. (The design discussion packs a u16
 * position instead of the first-object pointer to fit a 16-byte minimum
 * object; this allocator's minimum is 32 bytes, and carrying the pointer
 * means the freeing thread never reads a slab record at all.)
 *
 * Owners are slots in fixed global arrays rather than pointers to
 * thread-local state: a producer may push to an owner whose thread has
 * already exited, and a slot outlives its thread where thread-local
 * storage does not. Slots are never reused, because reuse would need
 * proof that no other thread still holds the slot in a chain or is about
 * to push to its inbox. The cost is a hard cap: a process gets MAX_OWNERS
 * distinct allocator-using threads over its whole life, and the next one
 * aborts. The runtime's own threads are far below the cap; an embedding
 * that churns short-lived allocator-using threads is the case that could
 * reach it.
 */

#define BATCH_SIZE 32
#define MAX_OWNERS 1024
#define NO_OWNER_SLOT UINT32_MAX

typedef struct run_header_t
{
  struct run_header_t* next_run;
  pool_item_t* first;
  uint16_t len;
} run_header_t;

typedef struct chain_t
{
  pool_item_t* head;
  uint32_t count;
} chain_t;

typedef struct inbox_t
{
  alignas(64) PONY_ATOMIC(run_header_t*) head;
} inbox_t;

static inbox_t inboxes[MAX_OWNERS];
static PONY_ATOMIC(uint32_t) next_owner_slot;

typedef struct arena_unit_t
{
  /// Freed objects of this slab, linked through their first word.
  pool_item_t* free_list;
  /// Doubly-linked per-class list of slabs with space, threaded through the
  /// owning thread's partial_slabs heads.
  struct arena_unit_t* next_partial;
  struct arena_unit_t* prev_partial;
  /// Next never-allocated object index.
  uint16_t bump;
  /// Live objects in the slab. An object counts as live from allocation
  /// until its free is applied by the owner.
  uint16_t live;
  /// Units in the slab. Set on the head record.
  uint16_t span;
  /// For UNIT_STATE_CONT: units back to the slab head.
  uint16_t head_offset;
  uint8_t size_class;
  uint8_t state;
  /// The slab is on its class's partial list.
  uint8_t on_partial;
} arena_unit_t;

struct pool_arena_thread_t;

/// A released slab this many units or larger has its pages dropped at once;
/// smaller ones go dirty and are swept in batches, so that churning small
/// slabs does not pay a system call per slab.
#define DECOMMIT_IMMEDIATE_SPAN 16

/// Sweep an arena's dirty units once this many accumulate: a quarter of the
/// arena (32 MiB on 64-bit). Below that, free units keep their pages as a
/// reuse cache; dropping them eagerly makes a churning workload re-fault
/// the pages back in, which costs more than the memory is worth.
#define DIRTY_SWEEP_THRESHOLD (ARENA_UNITS / 4)

/// The header at the start of every arena. Only the owner writes any of it.
/// The first cache line holds only the fields every foreign free reads and
/// nothing the owner writes after creation, so owner writes at slab cadence
/// do not knock the line out of the freeing threads' caches.
typedef struct arena_t
{
  uint8_t kind;
  /// The owning thread's slot. Never changes for the arena's lifetime, so
  /// a freeing thread may read it plainly: it holds a live object in the
  /// arena, and a mapped arena's owner is fixed.
  uint32_t owner_slot;
  /// Owner-written fields start on their own cache line.
  alignas(64) struct arena_t* next;
  struct arena_t* prev;
  /// Units not free, including the header's own units.
  uint32_t used_units;
  /// Scan hint: no free unit exists below this index.
  uint32_t first_free;
  /// Upper bound on the longest free run of units. A failed span search
  /// tightens it to the longest run the scan crossed; a release raises it
  /// to the run it merged into. Never smaller than the true longest run, so
  /// the reserve guard can skip a fragmented arena without scanning it.
  uint32_t span_bound;
  /// Free units whose pages are still committed, not yet given back.
  uint32_t dirty_units;
  uint64_t bitmap[BITMAP_WORDS];
  /// A set bit is a free unit whose pages are still committed.
  uint64_t dirty[BITMAP_WORDS];
  arena_unit_t units[ARENA_UNITS];
} arena_t;

/// The header at the start of an oversized mapping: an allocation too large
/// to fit in an arena gets its own mapping, and its payload starts one unit
/// in so this header can identify it to ponyint_pool_free_size.
typedef struct oversized_t
{
  uint8_t kind;
  /// The whole mapping's reserved size.
  size_t reserved;
} oversized_t;

typedef struct pool_arena_thread_t
{
  bool init;
  uint32_t slot;
  /// Owned arenas.
  arena_t* arenas;
  /// The slab each size class is currently allocating from.
  arena_unit_t* class_slab[POOL_COUNT];
  /// Slabs with space, per class, excluding the current slab.
  arena_unit_t* partial_slabs[POOL_COUNT];
  /// Foreign objects not yet handed to their owners, one chain per owner.
  chain_t chains[MAX_OWNERS];
} pool_arena_thread_t;

static __pony_thread_local pool_arena_thread_t this_thread;

/// Load-bearing checks stay in release builds: the allocator unmaps memory
/// on the strength of this bookkeeping, so a corrupt count or list must
/// stop the program rather than corrupt memory.
#define ARENA_CHECK(EXPR) \
  { \
    if(!(EXPR)) \
    { \
      fprintf(stderr, "%s:%d: pool_arena check failed: %s\n", \
        __FILE__, __LINE__, #EXPR); \
      abort(); \
    } \
  }

static size_t class_size(size_t index)
{
  return (size_t)POOL_MIN << index;
}

/* Units per slab and objects per slab, one entry per size class. Tables
 * rather than computed: the capacity check runs on every allocation, and
 * the division it would need is a real divide on some compilers.
 */
#define CLASS_SPAN_AT(I) \
  ((((size_t)POOL_MIN << (I)) <= UNIT_SIZE) ? 1 : \
    (((size_t)POOL_MIN << (I)) >> UNIT_BITS))
#define CLASS_CAPACITY_AT(I) \
  ((((size_t)POOL_MIN << (I)) <= UNIT_SIZE) ? \
    (UNIT_SIZE / ((size_t)POOL_MIN << (I))) : 1)

pony_static_assert(POOL_COUNT == 16,
  "the class tables below enumerate exactly the arena's 16 size classes");

static const uint16_t class_spans[POOL_COUNT] =
{
  CLASS_SPAN_AT(0), CLASS_SPAN_AT(1), CLASS_SPAN_AT(2), CLASS_SPAN_AT(3),
  CLASS_SPAN_AT(4), CLASS_SPAN_AT(5), CLASS_SPAN_AT(6), CLASS_SPAN_AT(7),
  CLASS_SPAN_AT(8), CLASS_SPAN_AT(9), CLASS_SPAN_AT(10), CLASS_SPAN_AT(11),
  CLASS_SPAN_AT(12), CLASS_SPAN_AT(13), CLASS_SPAN_AT(14), CLASS_SPAN_AT(15)
};

static const uint16_t class_capacities[POOL_COUNT] =
{
  CLASS_CAPACITY_AT(0), CLASS_CAPACITY_AT(1), CLASS_CAPACITY_AT(2),
  CLASS_CAPACITY_AT(3), CLASS_CAPACITY_AT(4), CLASS_CAPACITY_AT(5),
  CLASS_CAPACITY_AT(6), CLASS_CAPACITY_AT(7), CLASS_CAPACITY_AT(8),
  CLASS_CAPACITY_AT(9), CLASS_CAPACITY_AT(10), CLASS_CAPACITY_AT(11),
  CLASS_CAPACITY_AT(12), CLASS_CAPACITY_AT(13), CLASS_CAPACITY_AT(14),
  CLASS_CAPACITY_AT(15)
};

static size_t class_span(size_t index)
{
  return class_spans[index];
}

static size_t class_capacity(size_t index)
{
  return class_capacities[index];
}

static size_t arena_header_units()
{
  return (sizeof(arena_t) + UNIT_SIZE - 1) >> UNIT_BITS;
}

/// The mapping header for a pointer, by masking. For arenas any interior
/// pointer works; for an oversized mapping larger than ARENA_SIZE only
/// pointers within the first ARENA_SIZE window mask back to the header,
/// which holds for the payload base every correct free passes.
static arena_t* arena_of(void* p)
{
  return (arena_t*)((uintptr_t)p & ~(uintptr_t)ARENA_MASK);
}

static size_t unit_index(arena_t* arena, void* p)
{
  return ((uintptr_t)p - (uintptr_t)arena) >> UNIT_BITS;
}

static char* unit_base(arena_t* arena, arena_unit_t* rec)
{
  size_t index = (size_t)(rec - arena->units);
  return (char*)arena + (index << UNIT_BITS);
}

static void bitmap_set(arena_t* arena, size_t from, size_t count)
{
  for(size_t i = from; i < (from + count); i++)
    arena->bitmap[i >> 6] |= ((uint64_t)1 << (i & 63));
}

static void bitmap_clear(arena_t* arena, size_t from, size_t count)
{
  for(size_t i = from; i < (from + count); i++)
    arena->bitmap[i >> 6] &= ~((uint64_t)1 << (i & 63));
}

static bool bitmap_is_set(arena_t* arena, size_t i)
{
  return (arena->bitmap[i >> 6] & ((uint64_t)1 << (i & 63))) != 0;
}

static void dirty_set(arena_t* arena, size_t from, size_t count)
{
  for(size_t i = from; i < (from + count); i++)
    arena->dirty[i >> 6] |= ((uint64_t)1 << (i & 63));

  arena->dirty_units += (uint32_t)count;
}

static void dirty_clear(arena_t* arena, size_t from, size_t count)
{
  for(size_t i = from; i < (from + count); i++)
  {
    uint64_t bit = (uint64_t)1 << (i & 63);

    if((arena->dirty[i >> 6] & bit) != 0)
    {
      arena->dirty[i >> 6] &= ~bit;
      arena->dirty_units--;
    }
  }
}

static bool dirty_is_set(arena_t* arena, size_t i)
{
  return (arena->dirty[i >> 6] & ((uint64_t)1 << (i & 63))) != 0;
}

/// Drops the pages behind every dirty unit, one run at a time.
static void dirty_sweep(arena_t* arena)
{
  size_t i = 0;

  while(i < ARENA_UNITS)
  {
    if(!dirty_is_set(arena, i))
    {
      i++;
      continue;
    }

    size_t start = i;

    while((i < ARENA_UNITS) && dirty_is_set(arena, i))
    {
      arena->dirty[i >> 6] &= ~((uint64_t)1 << (i & 63));
      i++;
    }

    ponyint_virt_decommit((char*)arena + (start << UNIT_BITS),
      (i - start) << UNIT_BITS);
  }

  arena->dirty_units = 0;
}

/// Finds a run of span free units, or ARENA_UNITS if none exists.
/// Single units are carved from the low end of the arena and longer spans
/// from the high end. One direction alone spoils the other's holes: a
/// freed block's span is the lowest free run, so ascending first-fit puts
/// the next small slab in the middle of it, and the span never serves a
/// block again. Keeping the two kinds at opposite ends preserves long
/// runs.
static size_t bitmap_find_span(arena_t* arena, size_t span)
{
  size_t run = 0;

  if(span == 1)
  {
    for(size_t i = arena->first_free; i < ARENA_UNITS; i++)
    {
      if(!bitmap_is_set(arena, i))
        return i;
    }

    return ARENA_UNITS;
  }

  size_t longest = 0;

  for(size_t i = ARENA_UNITS; i-- > 0;)
  {
    if(bitmap_is_set(arena, i))
    {
      run = 0;
    } else {
      run++;

      if(run > longest)
        longest = run;

      if(run == span)
        return i;
    }
  }

  // The scan covered every free run, so the bound becomes exact until
  // the next release grows a run.
  arena->span_bound = (uint32_t)longest;
  return ARENA_UNITS;
}

static void thread_init()
{
  if(this_thread.init)
    return;

  uint32_t slot = atomic_fetch_add_explicit(&next_owner_slot, 1,
    memory_order_relaxed);

  if(slot >= MAX_OWNERS)
  {
    fprintf(stderr, "pool_arena: more than %d threads used the allocator; "
      "slots are never reused\n", MAX_OWNERS);
    abort();
  }

  this_thread.slot = slot;
  this_thread.init = true;
}

static arena_t* arena_new()
{
  arena_t* arena = (arena_t*)ponyint_virt_reserve_aligned(ARENA_SIZE);

  if(arena == NULL)
  {
    perror("out of memory: ");
    fprintf(stderr, "(tried to reserve a " __zu " byte arena)\n", ARENA_SIZE);
    abort();
  }

  ponyint_virt_commit(arena, arena_header_units() << UNIT_BITS);

  arena->kind = MAPPING_ARENA;
  arena->owner_slot = this_thread.slot;
  arena->used_units = (uint32_t)arena_header_units();
  arena->first_free = (uint32_t)arena_header_units();
  arena->span_bound = (uint32_t)(ARENA_UNITS - arena_header_units());
  bitmap_set(arena, 0, arena_header_units());

  arena->prev = NULL;
  arena->next = this_thread.arenas;

  if(arena->next != NULL)
    arena->next->prev = arena;

  this_thread.arenas = arena;
  return arena;
}

static void arena_release(arena_t* arena)
{
  if(arena->prev != NULL)
    arena->prev->next = arena->next;
  else
    this_thread.arenas = arena->next;

  if(arena->next != NULL)
    arena->next->prev = arena->prev;

  ponyint_virt_free(arena, ARENA_SIZE);
}

/// Takes span units out of an owned arena and initializes the head record.
/// Returns NULL when no owned arena has the space.
static arena_unit_t* slab_reserve(size_t span, uint8_t size_class)
{
  arena_t* arena = this_thread.arenas;
  size_t start = ARENA_UNITS;

  while(arena != NULL)
  {
    // Two guards, both upper bounds: total free units, and the longest
    // free run. A fragmented arena fails the second without a scan.
    if(((ARENA_UNITS - arena->used_units) >= span) &&
      (arena->span_bound >= span))
    {
      start = bitmap_find_span(arena, span);

      if(start != ARENA_UNITS)
        break;
    }

    arena = arena->next;
  }

  if(arena == NULL)
    return NULL;

#ifndef PONY_NDEBUG
  for(size_t i = 0; i < span; i++)
  {
    if(dirty_is_set(arena, start + i))
    {
      pony_assert(*(uint64_t*)((char*)arena + ((start + i) << UNIT_BITS)) ==
        POISON);
    }
  }
#endif

  bitmap_set(arena, start, span);
  dirty_clear(arena, start, span);
  arena->used_units += (uint32_t)span;

  if(start == arena->first_free)
    arena->first_free = (uint32_t)(start + span);

  ponyint_virt_commit((char*)arena + (start << UNIT_BITS),
    span << UNIT_BITS);

  arena_unit_t* rec = &arena->units[start];
  rec->free_list = NULL;
  rec->next_partial = NULL;
  rec->prev_partial = NULL;
  rec->bump = 0;
  rec->live = 0;
  rec->span = (uint16_t)span;
  rec->head_offset = 0;
  rec->size_class = size_class;
  rec->state = UNIT_STATE_HEAD;
  rec->on_partial = 0;

  for(size_t i = 1; i < span; i++)
  {
    arena_unit_t* cont = &arena->units[start + i];
    cont->state = UNIT_STATE_CONT;
    cont->head_offset = (uint16_t)i;
    cont->size_class = size_class;
    cont->on_partial = 0;
  }

  return rec;
}

/// Returns a slab's units to its arena, and the arena to the operating
/// system when that empties it.
static void slab_release(arena_t* arena, arena_unit_t* rec)
{
  size_t start = (size_t)(rec - arena->units);
  size_t span = rec->span;

  rec->state = UNIT_STATE_FREE;

  for(size_t i = 1; i < span; i++)
    arena->units[start + i].state = UNIT_STATE_FREE;

  bitmap_clear(arena, start, span);
  arena->used_units -= (uint32_t)span;

  if(start < arena->first_free)
    arena->first_free = (uint32_t)start;

  // The released units and their free neighbors form one run; raise the
  // bound to it. The walk is proportional to the run it measures.
  {
    size_t lo = start;
    size_t hi = start + span;

    while((lo > 0) && !bitmap_is_set(arena, lo - 1))
      lo--;

    while((hi < ARENA_UNITS) && !bitmap_is_set(arena, hi))
      hi++;

    if((hi - lo) > arena->span_bound)
      arena->span_bound = (uint32_t)(hi - lo);
  }

  if(arena->used_units == arena_header_units())
  {
    // Keep one completely empty arena per thread in reserve, its pages
    // dropped but its address space held, so churn across the arena
    // boundary does not pay a map/unmap cycle each time. A second empty
    // arena goes back to the operating system.
    arena_t* other = this_thread.arenas;

    while(other != NULL)
    {
      if((other != arena) && (other->used_units == arena_header_units()))
        break;

      other = other->next;
    }

    if(other != NULL)
    {
      arena_release(arena);
      return;
    }

    dirty_sweep(arena);
    ponyint_virt_decommit(unit_base(arena, rec), span << UNIT_BITS);
    return;
  }

  if(span >= DECOMMIT_IMMEDIATE_SPAN)
  {
    ponyint_virt_decommit(unit_base(arena, rec), span << UNIT_BITS);
    return;
  }

#ifndef PONY_NDEBUG
  for(size_t i = 0; i < span; i++)
    *(uint64_t*)(unit_base(arena, rec) + (i << UNIT_BITS)) = POISON;
#endif

  dirty_set(arena, start, span);

  if(arena->dirty_units >= DIRTY_SWEEP_THRESHOLD)
    dirty_sweep(arena);
}

static void partial_push(arena_unit_t* rec)
{
  size_t index = rec->size_class;
  arena_unit_t* head = this_thread.partial_slabs[index];

  rec->next_partial = head;
  rec->prev_partial = NULL;

  if(head != NULL)
    head->prev_partial = rec;

  this_thread.partial_slabs[index] = rec;
  rec->on_partial = 1;
}

static void partial_remove(arena_unit_t* rec)
{
  size_t index = rec->size_class;

  if(rec->prev_partial != NULL)
    rec->prev_partial->next_partial = rec->next_partial;
  else
    this_thread.partial_slabs[index] = rec->next_partial;

  if(rec->next_partial != NULL)
    rec->next_partial->prev_partial = rec->prev_partial;

  rec->next_partial = NULL;
  rec->prev_partial = NULL;
  rec->on_partial = 0;
}

/// Pops an object from a slab known to have one, raising the live count.
static void* slab_get(arena_t* arena, arena_unit_t* rec, size_t index)
{
  size_t size = class_size(index);
  char* base = unit_base(arena, rec);
  void* p;

  pool_item_t* item = rec->free_list;

  if(item != NULL)
  {
    // A freed object's link word lives in memory the program owned, so
    // validate it before handing it out: it must point into this slab, at
    // an object boundary, or be the list's end.
    pool_item_t* next = item->next;
    ARENA_CHECK((next == NULL) ||
      (((uintptr_t)next >= (uintptr_t)base) &&
        ((uintptr_t)next < (uintptr_t)(base + (rec->span << UNIT_BITS))) &&
        ((((uintptr_t)next - (uintptr_t)base) & (size - 1)) == 0)));

    rec->free_list = next;
    p = item;
  } else {
    pony_assert(rec->bump < class_capacity(index));
    p = base + ((size_t)rec->bump * size);
    rec->bump++;
  }

  ARENA_CHECK(rec->live < class_capacity(index));
  rec->live++;
  ARENA_CHECK((((uintptr_t)p - (uintptr_t)base) & (size - 1)) == 0);
  return p;
}

static bool slab_has_space(arena_unit_t* rec, size_t index)
{
  return (rec->free_list != NULL) || (rec->bump < class_capacity(index));
}

/// The state transitions after a slab's live count fell: release or reset
/// an emptied slab, or put a newly-usable full slab on the partial list.
static void slab_after_free(arena_t* arena, arena_unit_t* rec, size_t index,
  bool was_full)
{
  bool current = (this_thread.class_slab[index] == rec);

  if(rec->live == 0)
  {
    if(current)
    {
      // Keep the hot slab: reset it in place rather than releasing and
      // immediately re-reserving it on the next allocation.
      rec->free_list = NULL;
      rec->bump = 0;
    } else {
      if(rec->on_partial)
        partial_remove(rec);

      slab_release(arena, rec);
    }
  } else if(was_full && !current) {
    partial_push(rec);
  }
}

static void chain_flush(uint32_t owner);

/// A free of another thread's object: hold it on that owner's chain, and
/// hand the chain over once it reaches a batch.
static void chain_push(uint32_t owner, void* p)
{
  chain_t* c = &this_thread.chains[owner];
  pool_item_t* item = (pool_item_t*)p;
  item->next = c->head;
  c->head = item;
  c->count++;

  if(c->count >= BATCH_SIZE)
    chain_flush(owner);
}

/// Sorts a chain into runs — one per unit, which is one per slab — and
/// pushes the linked runs onto the owner's inbox with one atomic
/// compare-and-swap, paying one atomic for the whole batch.
static void chain_flush(uint32_t owner)
{
  chain_t* c = &this_thread.chains[owner];

  if(c->head == NULL)
    return;

  // groups[] below is sized to a full batch and nothing more; the push
  // path flushes the moment a chain reaches BATCH_SIZE.
  ARENA_CHECK(c->count <= BATCH_SIZE);

  struct
  {
    uintptr_t unit_addr;
    pool_item_t* first;
    pool_item_t* tail;
    uint16_t len;
  } groups[BATCH_SIZE];

  size_t ngroups = 0;
  pool_item_t* next;

  for(pool_item_t* it = c->head; it != NULL; it = next)
  {
    next = it->next;
    uintptr_t unit_addr = (uintptr_t)it & ~((uintptr_t)UNIT_SIZE - 1);

    size_t g = 0;

    while((g < ngroups) && (groups[g].unit_addr != unit_addr))
      g++;

    if(g == ngroups)
    {
      // First object seen for this unit: it becomes the run's tail, the
      // object whose first word will carry the run's header.
      it->next = NULL;
      groups[g].unit_addr = unit_addr;
      groups[g].first = it;
      groups[g].tail = it;
      groups[g].len = 1;
      ngroups++;
    } else {
      it->next = groups[g].first;
      groups[g].first = it;
      groups[g].len++;
    }
  }

  c->head = NULL;
  c->count = 0;

  run_header_t* run_list = NULL;

  for(size_t g = 0; g < ngroups; g++)
  {
    run_header_t* h = (run_header_t*)groups[g].tail;
    h->next_run = run_list;
    h->first = groups[g].first;
    h->len = groups[g].len;
    run_list = h;
  }

  // The list was built backwards, so the first group's header is the
  // list's tail: the old inbox content hangs off it.
  run_header_t* list_tail = (run_header_t*)groups[0].tail;

  PONY_ATOMIC(run_header_t*)* head = &inboxes[owner].head;
  run_header_t* old = atomic_load_explicit(head, memory_order_relaxed);

  do
  {
    list_tail->next_run = old;
  } while(!atomic_compare_exchange_weak_explicit(head, &old, run_list,
    memory_order_release, memory_order_relaxed));
}

/// Credits one run of foreign-freed objects to its slab, after checking
/// that every object in the run belongs to the slab the run's tail
/// object sits in.
static void apply_run(run_header_t* h)
{
  pool_item_t* last = (pool_item_t*)h;
  pool_item_t* first = h->first;
  uint16_t len = h->len;

  arena_t* arena = arena_of(last);
  ARENA_CHECK(arena->kind == MAPPING_ARENA);
  ARENA_CHECK(arena->owner_slot == this_thread.slot);

  arena_unit_t* rec = &arena->units[unit_index(arena, last)];
  ARENA_CHECK(rec->state == UNIT_STATE_HEAD);

  char* base = unit_base(arena, rec);

  if(rec->size_class == SLAB_CLASS_BLOCK)
  {
    ARENA_CHECK(len == 1);
    ARENA_CHECK((void*)first == (void*)base);
    ARENA_CHECK(rec->live == 1);
    rec->live = 0;
    slab_release(arena, rec);
    return;
  }

  size_t index = rec->size_class;
  size_t size = class_size(index);
  size_t span_bytes = (size_t)rec->span << UNIT_BITS;

  pool_item_t* it = first;

  for(uint16_t i = 0; (i + 1) < len; i++)
  {
    ARENA_CHECK(((uintptr_t)it >= (uintptr_t)base) &&
      ((uintptr_t)it < (uintptr_t)(base + span_bytes)));
    ARENA_CHECK((((uintptr_t)it - (uintptr_t)base) & (size - 1)) == 0);
    it = it->next;
  }

  ARENA_CHECK(it == last);
  ARENA_CHECK(((uintptr_t)it >= (uintptr_t)base) &&
    ((uintptr_t)it < (uintptr_t)(base + span_bytes)));
  ARENA_CHECK((((uintptr_t)it - (uintptr_t)base) & (size - 1)) == 0);
  ARENA_CHECK(rec->live >= len);

  bool was_full = !slab_has_space(rec, index);

  // The header word becomes the free-list link, and the whole run goes on
  // in one splice.
  last->next = rec->free_list;
  rec->free_list = first;
  rec->live -= (uint16_t)len;

  slab_after_free(arena, rec, index, was_full);
}

/// Takes everything in this thread's inbox with one atomic exchange and
/// credits each run. Runs on the allocation slow path, so an allocating
/// thread reclaims what other threads freed for it before it reserves
/// anything new.
static void inbox_drain()
{
  run_header_t* h = atomic_exchange_explicit(
    &inboxes[this_thread.slot].head, NULL, memory_order_acquire);

  while(h != NULL)
  {
    // apply_run overwrites the header word, so read the link first.
    run_header_t* next = h->next_run;
    apply_run(h);
    h = next;
  }
}

void* ponyint_pool_alloc(size_t index)
{
  pony_assert(index < POOL_COUNT);
  thread_init();

  arena_unit_t* rec = this_thread.class_slab[index];

  if((rec != NULL) && slab_has_space(rec, index))
    return slab_get(arena_of(rec), rec, index);

  // The fast path is exhausted: reclaim what other threads freed for this
  // one before reserving anything new. The drain may have refilled the
  // current slab.
  inbox_drain();

  rec = this_thread.class_slab[index];

  if((rec != NULL) && slab_has_space(rec, index))
    return slab_get(arena_of(rec), rec, index);

  rec = this_thread.partial_slabs[index];

  if(rec != NULL)
  {
    partial_remove(rec);
    this_thread.class_slab[index] = rec;
    return slab_get(arena_of(rec), rec, index);
  }

  rec = slab_reserve(class_span(index), (uint8_t)index);

  if(rec == NULL)
  {
    arena_new();
    rec = slab_reserve(class_span(index), (uint8_t)index);
    ARENA_CHECK(rec != NULL);
  }

  this_thread.class_slab[index] = rec;
  return slab_get(arena_of(rec), rec, index);
}

void ponyint_pool_free(size_t index, void* p)
{
  pony_assert(index < POOL_COUNT);
  thread_init();

  arena_t* arena = arena_of(p);
  ARENA_CHECK(arena->kind == MAPPING_ARENA);

  if(arena->owner_slot != this_thread.slot)
  {
    // Another thread's memory goes home through its owner's inbox. Nothing
    // of the owner's bookkeeping is touched here: only the object's own
    // first word, this thread's chain, and (at a batch) the inbox head.
    chain_push(arena->owner_slot, p);
    return;
  }

  arena_unit_t* rec = &arena->units[unit_index(arena, p)];

  // No pointer the allocator returned lands in a continuation unit (a
  // multi-unit slab holds one object, at its base). The redirect is for
  // an interior pointer, so the misuse fails the head checks below
  // against the right record.
  if(rec->state == UNIT_STATE_CONT)
    rec -= rec->head_offset;

  size_t size = class_size(index);
  char* base = unit_base(arena, rec);

  ARENA_CHECK(rec->state == UNIT_STATE_HEAD);
  ARENA_CHECK(rec->size_class == index);
  ARENA_CHECK((((uintptr_t)p - (uintptr_t)base) & (size - 1)) == 0);
  ARENA_CHECK(rec->live > 0);

  bool was_full = !slab_has_space(rec, index);

  pool_item_t* item = (pool_item_t*)p;
  item->next = rec->free_list;
  rec->free_list = item;
  rec->live--;

  slab_after_free(arena, rec, index, was_full);
}

static void* pool_block_alloc(size_t adjusted)
{
  size_t span = (adjusted + UNIT_SIZE - 1) >> UNIT_BITS;

  thread_init();
  inbox_drain();

  arena_unit_t* rec = slab_reserve(span, SLAB_CLASS_BLOCK);

  if(rec == NULL)
  {
    arena_new();
    rec = slab_reserve(span, SLAB_CLASS_BLOCK);
    ARENA_CHECK(rec != NULL);
  }

  rec->live = 1;
  rec->bump = 1;

  char* p = unit_base(arena_of(rec), rec);
  ARENA_CHECK(((uintptr_t)p & (POOL_ALIGN - 1)) == 0);
  return p;
}

static void pool_block_free(void* p)
{
  arena_t* arena = arena_of(p);
  arena_unit_t* rec = &arena->units[unit_index(arena, p)];

  ARENA_CHECK(rec->state == UNIT_STATE_HEAD);
  ARENA_CHECK(rec->size_class == SLAB_CLASS_BLOCK);
  ARENA_CHECK(rec->live == 1);
  ARENA_CHECK((void*)unit_base(arena, rec) == p);

  rec->live = 0;
  slab_release(arena, rec);
}

/// The usable span of an arena, in bytes.
static size_t arena_capacity()
{
  return (ARENA_UNITS - arena_header_units()) << UNIT_BITS;
}

/// The next power of two at or above size. The caller keeps size at or
/// below SIZE_MAX/2; above that no power of two fits and the shift would
/// wrap forever.
static size_t next_pow2(size_t size)
{
  size_t p = 1;

  while(p < size)
    p <<= 1;

  return p;
}

/// An allocation too large to fit in an arena gets its own mapping. The
/// payload starts one unit in, so masking the payload pointer finds the
/// mapping's header, the same way it finds an arena.
static void* oversized_alloc(size_t adjusted)
{
  // Near SIZE_MAX the header room would wrap the sum and the power-of-two
  // round-up has nowhere to go; fail the way an unsatisfiable reservation
  // fails instead of mapping less than the caller asked for.
  if(adjusted > ((SIZE_MAX / 2) - UNIT_SIZE))
  {
    perror("out of memory: ");
    fprintf(stderr, "(tried to reserve " __zu " bytes)\n", adjusted);
    abort();
  }

  size_t reserved = next_pow2(adjusted + UNIT_SIZE);

  if(reserved < ARENA_SIZE)
    reserved = ARENA_SIZE;

  oversized_t* over = (oversized_t*)ponyint_virt_reserve_aligned(reserved);

  if(over == NULL)
  {
    perror("out of memory: ");
    fprintf(stderr, "(tried to reserve " __zu " bytes)\n", reserved);
    abort();
  }

  ponyint_virt_commit(over, sizeof(oversized_t));

  over->kind = MAPPING_OVERSIZED;
  over->reserved = reserved;

  char* p = (char*)over + UNIT_SIZE;
  ponyint_virt_commit(p, adjusted);
  ARENA_CHECK(((uintptr_t)p & (POOL_ALIGN - 1)) == 0);
  return p;
}

static void oversized_free(void* p)
{
  oversized_t* over = (oversized_t*)((uintptr_t)p - UNIT_SIZE);

  ARENA_CHECK(over->kind == MAPPING_OVERSIZED);
  ponyint_virt_free(over, over->reserved);
}

void* ponyint_pool_alloc_size(size_t size)
{
  size_t index = ponyint_pool_index(size);

  if(index < POOL_COUNT)
    return ponyint_pool_alloc(index);

  size_t adjusted = ponyint_pool_adjust_size(size);

  if(adjusted <= arena_capacity())
    return pool_block_alloc(adjusted);

  return oversized_alloc(adjusted);
}

void ponyint_pool_free_size(size_t size, void* p)
{
  size_t index = ponyint_pool_index(size);

  if(index < POOL_COUNT)
    return ponyint_pool_free(index, p);

  thread_init();

  arena_t* arena = arena_of(p);

  if(arena->kind == MAPPING_OVERSIZED)
  {
    // An oversized mapping has no bookkeeping shared with anything, so any
    // thread can return it to the operating system directly.
    oversized_free(p);
    return;
  }

  ARENA_CHECK(arena->kind == MAPPING_ARENA);

  if(arena->owner_slot != this_thread.slot)
  {
    chain_push(arena->owner_slot, p);
    return;
  }

  pool_block_free(p);
}

void* ponyint_pool_realloc_size(size_t old_size, size_t new_size, void* p)
{
  // Can only reuse the old pointer if the old index/adjusted size is equal to
  // the new one, not greater.

  if(p == NULL)
    return ponyint_pool_alloc_size(new_size);

  size_t old_index = ponyint_pool_index(old_size);
  size_t new_index = ponyint_pool_index(new_size);
  size_t old_adj_size = 0;

  // Computed for every path: the free at the end of the function needs it
  // whenever the old block was larger than POOL_MAX, whichever branch the
  // new size takes below.
  if(old_index >= POOL_COUNT)
    old_adj_size = ponyint_pool_adjust_size(old_size);

  void* new_p;

  if(new_index < POOL_COUNT)
  {
    if(old_index == new_index)
      return p;

    new_p = ponyint_pool_alloc(new_index);
  } else {
    size_t new_adj_size = ponyint_pool_adjust_size(new_size);

    if((old_index >= POOL_COUNT) && (old_adj_size == new_adj_size))
      return p;

    new_p = ponyint_pool_alloc_size(new_adj_size);
  }

  memcpy(new_p, p, old_size < new_size ? old_size : new_size);

  if(old_index < POOL_COUNT)
    ponyint_pool_free(old_index, p);
  else
    ponyint_pool_free_size(old_adj_size, p);

  return new_p;
}

void ponyint_pool_thread_cleanup()
{
  if(!this_thread.init)
    return;

  // Deliver every pending foreign free to its owner. Nothing else happens
  // at thread exit: the allocator leaves its own memory in place, because
  // threads exit in no fixed order and unmapping could hit memory another
  // thread still uses.
  uint32_t owners = atomic_load_explicit(&next_owner_slot,
    memory_order_relaxed);

  if(owners > MAX_OWNERS)
    owners = MAX_OWNERS;

  for(uint32_t slot = 0; slot < owners; slot++)
  {
    if(this_thread.chains[slot].head != NULL)
      chain_flush(slot);
  }
}

#endif
