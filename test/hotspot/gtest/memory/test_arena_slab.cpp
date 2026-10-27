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

class Tester {

  const size_t _block_size;
  const size_t _granule_size;
  const unsigned _num_blocks;
  const ArenaSlab::Storage _storage;
  ArenaSlab _slab;
  const BlockRange<unsigned> _block_range;
  CHeapBitMap _control;
  PredictableRandom _random;

  static size_t calc_total_storage_size(const ArenaSlab::Storage& storage) {
    return align_up(storage._blocks_bytes + storage._meta_bytes, os::vm_page_size());
  }

  static ArenaSlab::Storage allocate_storage_areas(unsigned block_size, unsigned num_blocks) {
    ArenaSlab::Storage storage;
    ArenaSlab::calculate_storage_needs(block_size, num_blocks, &storage);
    const size_t total = calc_total_storage_size(storage);

    address p_total = (address)os::reserve_memory(storage._blocks_bytes, mtTest, false);
    assert(p_total != nullptr, "OOM");
    bool b = os::commit_memory((char*)p_total, total, false);
    assert(!b, "OOM");

    storage._blocks = p_total;
    storage._meta = p_total + storage._blocks_bytes;

    return storage;
  }

  static void deallocate_storage_areas(const ArenaSlab::Storage& storage) {
    const size_t total = calc_total_storage_size(storage);
    os::release_memory((char*)storage._blocks, total);
  }

  void check_equality() const {
    EXPECT_EQ(_control.count_one_bits(), _slab.num_in_use_blocks());
    EXPECT_LE(_control.count_one_bits(), _slab.num_dirty_blocks());
  }

  int allocate_blocks_and_verify(int num) {

    // fake-allocate in control bitmap
    CHeapBitMap tmp(_num_blocks, mtTest, false);
    tmp.set_from(_control);
    BitMap::idx_t idx = 0;
    for (int i = 0; i < num; ++i) {
      idx = tmp.find_first_clear_bit(idx, _num_blocks);
      if (idx == _num_blocks) {
        break;
      }
      tmp.set_bit(idx);
    }

    // now the real thing
    const_blockptr_t* const arr = NEW_C_HEAP_ARRAY(const_blockptr_t, num, mtTest);
    const unsigned n = _slab.allocate_blocks(arr, num);
    EXPECT_EQ(n, tmp.count_one_bits());

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

  void deallocate_and_verify(uintptr_t block_indexes[], unsigned num) {
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
    ASSERT_EQ(_slab.num_in_use_blocks(), _num_blocks);
  }

public:

  Tester(size_t block_size, size_t granule_size, unsigned num_blocks) :
    _block_size(block_size),
    _granule_size(granule_size),
    _num_blocks(num_blocks),
    _storage(allocate_storage_areas(block_size, granule_size)),
    _slab(&_storage, block_size, granule_size, num_blocks, true, true),
    _block_range(_storage._blocks, block_size, num_blocks),
    _control(num_blocks, mtTest, true)
  {
  }

  ~Tester() {
    deallocate_storage_areas(_storage);
  }

  void test_allocate_and_deallocate() {
    allocate_fully();
    // a bit beyond, but slab is full
    ASSERT_EQ(allocate_blocks_and_verify(2), 0);
    // Now deallocate.
    // Mode=0: forward, Mode=1 reverse, Mode=2 random
    int num_allocated = _num_blocks;
    uintptr_t arr[16];
    while (num_allocated > 0) {
      const int num_to_deallocate = MIN2(num_allocated, 1 + (int)_random.next(15));
      ArenaTestHelpers::get_random_set_bits(_random, _control, arr, num_to_deallocate);
      deallocate_and_verify(arr, num_to_deallocate);
    }
  }

};


#define CALL_TEST(XXX) test_##XXX()

#define DEFINE_TESTS(block_size, granule_size, num_blocks, testname) \
TEST_VM(Arena2_Slab, test_ ## testname ## _ ## block_size ## _ ## num_blocks ## _ ## granule_size) { \
  Tester tester(block_size, granule_size, num_blocks); \
  tester.CALL_TEST(testname); \
}

#define DEFINE_ALL_TESTS(numblocks, granulesize, testname) \
  DEFINE_TESTS(numblocks, granulesize, testname, allocate_and_deallocate )

DEFINE_ALL_TESTS(256, 4096, 1024)
DEFINE_ALL_TESTS(256, 2097152, 8192)
