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

namespace {
  class Tester {

    const size_t _block_size;
    const size_t _granule_size;
    const unsigned _num_blocks;
    const unsigned _release_boundary;
    const double _decay_factor;

    struct Range { address p; size_t sz; };
    ArenaHeap _heap;
    Range _metadata_space;
    ArenaSlab _slab;
    const BlockRange<unsigned> _block_range;
    CHeapBitMap _control;
    PredictableRandom _random;

    static Range allocate_metadata_space(size_t block_size, size_t granule_size, unsigned num_blocks) {
      // needs to be fully accessible
      const size_t len = align_up(ArenaSlab::calc_metadata_space(block_size, granule_size, num_blocks), os::vm_page_size());
      char* const p = os::reserve_memory(len, mtTest, false);
      assert(p != nullptr, "OOM");
      const bool b = os::commit_memory(p, len, false);
      return Range { (address) p, len };
    }

    void check_equality() const {
      EXPECT_EQ(_control.count_one_bits(), _slab.count_in_use_blocks());
      EXPECT_LE(_control.count_one_bits(), _slab.count_dirty_blocks());
    }

    unsigned allocate_blocks_and_verify(unsigned num) {

      // fake-allocate in control bitmap
      CHeapBitMap tmp(_num_blocks, mtTest, false);
      tmp.set_from(_control);
      BitMap::idx_t idx = 0;
      unsigned num_fake_allocated = 0;
      while (idx < _num_blocks && num_fake_allocated < num) {
        idx = tmp.find_first_clear_bit(idx, _num_blocks);
        if (idx < _num_blocks) {
          num_fake_allocated ++;
          tmp.set_bit(idx);
        }
      }

      // now the real thing
      const_blockptr_t* const arr = NEW_C_HEAP_ARRAY(const_blockptr_t, num, mtTest);
      const unsigned n = _slab.allocate_blocks(arr, num);
      EXPECT_EQ(n, num_fake_allocated);

      for (unsigned i = 0; i < n; ++i) {
        EXPECT_TRUE(_block_range.is_valid_block(arr[i]));
        EXPECT_TRUE(tmp.at(_block_range.block_to_index(arr[i])));
      }

      _control.set_union(tmp);

      // write into all the blocks to check they are writable
      for (unsigned i = 0; i < n; ++i) {
        const blockptr_t b = const_cast<blockptr_t>(arr[i]);
        os::pretouch_memory(b, b + _block_size, os::vm_page_size());
        b[0] = 17;
        b[_block_size - 1] = 18;
      }
      FREE_C_HEAP_ARRAY(arr);

      check_equality();
      DEBUG_ONLY(_slab.verify();)
      return n;
    }

    void deallocate_and_verify(BitMap::idx_t block_indexes[], unsigned num) {
      // check that the released blocks are in use
      CHeapBitMap tmp(_num_blocks, mtTest, true);
      for (unsigned i = 0; i < num; ++i) {
        tmp.set_bit(block_indexes[i]);
      }
      assert(_control.contains(tmp), "releasing not in-use blocks");

      // Update control
      _control.set_difference(tmp);

      // Do it for real now
      const_blockptr_t* const arr = NEW_C_HEAP_ARRAY(const_blockptr_t, num, mtTest);
      for (unsigned i = 0; i < num; ++i) {
        arr[i] = _block_range.index_to_block(block_indexes[i]);
      }
      _slab.deallocate_blocks(arr, num);
      FREE_C_HEAP_ARRAY(arr);

      check_equality();
      DEBUG_ONLY(_slab.verify();)
    }

    void allocate_fully() {
      // allocate to the brim and a bit beyond
      unsigned num_allocated = 0;
      while (num_allocated < _num_blocks) {
        const unsigned num_to_allocate = MAX2(_random.next(16), 1u);
        const unsigned n = allocate_blocks_and_verify(num_to_allocate);
        ASSERT_LE(n, num_to_allocate);
        ASSERT_GE(n, 1u);
        num_allocated += n;
      }
      ASSERT_EQ(_slab.count_in_use_blocks(), _num_blocks);
    }

  public:

    Tester(size_t block_size, size_t granule_size, unsigned num_blocks, unsigned release_boundary, double decay_factor) :
      _block_size(block_size), _granule_size(granule_size), _num_blocks(num_blocks),
      _release_boundary(release_boundary), _decay_factor(decay_factor),
      _heap(),
      _metadata_space(allocate_metadata_space(block_size, granule_size, num_blocks)),
      _slab(&_heap, _heap.base(), _heap.size(), _metadata_space.p, _metadata_space.sz,
            block_size, granule_size, num_blocks, release_boundary, decay_factor),
      _block_range(_heap.base(), block_size, num_blocks),
      _control(num_blocks, mtTest, true) {
    }

    ~Tester() {
      os::release_memory((char*)_metadata_space.p, _metadata_space.sz);
    }

    void test_allocate_and_deallocate() {
      allocate_fully();
      // a bit beyond, but slab is full
      ASSERT_EQ(allocate_blocks_and_verify(2), 0u);
      // Now deallocate.
      // Mode=0: forward, Mode=1 reverse, Mode=2 random
      int num_allocated = _num_blocks;
      BitMap::idx_t arr[16];
      while (num_allocated > 0) {
        int num_to_deallocate = MIN2(num_allocated, 1 + (int)_random.next(15));
        num_to_deallocate = ArenaTestHelpers::get_random_set_bits(_random, _control, arr, num_to_deallocate);
        deallocate_and_verify(arr, num_to_deallocate);
        num_allocated -= num_to_deallocate;
      }
    }

  };
}


#define CALL_TEST(XXX) test_##XXX()

#define DEFINE_TESTS(block_size, granule_size, num_blocks, release_boundary, decay_factor, testname) \
TEST_VM(Arena2_Slab, test_ ## testname ## _ ## block_size ## _ ## granule_size ## _ ## num_blocks ## _ ## release_boundary ## _ ## decay_factor) { \
  Tester tester(block_size, granule_size, num_blocks, release_boundary, decay_factor); \
  tester.CALL_TEST(testname); \
}

#define DEFINE_ALL_TESTS(block_size, granule_size, num_blocks, release_boundary, decay_factor) \
  DEFINE_TESTS(block_size, granule_size, num_blocks, release_boundary, decay_factor, allocate_and_deallocate )

// block_size, granule_size, num_blocks, release_boundary, decay_factor
DEFINE_ALL_TESTS(256, 16384, 65536, 0, 0)
DEFINE_ALL_TESTS(256, 65536, 65536, 0, 0)
