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

#define LOG_PLEASE
#include "testutils.hpp"
#include "threadHelper.inline.hpp"

namespace {
  struct Tester {
    const unsigned _max_cpus;
    ArenaHeap _arena_heap;
    ArenaMetadataHeap _metadata_heap;
    ArenaSlabGroup _slab_group;
    ArenaPerCoreDataHolder _holder;

    explicit Tester(unsigned max_cpus) :
      _max_cpus(max_cpus),
      _slab_group(&_arena_heap, &_metadata_heap),
      _holder(&_metadata_heap, &_slab_group, _max_cpus) {}

#ifdef ASSERT
    void verify() const {
      _arena_heap.verify();
      _metadata_heap.verify();
      _slab_group.verify();
      _holder.verify();
    }
#endif

    void layout_test() {
      ASSERT_GE(_holder.num_cpus(), 1u);
      if (_max_cpus == 0) {
        ASSERT_EQ(_holder.num_cpus(), (unsigned)os::processor_count());
      } else {
        ASSERT_LE(_holder.num_cpus(), _max_cpus);
        ASSERT_LE(_holder.num_cpus(), (unsigned)os::processor_count());
      }
      STATIC_ASSERT(sizeof(ArenaPerCoreDataHolder::Data) % DEFAULT_PADDING_SIZE == 0);
      STATIC_ASSERT(alignof(ArenaPerCoreDataHolder::Data) == DEFAULT_PADDING_SIZE);
      const ArenaPerCoreDataHolder::Data* const d = _holder.get_current_core_data();
      ASSERT_TRUE(is_aligned(d, DEFAULT_PADDING_SIZE));
      DEBUG_ONLY(verify();)
    }

    void stress_test() {
      ArenaPerCoreDataHolder& holder = _holder;
      auto stresser = [&holder] (Thread*, int) {
        for (int i = 0; i < 10000; i++) {
          constexpr  block_size_class_t szclass = ArenaBlockHelpers::szclass_32K;
          constexpr  size_t block_size = ArenaBlockHelpers::size_class_to_block_size(szclass);
          const_blockptr_t blocks[4];
          {
            ArenaPerCoreDataHolder::Data* const data1 = holder.get_current_core_data();
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
            ArenaPerCoreDataHolder::Data* const data2 = holder.get_current_core_data();
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

      LOG_HERE("Num CPUs : %u", _holder.num_cpus());
      LOG_HERE("Num CPUs used: %u", _holder.num_cpus_used());

    #ifdef ARENA_STATS
    #ifdef LOG_PLEASE
      _arena_heap.print_on(tty);
      _metadata_heap.print_on(tty);
      _slab_group.print_on(tty);
      _holder.print_on(tty);
    #endif
    #endif

      DEBUG_ONLY(verify();)
    }

  };
}


#define DEFINE_TESTS(num_cpus, testname) \
TEST_VM(Arena2_PerCore, test_ ## testname ## _ ## num_cpus) { \
  Tester tester(num_cpus); \
  tester. testname ## _test(); \
}

#define DEFINE_ALL_TESTS(num_cpus) \
  DEFINE_TESTS(num_cpus, layout ) \
  DEFINE_TESTS(num_cpus, stress )

DEFINE_ALL_TESTS(1)
DEFINE_ALL_TESTS(4)
DEFINE_ALL_TESTS(16)
// Default
DEFINE_ALL_TESTS(0)