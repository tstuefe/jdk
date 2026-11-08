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

    const ArenaSlab::Parameters* const _params;

    ArenaHeap _arena_heap;
    ArenaMetadataHeap _metadata_heap;
    ArenaSlab _slab;
    const BlockRange<unsigned> _block_range;
    CHeapBitMap _control;
    PredictableRandom _random;

    size_t block_size() const { return _params->block_size; }
    size_t granule_size() const { return _params->granule_size; }
    unsigned num_blocks() const { return _params->num_blocks; }
    unsigned release_boundary() const { return _params->release_boundary; }
    double decay_factor() const { return _params->decay_factor; }

    void check_equality() const {
      CHeapBitMap tmp(num_blocks(), mtTest, false);
      _slab.get_in_use_map(tmp);
      EXPECT_TRUE(_control.is_same(tmp));
      EXPECT_EQ(_control.count_one_bits(), _slab.count_in_use_blocks());
      EXPECT_LE(_control.count_one_bits(), _slab.count_dirty_blocks());
      DEBUG_ONLY(_slab.verify();)
    }

    // Allocates num blocks, with verification; returns number of allocated
    // blocks
    unsigned allocate_blocks_and_verify(unsigned num) {

      // fake-allocate in control bitmap
      CHeapBitMap prediction(num_blocks(), mtTest, true);
      BitMap::idx_t idx = 0;
      unsigned num_fake_allocated = 0;
      while (idx < num_blocks() && num_fake_allocated < num) {
        idx = _control.find_first_clear_bit(idx, num_blocks());
        if (idx < num_blocks()) {
          num_fake_allocated ++;
          prediction.set_bit(idx);
          idx ++;
        }
      }

      // now the real thing
      const_blockptr_t* const arr = NEW_C_HEAP_ARRAY(const_blockptr_t, num, mtTest);
      const unsigned num_free_before_allocation = num_blocks() - _slab.count_in_use_blocks();
      const unsigned num_allocated = _slab.allocate_blocks(arr, num);
      EXPECT_EQ(num_allocated, num_fake_allocated);
      EXPECT_EQ(num_allocated, MIN2(num_free_before_allocation, num));
      EXPECT_EQ(num_allocated, prediction.count_one_bits());

      const_blockptr_t last = nullptr;
      for (unsigned i = 0; i < num_allocated; ++i) {
        EXPECT_TRUE(_block_range.is_valid_block(arr[i]));
        EXPECT_TRUE(prediction.at(_block_range.block_to_index(arr[i])));
        EXPECT_FALSE(_control.at(_block_range.block_to_index(arr[i])));
        if (last != nullptr) {
          EXPECT_GT(arr[i], last);
        }
        last = arr[i];
      }

      _control.set_union(prediction);

      // write into all the blocks to check they are writable
      for (unsigned i = 0; i < num_allocated; ++i) {
        const blockptr_t b = const_cast<blockptr_t>(arr[i]);
        os::pretouch_memory(b, b + block_size(), os::vm_page_size());
        b[0] = 17;
        b[block_size() - 1] = 18;
      }
      FREE_C_HEAP_ARRAY(arr);

      check_equality();
      DEBUG_ONLY(_slab.verify();)
      return num_allocated;
    }

    void deallocate_blocks_and_verify(BitMap::idx_t block_indexes[], unsigned num) {
      // check that the released blocks are in use
      CHeapBitMap tmp(num_blocks(), mtTest, true);
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

    // void deallocate_blocks_and_verify(BitMap::idx_t block_index) {
    //   BitMap::idx_t idx = block_index;
    //   deallocate_blocks_and_verify(&idx, 1);
    // }

    // Allocates a huge block, with verification. Returns block index.
    const_blockptr_t allocate_huge_block_and_verify(size_t size) {
      LOG_HERE("allocate_huge_block_and_verify %zu", size);
      assert(is_aligned(size, ArenaBlockHelpers::huge_block_granularity), "Sanity");
      const unsigned num = size / ArenaBlockHelpers::huge_block_granularity;

      // fake-allocate in control bitmap
      const BitMap::idx_t idx = ArenaTestHelpers::find_first_clear_run(_control, num);

      // allocate in real bitmap
      const const_blockptr_t p = _slab.allocate_huge_block(size);

      if (p == nullptr) {
        EXPECT_EQ(idx, num_blocks());
      } else {
        EXPECT_EQ(_block_range.index_to_block(idx), p);
        _control.set_range(idx, idx + num);
        // write into all the blocks to check they are writable
        os::pretouch_memory(const_cast<blockptr_t>(p),
                            const_cast<blockptr_t>(p) + size,
                            os::vm_page_size());

      }

      check_equality();
      DEBUG_ONLY(_slab.verify();)
      return p;
    }

    void deallocate_huge_block_and_verify(const_blockptr_t p, size_t size) {
      LOG_HERE("deallocate_huge_block_and_verify %zu", size);

      assert(is_aligned(size, ArenaBlockHelpers::huge_block_granularity), "Sanity");

      const unsigned num = size / ArenaBlockHelpers::huge_block_granularity;
      const BitMap::idx_t idx = _block_range.block_to_index(p);

      // Range must be marked as in-use
      ASSERT_TRUE(_control.at(idx));
      const BitMap::idx_t idx2 = _control.find_first_clear_bit(idx);
      ASSERT_GE(idx2, idx + num);

      // Fake-clear in control
      _control.clear_range(idx, idx + num);

      // Deallocate for real
      _slab.deallocate_huge_block(p, size);

      check_equality();
      DEBUG_ONLY(_slab.verify();)
    }

    // void deallocate_block_range_and_verify(BitMap::idx_t from, BitMap::idx_t to) {
    //   const unsigned num = to - from;
    //   assert(num > 0, "Sanity");
    //   BitMap::idx_t* const arr = NEW_C_HEAP_ARRAY(BitMap::idx_t, num, mtTest);
    //   for (BitMap::idx_t idx = from; idx < to; idx ++) {
    //     arr[idx - from] = idx;
    //   }
    //   deallocate_blocks_and_verify(arr, num);
    // }

    void allocate_fully() {
      // allocate to the brim and a bit beyond
      unsigned num_allocated = 0;
      while (num_allocated < num_blocks()) {
        const unsigned num_to_allocate = MAX2(_random.next(16), 1u);
        const unsigned n = allocate_blocks_and_verify(num_to_allocate);
        ASSERT_LE(n, num_to_allocate);
        ASSERT_GE(n, 1u);
        num_allocated += n;
      }
      ASSERT_EQ(_slab.count_in_use_blocks(), num_blocks());
    }

  public:
    
    explicit Tester(const ArenaSlab::Parameters* params) :
      _params(params),
      _arena_heap(), _metadata_heap(),
      _slab(&_arena_heap, &_metadata_heap, _params),
      _block_range(_slab.base(), params->block_size, params->num_blocks),
      _control(params->num_blocks, mtTest, true) {
    }

    ~Tester() {
    }

    void test_allocate_and_deallocate() {
      allocate_fully();
      // a bit beyond, but slab is full
      ASSERT_EQ(allocate_blocks_and_verify(2), 0u);
      // Now deallocate.
      // Mode=0: forward, Mode=1 reverse, Mode=2 random
      int num_allocated = num_blocks();
      BitMap::idx_t arr[16];
      while (num_allocated > 0) {
        int num_to_deallocate = MIN2(num_allocated, 1 + (int)_random.next(15));
        num_to_deallocate = ArenaTestHelpers::get_random_set_bits(_random, _control, arr, num_to_deallocate);
        deallocate_blocks_and_verify(arr, num_to_deallocate);
        num_allocated -= num_to_deallocate;
        if (::testing::Test::HasFailure()) {
          return;
        }
      }
    }

    void test_allocate_first_fit_order_0(bool exhaust_slab) {
      allocate_fully();

      // Punch holes
      constexpr unsigned max_num_holes = 119u;
      const unsigned num_holes = MIN2(num_blocks() / 4, max_num_holes);
      BitMap::idx_t holes [max_num_holes];
      ArenaTestHelpers::get_random_set_bits(_random, _control, holes, num_holes);
      deallocate_blocks_and_verify(holes, num_holes);
      assert(_slab.count_in_use_blocks() == num_blocks() - num_holes, "Sanity");

      const unsigned to_fetch = exhaust_slab ? num_holes * 2 : num_holes / 2;
      const unsigned got = allocate_blocks_and_verify(to_fetch);
      if (exhaust_slab) {
        EXPECT_EQ(got, num_holes);
      } else {
        EXPECT_EQ(got, to_fetch);
      }
    }

    void test_allocate_first_fit_order_no_exhaustion() { test_allocate_first_fit_order_0(false); }
    void test_allocate_first_fit_order_with_exhaustion() { test_allocate_first_fit_order_0(true); }

    void test_huge_block_allocation_deallocation() {
      if (block_size() != ArenaBlockHelpers::huge_block_granularity) {
        GTEST_SKIP() << "only for huge blocks";
      }
      assert(num_blocks() >= 64, "need at least this many blocks");

      const const_blockptr_t p1 = allocate_huge_block_and_verify(4 * M); // blocks 0-1
      EXPECT_EQ(p1, _slab.base());
      const const_blockptr_t p2 = allocate_huge_block_and_verify(2 * M); // block 2
      EXPECT_EQ(p2, p1 + 4 * M);
      const const_blockptr_t p3 = allocate_huge_block_and_verify(8 * M); // blocks 3-6
      EXPECT_EQ(p3, p2 + 2 * M);

      deallocate_huge_block_and_verify(p1, 4 * M);                        // hole of 2 at [0,2)
      // Allocate, does not fit hole
      const const_blockptr_t p4 = allocate_huge_block_and_verify(6 * M);
      EXPECT_EQ(p4, p3 + 8 * M);
      // Allocate, fits exactly
      const const_blockptr_t p5 = allocate_huge_block_and_verify(4 * M);
      EXPECT_EQ(p5, _slab.base());

      // Exhaust the slab
      const const_blockptr_t p6 = allocate_huge_block_and_verify((num_blocks() * block_size()) - 20 * M);
      EXPECT_EQ(p6, _slab.base() + 20 * M);
      EXPECT_EQ(_slab.count_in_use_blocks(), num_blocks());

      // A hole near the end of the slab, nothing else free
      const const_blockptr_t hole = align_down(_slab.end() - (9 * M), block_size());
      deallocate_huge_block_and_verify(hole, 2 * M);

      EXPECT_EQ(allocate_huge_block_and_verify(6 * M), nullptr); // Should fail
      EXPECT_EQ(allocate_huge_block_and_verify(2 * M), hole); // Should succeed
      EXPECT_EQ(_slab.count_in_use_blocks(), num_blocks()); // Full again

      // Fragmentation: enough free total, no contiguous run
      EXPECT_EQ(_slab.count_in_use_blocks(), num_blocks());
      for (const_blockptr_t p7 = _slab.base(); p7 < _slab.end(); p7 += 4 * M) {
        deallocate_huge_block_and_verify(p7, 2 * M);
      }
      EXPECT_EQ(_slab.count_in_use_blocks(), num_blocks() / 2);

      EXPECT_EQ(_slab.allocate_huge_block(4 * M), nullptr);
      EXPECT_NE(_slab.allocate_huge_block(2 * M), nullptr);
    }

    void test_random() {
      for (int cycle = 0; cycle < 10000; cycle++) {
        const int op_select = _random.next(16);
        if (op_select < 8) {
          allocate_blocks_and_verify(1 + _random.next(8));
        } else if (op_select < 15) {
          BitMap::idx_t blocks[8];
          unsigned num = 1 + _random.next(8);
          num = ArenaTestHelpers::get_random_set_bits(_random, _control, blocks, num);
          deallocate_blocks_and_verify(blocks, num);
        } else {
          // Todo
          // tick_and_test();
        }
        if (::testing::Test::HasFailure()) {
          tty->print_cr("cycle %d op %d", cycle, op_select);
          return;
        }
      }
    }
  };
}


