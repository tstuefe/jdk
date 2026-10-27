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
#include "utilities/ostream.hpp"

#include "unittest.hpp"

//#define LOG_PLEASE
#include "testutils.hpp"
#include "test_arena_helpers.hpp"

template <size_t szclass,
          uintptr_t num_blocks,
          size_t granulesize>
class Tester {

  typedef ArenaSlab<szclass, num_blocks, granulesize> SlabType;

  const address _storage;
  SlabType _slab;
  const BlockRange<uintptr_t, szclass> _blockrange;
  CHeapBitMap _control;
  PredictableRandom _random;

  static address allocate_storage() {
    const address p = (address)os::reserve_memory(SlabType::total_size, mtTest, false);
    assert(p != nullptr, "OOM");
    return p;
  }

  void check_equality() const {
    EXPECT_EQ(_control.count_one_bits(), _slab.num_in_use_blocks());
    EXPECT_LE(_control.count_one_bits(), _slab.num_dirty_blocks());
  }

  int allocate_blocks_and_verify(int num) {

    // fake-allocate in control bitmap
    CHeapBitMap tmp(num_blocks, mtTest, false);
    tmp.set_from(_control);
    BitMap::idx_t idx = 0;
    for (int i = 0; i < num; ++i) {
      idx = _control.find_first_clear_bit(idx, num_blocks);
      if (idx == num_blocks) {
        break;
      }
      _control.set_bit(idx);
    }

    // now the real thing
    const_blockptr_t* const arr = NEW_C_HEAP_ARRAY(const_blockptr_t, num, mtTest);
    const int n = _slab.allocate_blocks(arr, num);
    EXPECT_EQ(n, tmp.count_one_bits());

    for (int i = 0; i < n; ++i) {
      EXPECT_TRUE(_blockrange.is_valid_block(arr[i]));
      EXPECT_TRUE(tmp.at(_blockrange.block_to_index(arr[i])));
    }

    _control.set_union(tmp);

    // write into all the blocks to check they are writable
    for (int i = 0; i < n; ++i) {
      blockptr_t b = const_cast<blockptr_t>(arr[i]);
      os::pretouch_memory(b, b + szclass, os::vm_page_size());
      b[0] = 17;
      b[szclass - 1] = 18;
    }
    FREE_C_HEAP_ARRAY(arr);

    check_equality();
    DEBUG_ONLY(_slab.verify();)
    return n;
  }

  void deallocate_and_verify(uintptr_t block_indexes[], int num) {
    // check that the released blocks are in use
    CHeapBitMap tmp(num_blocks, mtTest, true);
    for (int i = 0; i < num; ++i) {
      tmp.set_bit(block_indexes[i]);
    }
    assert(_control.contains(tmp), "releasing not in-use blocks");

    // Update control
    _control.set_difference(tmp);

    // Do it for real now
    const_blockptr_t* const arr = NEW_C_HEAP_ARRAY(const_blockptr_t, num, mtTest);
    for (int i = 0; i < num; ++i) {
      arr[i] = _blockrange.index_to_block(block_indexes[i]);
    }
    _slab.deallocate_blocks(arr, num);
    FREE_C_HEAP_ARRAY(arr);

    check_equality();
    DEBUG_ONLY(_slab.verify();)
  }

  void allocate_fully() {
    // allocate to the brim and a bit beyond
    int num_allocated = 0;
    const_blockptr_t arr[16];
    while (num_allocated < num_blocks) {
      const int next_to_allocate = MAX2(_random.next(16), 1u);
      const int n = allocate_blocks_and_verify(arr, next_to_allocate);
      ASSERT_LE(n, next_to_allocate);
      ASSERT_GE(n, 1);
      num_allocated += n;
    }
    ASSERT_EQ(_slab.num_in_use_blocks(), num_blocks);
  }

public:

  Tester() :
    _storage(allocate_storage()),
    _slab(_storage, SlabType::total_size),
    _blockrange(_storage, num_blocks),
    _control(num_blocks, mtTest, true)
  {
  }

  ~Tester() {
    os::release_memory(_storage, SlabType::total_size);
  }

  void test_allocate_and_deallocate() {
    allocate_fully();
    // a bit beyond, but slab is full
    ASSERT_EQ(allocate_blocks_and_verify(2), 0);
    // Now deallocate.
    // Mode=0: forward, Mode=1 reverse, Mode=2 random
    int num_allocated = num_blocks;
    uintptr_t arr[16];
    while (num_allocated > 0) {
      const int num_to_deallocate = MIN2(num_allocated, 1 + (int)_random.next(15));
      ArenaTestHelpers::get_random_set_bits(_random, _control, arr, num_to_deallocate);
      deallocate_and_verify(arr, num_to_deallocate);
    }
  }

};


#define CALL_TEST(XXX) test_##XXX()

#define DEFINE_TESTS(szclass, numblocks, granulesize, testname) \
TEST_VM(Arena2_Slab, test_ ## testname ## _ ## szclass ## _ ## numblocks ## _ ## granulesize) { \
  Tester<szclass, numblocks, granulesize> tester; \
  tester.CALL_TEST(testname); \
}

#define DEFINE_ALL_TESTS(numblocks, granulesize, testname) \
  DEFINE_TESTS(numblocks, granulesize, testname, allocate_and_deallocate )

DEFINE_ALL_TESTS(256, 4096, 65536)
