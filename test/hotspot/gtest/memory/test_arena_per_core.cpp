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
#include "memory/arena/arenaBlockCache.hpp"
#include "memory/arena/arenaSlab.hpp"
#include "memory/arena/arenaPerCore.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/macros.hpp"
#include "utilities/ostream.hpp"

#include "unittest.hpp"

#include "test_arena_helpers.hpp"

//#define LOG_PLEASE
#include "testutils.hpp"
#include "threadHelper.inline.hpp"

struct PerCoreFixture {
  ArenaHeap arena_heap;
  ArenaMetadataHeap metadata_heap;
  ArenaSlabGroup slab_group;
  ArenaPerCoreDataHolder holder;
  PerCoreFixture() :
    slab_group(&arena_heap, &metadata_heap),
    holder(&metadata_heap, &slab_group) {}
#ifdef ASSERT
  void verify() const {
    arena_heap.verify();
    metadata_heap.verify();
    slab_group.verify();
    holder.verify();
  }
#endif
};

// Every slot is aligned and padded apart; the holder has one slot per possible CPU id.
TEST_VM(Arena2_PerCore, layout) {
  PerCoreFixture f;
  ASSERT_EQ(f.holder.num_cpus(), (unsigned)os::processor_count());
  STATIC_ASSERT(sizeof(ArenaPerCoreDataHolder::Data) % DEFAULT_PADDING_SIZE == 0);
  STATIC_ASSERT(alignof(ArenaPerCoreDataHolder::Data) == DEFAULT_PADDING_SIZE);
  const ArenaPerCoreDataHolder::Data* const d = f.holder.get_current_core_data();
  ASSERT_TRUE(is_aligned(d, DEFAULT_PADDING_SIZE));
  DEBUG_ONLY(f.verify();)
}


TEST_VM(Arena2_PerCore, stress) {
  PerCoreFixture f;
  auto stresser = [&f] (Thread*, int) {
    for (int i = 0; i < 10000; i++) {
      constexpr  block_size_class_t szclass = ArenaBlockHelpers::szclass_32K;
      constexpr  size_t block_size = ArenaBlockHelpers::size_class_to_block_size(szclass);
      const_blockptr_t blocks[4];
      {
        ArenaPerCoreDataHolder::Data* const data1 = f.holder.get_current_core_data();
        ASSERT_TRUE(is_aligned(data1, DEFAULT_PADDING_SIZE));
        {
          const ArenaMutexLocker ml(data1->_lock);
          ArenaBlockCache* const c = data1->_cache_group.cache_for_size_class(szclass);
          for (int j = 0; j < 4; j++) {
            blocks[j] = (blockptr_t)c->allocate_block();
            ASSERT_NE(blocks[j], nullptr);
            ASSERT_TRUE(is_aligned(blocks[j], block_size));
          }
        }
      }
      // Touch outside the lock: a block handed out twice would show up as a
      // corrupted pattern (or as an assert in the cache/slab on free).
      for (int j = 0; j < 4; j++) {
        uintptr_t* const p = (uintptr_t*) blocks[j];
        (*p) = p2u(blocks + j); // Unique per thread and per slot. To catch the cache returning a block twice.
      }
      os::naked_yield();
      // Nobody should have touched our blocks.
      for (int j = 0; j < 4; j++) {
        const uintptr_t* const p = (const uintptr_t*) blocks[j];
        ASSERT_EQ(*p, p2u(blocks + j));
      }

      // Free may happen on a different slot than we allocated from (migration, cross-core free).
      {
        ArenaPerCoreDataHolder::Data* const data2 = f.holder.get_current_core_data();
        ArenaMutexLocker ml(data2->_lock);
        ArenaBlockCache* const c = data2->_cache_group.cache_for_size_class(szclass);
        c->bulk_deallocate_block(blocks, 4);
      }
    }
  };
  const unsigned num_threads = os::processor_count() * 2;
  // we should not exhaust the slab. The worst allocation load can be 4 * number-of-stressers
  // if all allocations from all stressers synchronize.
  // Lets keep that number well below the max number of 32k blocks.
  constexpr unsigned max_num_threads = 1024;
  TestThreadGroup ttg(stresser, MIN2(max_num_threads, num_threads));
  ttg.doit();
  ttg.join();
  DEBUG_ONLY(f.verify();)
}
