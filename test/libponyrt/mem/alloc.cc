#include <platform.h>

#include <mem/alloc.h>

#include <gtest/gtest.h>

#include <string.h>

// The aligned-reservation primitives don't exist on Windows until the arena
// allocator's Windows support lands; see alloc.h.
#ifndef PLATFORM_IS_WINDOWS

TEST(Alloc, ReserveAlignedIsAlignedAndDisjoint)
{
  // The alignment and the trim arithmetic are the code under test: a
  // reservation's address must be a multiple of its size, and repeated
  // reservations must hand out non-overlapping ranges.
  static const size_t sizes[] =
    {64 * 1024, 1024 * 1024, 4 * 1024 * 1024};
  static const int reps = 4;

  void* got[sizeof(sizes) / sizeof(sizes[0]) * reps];
  size_t got_size[sizeof(sizes) / sizeof(sizes[0]) * reps];
  int n = 0;

  for(size_t si = 0; si < (sizeof(sizes) / sizeof(sizes[0])); si++)
  {
    for(int r = 0; r < reps; r++)
    {
      void* p = ponyint_virt_reserve_aligned(sizes[si]);
      ASSERT_NE(p, (void*)NULL);
      ASSERT_EQ(((uintptr_t)p & (sizes[si] - 1)), (uintptr_t)0);

      for(int i = 0; i < n; i++)
      {
        bool disjoint = (((char*)p + sizes[si]) <= (char*)got[i]) ||
          (((char*)got[i] + got_size[i]) <= (char*)p);
        ASSERT_TRUE(disjoint);
      }

      got[n] = p;
      got_size[n] = sizes[si];
      n++;
    }
  }

  for(int i = 0; i < n; i++)
    ponyint_virt_free(got[i], got_size[i]);
}

TEST(Alloc, ReserveAlignedWholeRangeUsable)
{
  // If the trim unmapped the wrong side, part of the returned span is gone
  // and touching it faults. Write and read back at the edges and in the
  // middle of the whole reservation.
  size_t size = 1024 * 1024;
  char* p = (char*)ponyint_virt_reserve_aligned(size);
  ASSERT_NE(p, (char*)NULL);

  ponyint_virt_commit(p, size);

  p[0] = 'a';
  p[size / 2] = 'b';
  p[size - 1] = 'c';

  ASSERT_EQ(p[0], 'a');
  ASSERT_EQ(p[size / 2], 'b');
  ASSERT_EQ(p[size - 1], 'c');

  ponyint_virt_free(p, size);
}

TEST(Alloc, ReserveAlignedDecommitThenRewrite)
{
  // A decommitted range must stay mapped and writable: the arena allocator
  // decommits an emptied slab's pages and later reuses the same units.
  // Content across a decommit is not asserted -- MADV_DONTNEED and friends
  // make no promise about it.
  size_t size = 1024 * 1024;
  char* p = (char*)ponyint_virt_reserve_aligned(size);
  ASSERT_NE(p, (char*)NULL);

  ponyint_virt_commit(p, size);
  memset(p, 0x5a, size);

  ponyint_virt_decommit(p, size);

  ponyint_virt_commit(p, size);
  p[0] = 'x';
  p[size - 1] = 'y';
  ASSERT_EQ(p[0], 'x');
  ASSERT_EQ(p[size - 1], 'y');

  ponyint_virt_free(p, size);
}

#endif
