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

class ArenaSlabTester {
  typedef BitMap::idx_t IndexType;

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
  unsigned num_granules() const { return _slab.num_granules(); }
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

  static constexpr uint8_t canary_1 = 0x17u;
  static constexpr uint8_t canary_2 = 0xFEu;

  void check_for_memory_corruption() {
    // Check for overwriters in every block that is should be in use and should not have
    // been touched by anything
    for (unsigned i = 0; i < num_blocks(); i ++) {
      if (_control.at(i)) {
        const const_blockptr_t b = _block_range.index_to_block(i);
        ASSERT_EQ(b[0], canary_1);
        ASSERT_EQ(b[block_size() - 1], canary_2);
      }
    }
  }

  // Allocates num blocks, with verification; returns number of allocated
  // blocks
  unsigned allocate_blocks_and_verify(unsigned num) {

    // fake-allocate in control bitmap
    CHeapBitMap prediction(num_blocks(), mtTest, true);
    IndexType idx = 0;
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
      b[0] = canary_1;
      b[block_size() - 1] = canary_2;
    }
    FREE_C_HEAP_ARRAY(arr);

    check_equality();
    DEBUG_ONLY(_slab.verify();)
    return num_allocated;
  }

  void deallocate_block_range_and_verify(IndexType beg, IndexType end) {
    const unsigned num = end - beg;
    IndexType* const arr = NEW_C_HEAP_ARRAY(IndexType, num, mtTest);
    for (unsigned i = 0; i < num; ++i) {
      arr[i] = beg + i;
    }
    deallocate_blocks_and_verify(arr, num);
  }

  void deallocate_blocks_and_verify(IndexType block_indexes[], unsigned num) {
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
      const const_blockptr_t b = _block_range.index_to_block(block_indexes[i]);
      ASSERT_EQ(b[0], canary_1); // check for overwriters
      ASSERT_EQ(b[block_size() - 1], canary_2);
      arr[i] = b;
    }
    _slab.deallocate_blocks(arr, num);
    FREE_C_HEAP_ARRAY(arr);

    check_equality();
    DEBUG_ONLY(_slab.verify();)
  }

  // void deallocate_blocks_and_verify(IndexType block_index) {
  //   IndexType idx = block_index;
  //   deallocate_blocks_and_verify(&idx, 1);
  // }

  // Allocates a huge block, with verification. Returns block index.
  const_blockptr_t allocate_huge_block_and_verify(size_t size) {
    LOG_HERE("allocate_huge_block_and_verify %zu", size);
    assert(is_aligned(size, ArenaBlockHelpers::huge_block_granularity), "Sanity");
    const unsigned num = size / ArenaBlockHelpers::huge_block_granularity;

    // fake-allocate in control bitmap
    const IndexType idx = ArenaTestHelpers::find_first_clear_run(_control, num);

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
    const IndexType idx = _block_range.block_to_index(p);

    // Range must be marked as in-use
    ASSERT_TRUE(_control.at(idx));
    const IndexType idx2 = _control.find_first_clear_bit(idx);
    ASSERT_GE(idx2, idx + num);

    // Fake-clear in control
    _control.clear_range(idx, idx + num);

    // Deallocate for real
    _slab.deallocate_huge_block(p, size);

    check_equality();
    DEBUG_ONLY(_slab.verify();)
  }

  void tick_and_verify(unsigned tick_number) {
    // For now, ignore on slabs where blocksize != granulesize;
    // releasing memory not yet implemented for those.
    // See comment in ArenaSlab::harvest();
    if (block_size() < granule_size()) {
      return;
    }
    const unsigned dirty_blocks_1 = _slab.count_dirty_blocks();
    _slab.tick(tick_number);
    const unsigned dirty_blocks_2 = _slab.count_dirty_blocks();
    ASSERT_LE(dirty_blocks_2, dirty_blocks_1);
    check_equality();
    check_for_memory_corruption();
    DEBUG_ONLY(_slab.verify();)
  }

  void harvest_dirty_granules_and_verify(unsigned release_max,
                                         IndexType granule_lo, IndexType granule_hi,
                                         unsigned min_release_len)
  {

    // Pre-state: - The slab contains 0..n in-use blocks that are marked with canary (which we know from _control_in_use)
    //            - The slab contains 0..m dirty granules (from dirty_granule_bitmap)
    //              These granules will still be accessible and their canaries still be in place.
    // Call harvest_dirty_granules on the slab and then:
    //            - all in-use blocks should remain untouched
    //            - all previously dirty granules that fall into the expected release zones should now be zero'd out

    // Please note that the _release_boundary does not matter for this test, since the function we test
    // (harvest_dirty_granules) functions at a lower layer.

    // For now, this works only for granules that are blocks. See comment in arenaSlab.hpp.
    assert(block_size() == granule_size(), "Sanity");

    // Equally, this only works if the slab is fully committed (to simplify matters)
    assert(num_granules() == _slab.commit_boundary(), "Sanity");

    // Grab a copy of the dirty bitmap from the slab
    CHeapBitMap dirty_bitmap(num_granules(), mtTest, false);
    _slab.get_dirty_map(dirty_bitmap);

    ASSERT_TRUE(dirty_bitmap.contains(_control));

    // Pre-state check. Check that all dirty blocks (which have to be not yet released at this point)
    // are canary-intact. All others are zero'd out (since the full slab is committed, they should
    // all be accessible)
    for (IndexType i = 0; i < num_granules(); i ++) {
      const const_blockptr_t b = _block_range.index_to_block(i);
      if (dirty_bitmap.at(i)) {
        ASSERT_EQ(b[0], canary_1);
        ASSERT_EQ(b[block_size() - 1], canary_2);
      } else {
        ASSERT_EQ(b[0], 0u);
        ASSERT_EQ(b[block_size() - 1], 0u);
      }
    }

    // Calculate the expected state of the dirty bitmap after we released memory
    // (essentially, we are mimicking harvest_dirty_blocks() here but keep the code deliberately
    // super simple
    CHeapBitMap dirty_bitmap_2(num_granules(), mtTest, false);
    dirty_bitmap_2.set_from(dirty_bitmap);
    IndexType idx = granule_hi - 1;
    unsigned run_len = 0;
    unsigned to_release = release_max;
    auto consider_range = [&](IndexType i1, IndexType i2) {
      unsigned len = checked_cast<unsigned>(i2 - i1);
      len = MIN2(len, to_release);
      if (len >= min_release_len) {
        dirty_bitmap_2.clear_range(i2 - len, i2);
        to_release -= len;
      }
    };
    while (to_release > 0) {
      const bool releasable = !_control.at(idx) && dirty_bitmap_2.at(idx);
      if (releasable) {
        run_len++;
      } else {
        consider_range(idx + 1, idx + 1 + run_len);
        run_len = 0;
      }
      if (idx == granule_lo) {
        break;
      }
      idx --;
    }
    if (run_len > 0 && to_release > 0) {
      consider_range(idx, idx + run_len);
    }

    // Now release for real
    _slab.harvest_dirty_granules(release_max, granule_lo, granule_hi, min_release_len);

    // Check the expected dirty state
    _slab.get_dirty_map(dirty_bitmap);
    ASSERT_TRUE(dirty_bitmap.is_same(dirty_bitmap_2));
    ASSERT_TRUE(dirty_bitmap.contains(_control));

    // Post-state check. Check that all dirty blocks (in-use or not in-use)
    // are canary-intact. All others are zero'd out.
    for (IndexType i = 0; i < num_granules(); i ++) {
      const const_blockptr_t b = _block_range.index_to_block(i);
      if (dirty_bitmap.at(i)) {
        ASSERT_EQ(b[0], canary_1);
        ASSERT_EQ(b[block_size() - 1], canary_2);
      } else {
        ASSERT_EQ(b[0], 0u);
        ASSERT_EQ(b[block_size() - 1], 0u);
      }
    }

    check_equality();
    DEBUG_ONLY(_slab.verify();)
  }

  void test_harvest_with_one_hole(IndexType hole_beg, IndexType hole_end,
                                  unsigned release_max,
                                  IndexType granule_lo, IndexType granule_hi,
                                  unsigned min_release_len
  ) {
    allocate_fully();
    deallocate_block_range_and_verify(hole_beg, hole_end);
    harvest_dirty_granules_and_verify(release_max, granule_lo, granule_hi, min_release_len);
  }

  void allocate_fully() {
    // allocate fully (regardless of current state of slab)
    unsigned free_blocks = num_blocks() - _slab.count_in_use_blocks();
    while (free_blocks > 0) {
      const unsigned num_to_allocate = clamp<unsigned>(_random.next(16), 1u, free_blocks);
      const unsigned n = allocate_blocks_and_verify(num_to_allocate);
      ASSERT_LE(n, num_to_allocate);
      ASSERT_GE(n, 1u);
      free_blocks -= n;
    }
    ASSERT_EQ(_slab.count_in_use_blocks(), num_blocks());
    ASSERT_EQ(_slab.count_dirty_blocks(), num_blocks());
  }