#define DEFINE_TESTS(block_size, granule_size, num_blocks, release_boundary, decay_factor, testname) \
TEST_VM(Arena2_Slab, test_ ## testname ## _ ## block_size ## _ ## granule_size ## _ ## num_blocks ## _ ## release_boundary ## _ ## decay_factor) { \
  const ArenaSlab::Parameters params = { block_size, granule_size, num_blocks, release_boundary, decay_factor }; \
  Tester tester(&params); \
  tester.testname (); \
}

#define DEFINE_ALL_TESTS(block_size, granule_size, num_blocks, release_boundary, decay_factor) \
  DEFINE_TESTS(block_size, granule_size, num_blocks, release_boundary, decay_factor, test_allocate_and_deallocate ) \
  DEFINE_TESTS(block_size, granule_size, num_blocks, release_boundary, decay_factor, test_allocate_first_fit_order_no_exhaustion ) \
  DEFINE_TESTS(block_size, granule_size, num_blocks, release_boundary, decay_factor, test_allocate_first_fit_order_with_exhaustion ) \
  DEFINE_TESTS(block_size, granule_size, num_blocks, release_boundary, decay_factor, test_huge_block_allocation_deallocation ) \
  DEFINE_TESTS(block_size, granule_size, num_blocks, release_boundary, decay_factor, test_random ) \


