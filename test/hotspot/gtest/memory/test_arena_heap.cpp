/*
 * Copyright (c) 2026, IBM Corp.
 * Copyright (c) 2026, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */


#include "runtime/os.hpp"
#include "memory/arena/arenaBlock.hpp"
#include "memory/arena/arenaHeap.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/macros.hpp"
#include "utilities/ostream.hpp"

#include "unittest.hpp"

#include "test_arena_helpers.hpp"

//#define LOG_PLEASE
#include "testutils.hpp"

TEST_VM(Arena2_Heap, arena_heap_basics) {
  ArenaHeap heap;
  constexpr size_t slabsize = 4 * M;
  for (size_t blocksize = ArenaBlockHelpers::min_block_size;
       blocksize <= ArenaBlockHelpers::huge_block_granularity;
       blocksize = blocksize * 2) {

    if (!ArenaBlockHelpers::is_valid_standard_size_class(blocksize) &&
        blocksize != ArenaBlockHelpers::huge_block_granularity) {
      // We have some "holes" in the list of valid block sizes. Skip those.
      continue;
    }
    const const_address hwm1 = heap.hwm();
    const size_t granule_size = MAX2(os::vm_page_size(), blocksize);
    const unsigned num_blocks = (unsigned)(slabsize / blocksize);
    const const_address p = (const_address) heap.reserve_slab_memory(blocksize, granule_size, num_blocks);
    ASSERT_NE(p, nullptr);
    ASSERT_TRUE(is_aligned(p, granule_size));
    ASSERT_EQ(hwm1 + blocksize * num_blocks, heap.hwm());
  }
}

TEST_VM(Arena2_Heap, arena_metaheap_basics) {
  ArenaMetadataHeap heap;

  constexpr size_t sz1 = 65;
  const const_address p1 = heap.allocate(sz1);
  ASSERT_EQ(heap.base(), p1);
  ASSERT_EQ(heap.hwm(), heap.base() + sz1);

  heap.align_hwm_to_padded_size();
  constexpr size_t sz2 = 17;
  const const_address p2 = heap.allocate(sz2);
  ASSERT_LT(p1 + sz1, p2);
  ASSERT_TRUE(is_aligned(p2, DEFAULT_PADDING_SIZE));

  heap.align_hwm_to<void*>();
  constexpr size_t sz3 = 4;
  const const_address p3 = heap.allocate(sz3);
  ASSERT_LT(p2 + sz2, p3);
  ASSERT_TRUE(is_aligned(p3, alignof(void*)));

  heap.align_hwm_to_page_size();
  constexpr size_t sz4 = 1024;
  const const_address p4 = heap.allocate(sz4);
  ASSERT_LT(p3 + sz3, p4);
  ASSERT_TRUE(is_aligned(p4, os::vm_page_size()));

  heap.align_hwm_to_large_page_size();
  constexpr size_t sz5 = 23 * K;
  const const_address p5 = heap.allocate(sz5);
  ASSERT_LT(p4 + sz4, p5);
  ASSERT_TRUE(is_aligned(p5, ArenaHeapBase::large_page_size));
}

TEST_VM(Arena2_Heap, disclaim) {
  ArenaMetadataHeap heap;

  const size_t sz = os::vm_page_size();
  const address p = heap.allocate(sz * 2);
  memset(p, 'A', sz * 2);

  ASSERT_EQ(p[0], 'A');
  ASSERT_EQ(p[sz - 1], 'A');
  ASSERT_EQ(p[sz], 'A');
  ASSERT_EQ(p[sz + sz - 1], 'A');

  heap.discard_memory(p + sz, p + sz * 2);

  ASSERT_EQ(p[0], 'A');
  ASSERT_EQ(p[sz - 1], 'A');

  // After discard, content should be zero'd out
  ASSERT_EQ(p[sz], 0);
  ASSERT_EQ(p[sz + sz - 1], 0);

  // Write to it again to see it works after disclaim
  p[sz] = p[sz + sz - 1] = 'B';

}