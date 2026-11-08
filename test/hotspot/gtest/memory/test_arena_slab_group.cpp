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

#include "memory/allocation.hpp"
#include "memory/arena/arenaSlab.hpp"

#include "runtime/os.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/macros.hpp"

#include "unittest.hpp"

//#define LOG_PLEASE
#include "testutils.hpp"
#include "test_arena_helpers.hpp"



TEST_VM(Arena2_SlabGroup, default_group) {
  ArenaHeap arena_heap;
  ArenaMetadataHeap metadata_heap;
  ArenaSlabGroup group(&arena_heap, &metadata_heap);

  auto tester = [&group](block_size_class_t szclass) {
    if (ArenaBlockHelpers::is_valid_standard_size_class(szclass)) {

      const size_t block_size = ArenaBlockHelpers::size_class_to_block_size(szclass);
      ArenaSlab* const slab = group.slab_for_size_class(szclass);
      ASSERT_EQ(slab->block_size(), block_size);
      ASSERT_GE(slab->granule_size(), 64 * K); // default slabs use 64K for the lower bound of granule size since we have 64K paged platforms

      // Test-allocate a few blocks (should fit, and in theory not use memory, since we never touch the blocks)
      const_blockptr_t blocks [64];
      const unsigned num_allocated = slab->allocate_blocks(blocks, 64);
      ASSERT_EQ(num_allocated, 64u);

    } else if (ArenaBlockHelpers::is_valid_huge_size_class(szclass)) {
      // Test-allocate a huge block
      ArenaSlab* const slab = group.slab_for_size_class(szclass);
      const const_blockptr_t p = slab->allocate_huge_block(5 * ArenaBlockHelpers::huge_block_granularity);
      ASSERT_NE(p, nullptr);
    }
  };
  ArenaBlockHelpers::iterate_size_classes_backward(tester);

#ifdef ASSERT
  arena_heap.verify();
  metadata_heap.verify();
  group.verify();
#endif

}