public:

  explicit ArenaSlabTester(const ArenaSlab::Parameters* params) :
    _params(params),
    _arena_heap(), _metadata_heap(),
    _slab(&_arena_heap, &_metadata_heap, _params),
    _block_range(_slab.base(), params->block_size, params->num_blocks),
    _control(params->num_blocks, mtTest, true) {
  }

  ~ArenaSlabTester() {
  }

  void test_allocate_and_deallocate() {
    allocate_fully();
    // a bit beyond, but slab is full
    ASSERT_EQ(allocate_blocks_and_verify(2), 0u);
    // Now deallocate.
    // Mode=0: forward, Mode=1 reverse, Mode=2 random
    int num_allocated = num_blocks();
    IndexType arr[16];
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
    IndexType holes [max_num_holes];
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

  void test_harvest_nothing_1() {
    if (block_size() < granule_size()) {
      GTEST_SKIP() << "only for blocksize == granulesize";
    }
    // all blocks in use, we harvest nothing
    allocate_fully();
    ASSERT_EQ(_slab.count_dirty_blocks(), num_blocks());
    harvest_dirty_granules_and_verify(num_granules(), 0, num_granules(), 1u);
    ASSERT_EQ(_slab.count_dirty_blocks(), num_blocks()); // nothing changed
  }

  void test_harvest_nothing_2() {
    if (block_size() < granule_size()) {
      GTEST_SKIP() << "only for blocksize == granulesize";
    }
    // minsize too large, we harvest nothing
    allocate_fully();
    deallocate_block_range_and_verify(0, 16);
    ASSERT_EQ(_slab.count_dirty_blocks(), num_blocks());
    harvest_dirty_granules_and_verify(num_granules(), 0, num_granules(), 17u);
    ASSERT_EQ(_slab.count_dirty_blocks(), num_blocks()); // nothing changed
  }

  void test_harvest_half() {
    if (block_size() < granule_size()) {
      GTEST_SKIP() << "only for blocksize == granulesize";
    }
    allocate_fully();
    deallocate_block_range_and_verify(0, num_blocks()); // dealloc all. Now all is dirty and releasable
    ASSERT_EQ(_slab.count_dirty_granules(), num_granules());

    // Release but only in upper range
    harvest_dirty_granules_and_verify(num_granules(), num_granules() / 2, num_granules(), 1u);
    ASSERT_EQ(_slab.count_dirty_granules(), num_granules() / 2); // only half should be released
  }

  void test_harvest_one_hole_sweep() {
    if (block_size() < granule_size()) {
      GTEST_SKIP() << "only for blocksize == granulesize";
    }
    for (unsigned hole_size = 1; hole_size < 10; hole_size ++) {
      for (unsigned offset = 0; offset < 3; offset ++) {
        for (unsigned minlen = 1; minlen < hole_size + 1; minlen *= 2) {
          for (int at_back = 0; at_back <= 1; at_back ++) {
            for (int has_budget_cap = 0; has_budget_cap <= 1; has_budget_cap ++) {
              const IndexType hole_beg = at_back ? num_granules() - hole_size - offset : offset;
              const IndexType hole_end = hole_beg + hole_size;
              const unsigned max_release = has_budget_cap ? hole_size / 2 : num_granules();
              if (max_release == 0) {
                continue;
              }
              test_harvest_with_one_hole(hole_beg, hole_end,
                max_release, 0, num_granules(), minlen);
              if (::testing::Test::HasFailure()) {
                return;
              }
            }
          }
        }
      }
    }
  }

  void test_harvest_random() {
    if (block_size() < granule_size()) {
      GTEST_SKIP() << "only for blocksize == granulesize";
    }
    // Release random ranges, then harvest the whole thing and verify
    for (unsigned cycle = 0; cycle < 16; cycle++) {
      allocate_fully();
      IndexType idx = 0;
      const unsigned max_run_len = MIN2(8u, num_granules() / 32u);
      while (idx < num_granules()) {
        const IndexType idx2 = MIN2<IndexType>(idx + _random.next(max_run_len) + 1u, num_granules());
        const bool release = _random.tosscoin();
        if (release) {
          deallocate_block_range_and_verify(idx, idx2);
        }
        idx = idx2;
      }
      const unsigned min_release = cycle % 8;
      harvest_dirty_granules_and_verify(num_granules(), 0, num_granules(), min_release);
      if (::testing::Test::HasFailure()) {
        return;
      }
    }
  }

  void test_random() {
    for (unsigned cycle = 0; cycle < 10000; cycle++) {
      const int op_select = _random.next(16);
      if (op_select < 13) {
        allocate_blocks_and_verify(1 + _random.next(8));
      } else if (op_select < 15) {
        IndexType blocks[8];
        unsigned num = 1 + _random.next(8);
        num = ArenaTestHelpers::get_random_set_bits(_random, _control, blocks, num);
        deallocate_blocks_and_verify(blocks, num);
      } else {
        tick_and_verify(cycle);
      }
      if (::testing::Test::HasFailure()) {
        tty->print_cr("cycle %d op %d", cycle, op_select);
        return;
      }
    }
  }
};

