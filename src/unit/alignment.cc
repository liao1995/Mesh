// -*- mode: c++; c-basic-offset: 2; indent-tabs-mode: nil -*-
// Copyright 2017 University of Massachusetts, Amherst

#include <stdalign.h>
#include <cstdint>
#include <cstdlib>

#include "gtest/gtest.h"

#include "common.h"
#include "internal.h"
#include "thread_local_heap.h"

using namespace mesh;

TEST(Alignment, NaturalAlignment) {
  auto heap = ThreadLocalHeap::GetHeap();

  for (size_t size = 0; size < 4096; size += 4) {
    for (size_t alignment = 2; alignment <= 4096; alignment *= 2) {
      bool logged = false;
      void **ptrs = reinterpret_cast<void **>(calloc(256, sizeof(void *)));
      for (size_t i = 0; i <= 256; i++) {
        void *ptr = heap->memalign(alignment, size);
        if (!logged) {
          size_t actual = heap->getSize(ptr);
          // debug("%10zu %10zu %10zu %10p\n", size, actual, alignment, ptr);
          logged = true;
        }
        const auto ptrval = reinterpret_cast<uintptr_t>(ptr);
        ASSERT_EQ(ptrval % alignment, 0);
        ptrs[i] = ptr;
      }
      for (size_t i = 0; i <= 256; i++) {
        heap->free(ptrs[i]);
      }
    }
  }

  heap->releaseAll();
  mesh::runtime().heap().flushAllBins();
}