//               block_size
//                        granule_size
//                                  num_blocks
//                                            release_boundary (in number of granules)
//                                                     decay_factor
// Note: Granules >= 2MB use large pages (todo)

DEFINE_ALL_TESTS(256,     16384,    16384,    0,       0)   // 4MB
DEFINE_ALL_TESTS(256,     16384,    16384,    4,       0)   // 4MB, 64K resident
DEFINE_ALL_TESTS(256,     65536,    8192,     0,       0)   // 2MB
DEFINE_ALL_TESTS(256,     65536,    8192,     32,      0)   // 2MB, all resident
DEFINE_ALL_TESTS(256,     2097152,  16384,    0,       0)

DEFINE_ALL_TESTS(32768,   32768,    2048,     0,       0)   // 64MB
DEFINE_ALL_TESTS(32768,   65536,    2048,     0,       0)
DEFINE_ALL_TESTS(32768,   2097152,  2048,     0,       0)

DEFINE_ALL_TESTS(1048576, 1048576,  32,       0,       0)   // 32MB
DEFINE_ALL_TESTS(1048576, 2097152,  32,       0,       0)   // 32MB

DEFINE_ALL_TESTS(2097152, 2097152,  64,       0,       0)   // 128MB
DEFINE_ALL_TESTS(2097152, 2097152,  64,       4,       0)   // 128MB, 8 MB resident