#define DEFINE_TESTS(block_size, granule_size, num_blocks, release_boundary, decay_factor, testname) \
TEST_VM(Arena2_Slab, test_ ## testname ## _ ## block_size ## _ ## granule_size ## _ ## num_blocks ## _ ## release_boundary ## _ ## decay_factor) { \
  const ArenaSlab::Parameters params = { block_size, granule_size, num_blocks, release_boundary, false, decay_factor }; \
  ArenaSlabTester tester(&params); \
  tester.testname (); \
}

#define DEFINE_ALL_TESTS(block_size, granule_size, num_blocks, release_boundary, decay_factor) \
  DEFINE_TESTS(block_size, granule_size, num_blocks, release_boundary, decay_factor, test_allocate_and_deallocate ) \
  DEFINE_TESTS(block_size, granule_size, num_blocks, release_boundary, decay_factor, test_allocate_first_fit_order_no_exhaustion ) \
  DEFINE_TESTS(block_size, granule_size, num_blocks, release_boundary, decay_factor, test_allocate_first_fit_order_with_exhaustion ) \
  DEFINE_TESTS(block_size, granule_size, num_blocks, release_boundary, decay_factor, test_harvest_nothing_1 ) \
  DEFINE_TESTS(block_size, granule_size, num_blocks, release_boundary, decay_factor, test_harvest_nothing_2 ) \
  DEFINE_TESTS(block_size, granule_size, num_blocks, release_boundary, decay_factor, test_harvest_half ) \
  DEFINE_TESTS(block_size, granule_size, num_blocks, release_boundary, decay_factor, test_harvest_one_hole_sweep ) \
  DEFINE_TESTS(block_size, granule_size, num_blocks, release_boundary, decay_factor, test_harvest_random ) \
  DEFINE_TESTS(block_size, granule_size, num_blocks, release_boundary, decay_factor, test_random ) \


//               block_size
//                        granule_size
//                                  num_blocks
//                                            release_boundary (in number of blocks!)
//                                                     decay_factor
// Note: Granules >= 2MB use large pages (todo)

DEFINE_ALL_TESTS(256,     16384,    16384,    16384,       0)   // 4MB
DEFINE_ALL_TESTS(256,     65536,    8192,     8192,        0)   // 2MB
DEFINE_ALL_TESTS(256,     2097152,  16384,    16384,       0)

DEFINE_ALL_TESTS(32768,   32768,    8192,     0,       0)   // 256MB
DEFINE_ALL_TESTS(32768,   32768,    8192,     17,      0)   // 256MB
DEFINE_ALL_TESTS(32768,   65536,    2048,     0,       0)
DEFINE_ALL_TESTS(32768,   2097152,  2048,     0,       0)

DEFINE_ALL_TESTS(1048576, 1048576,  32,       0,       0)   // 32MB
DEFINE_ALL_TESTS(1048576, 2097152,  32,       0,       0)   // 32MB

DEFINE_ALL_TESTS(2097152, 2097152,  64,       0,       0)   // 128MB

