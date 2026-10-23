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
#include "memory/arena/arenaSlabAllocator.hpp"
#include "utilities/bitMap.inline.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/macros.hpp"
#include "utilities/ostream.hpp"

#include "unittest.hpp"

#define LOG_PLEASE
#include "testutils.hpp"
#include "code/vtableStubs.hpp"

// A dummy slab allocator
template <size_t szclass>
class FakeSlabAllocator : public ArenaSlabAllocator {
public:
  static constexpr int num_blocks = 256;

private:
  // We never write to the blocks handed around in this test, so
  // no need to allocate them for real.
  static constexpr uintptr_t _fake_start = LP64_ONLY(32 * G) NOT_LP64(G);
  static constexpr uintptr_t _fake_end = _fake_start + (szclass * num_blocks);
  CHeapBitMap _owned_blocks;
  int _last_allocated;
  int _last_deallocated;

public:

  FakeSlabAllocator() :
    _owned_blocks(num_blocks, mtTest, false),
    _last_allocated(0), _last_deallocated(0)
  {
    set_all_free();
  }

  void deallocate_blocks(size_t szclass0, const_blockptr_t blocks[], int len) override {
    assert(szclass0 == szclass, "wrong block class");
    for (int i = 0; i < len; i++) {
      const const_blockptr_t b = blocks[i];
      assert(b != nullptr, "block is null");
      _owned_blocks.set_bit(block_to_index(b));
    }
    _last_deallocated = len;
  }

  int allocate_blocks(size_t szclass0, const_blockptr_t out[], int outlen) override {
    int allocated = 0;
    BitMap::idx_t slot = 0;
    while (allocated < outlen && slot < _owned_blocks.size()) {
      slot = _owned_blocks.find_first_set_bit(slot);
      if (slot < _owned_blocks.size()) {
        out[allocated] = index_to_block(slot);
        allocated ++;
        _owned_blocks.clear_bit(slot);
        slot++;
      }
    }
    _last_allocated = allocated;
    return allocated;
  }

  address base() const override { return (address)_fake_start; }
  address end() const override { return (address) _fake_end; }

  int last_allocated() const { return _last_allocated; }
  int last_deallocated() const { return _last_deallocated; }
  void reset_counters() {
    _last_allocated = _last_deallocated = 0;
  }

  static const_blockptr_t index_to_block(int idx) {
    assert(idx >= 0 && idx < num_blocks, "invalid block index");
    return (const_blockptr_t) _fake_start + (idx * szclass);
  }

  static int  block_to_index(const_blockptr_t b) {
    assert(p2u(b) >= _fake_start && p2u(b) < _fake_end, "block out of range");
    return checked_cast<int>((p2u(b) - _fake_start) / szclass);
  }

  // Sets the whole slab as "in use"
  void set_all_in_use() { _owned_blocks.clear_range(0, _owned_blocks.size()); }
  void set_all_free() { _owned_blocks.set_range(0, _owned_blocks.size()); }

  const BitMap& in_use_bitmap() const { return _owned_blocks; }
};


template <size_t szclass, int cachesize>
class Tester {
  typedef FakeSlabAllocator<szclass> SlabAllocatorType;
  typedef ArenaBlockCache<szclass, cachesize> CacheType;

  SlabAllocatorType _slab_allocator;
  CacheType _block_cache;

  // We use a bitmap to mimic block state: one bit per block in slab.
  // _owned_by_caller tracks those blocks that count as allocated, so are in-use
  // from the perspective of cache and slab.
  // _owned_by_cache tracks blocks that are in the cache.
  // The slab itself tracks blocks in the slab.
  CHeapBitMap _owned_by_caller;
  CHeapBitMap _owned_by_cache;
  const BitMap& _owned_by_slab;

  PredictableRandom _random;

  static constexpr int num_blocks = SlabAllocatorType::num_blocks;

  // Setup world as if cache were empty and all slabs were in-use by callers
  void set_all_in_use() {
    _block_cache.flush();
    _owned_by_cache.clear_range(0, _owned_by_cache.size());
    _slab_allocator.set_all_in_use();
    _owned_by_caller.set_range(0, _owned_by_caller.size());
  }

  // Setup world as if cache were empty and all slabs were free (in slab)
  void set_all_free() {
    _block_cache.flush();
    _owned_by_cache.clear_range(0, _owned_by_cache.size());
    _owned_by_caller.clear_range(0, _owned_by_caller.size());
    _slab_allocator.set_all_free();
  }

  int num_owned_by_caller() const {
    return (int)_owned_by_caller.count_one_bits();
  }

  template <bool is_true>
  int get_random_bit(const BitMap& bm) {
    const BitMap::idx_t beg = _random.next(SlabAllocatorType::num_blocks);
    BitMap::idx_t idx = is_true ? bm.find_first_set_bit(beg) : bm.find_first_clear_bit(beg);
    if (idx == bm.size()) {
      idx = is_true ? bm.find_first_set_bit(0, beg) : bm.find_first_clear_bit(0, beg);
      if (idx == bm.size()) {
        return -1;
      }
    }
    return checked_cast<int>(idx);
  }

  int get_random_in_use_block_index() {
    return get_random_bit<true>(_owned_by_caller);
  }

  const_blockptr_t get_random_in_use_block() {
    const int block_index = get_random_in_use_block_index();
    return block_index == -1 ? nullptr : _slab_allocator.index_to_block(block_index);
  }

  // Returns a number of unique random blocks in-use (owned by caller).
  // Returns number of blocks retrieved (which may be fewer)
  int get_random_in_use_blocks(const_blockptr_t out[], int outlen) {
    CHeapBitMap bm(_owned_by_caller.size(), mtTest, false);
    bm.set_from(_owned_by_caller);
    int num = 0;
    BitMap::idx_t idx = 0;
    while (num < outlen) {
      idx = get_random_bit<true>(bm);
      if (idx == bm.size()) {
        return num;
      }
      out[num] = _slab_allocator.index_to_block((int)idx);
      bm.clear_bit(idx);
      num ++;
    }
    return num;
  }

  // Helper function for test_tick_busy:
  // Keep cache busy by allocating something
  // (but avoid overflow and underflow, count should not change, and nothing should have been
  //  allocated/deallocated from the slab)
  void keep_cache_busy() {
    _slab_allocator.reset_counters();
    const int c1 = _block_cache.count();
    if (c1 > 0) {
      const const_blockptr_t b = _block_cache.allocate_block();
      assert(b != nullptr, "Sanity");
      _block_cache.deallocate_block(b);
    } else {
      const const_blockptr_t b = get_random_in_use_block();
      assert(b != nullptr, "Sanity");
      _block_cache.deallocate_block(b);
      const const_blockptr_t b2 = _block_cache.allocate_block();
      assert(b2 == b, "Sanity");
    }
    const int c2 = _block_cache.count();
    ASSERT_EQ(c1, c2);
    ASSERT_EQ(_slab_allocator.last_allocated(), 0);
    ASSERT_EQ(_slab_allocator.last_deallocated(), 0);
  }

  // We don't directly track the in-cache blocks since the logic of
  // proactively evicting and prefetching is difficult to model. We
  // instead re-calculate them after allocations or deallocations happened.
  void recalc_owned_by_cache_bitmap() {
    _owned_by_cache.set_range(0, _owned_by_cache.size());
    _owned_by_cache.set_difference(_owned_by_slab);
    _owned_by_cache.set_difference(_owned_by_caller);
    ASSERT_EQ((int)_owned_by_cache.count_one_bits(), _block_cache.count());
  }

  int predict_lowest_free_block_index() const {
    EXPECT_EQ((int)_owned_by_cache.count_one_bits(), _block_cache.count());
    // Next allocation will be satisfied from cache or from slab, depending
    // on whether the cache is empty
    const BitMap& bm = (_block_cache.count() > 0) ? _owned_by_cache : _owned_by_slab;
    const BitMap::idx_t idx = bm.find_first_set_bit(0, bm.size());
    if (idx == bm.size()) {
      return -1;
    }
    return (int) idx;
  }

  void check_block_in_use(int block_index) const {
    ASSERT_TRUE(_owned_by_caller.at(block_index));
    ASSERT_FALSE(_owned_by_cache.at(block_index));
    ASSERT_FALSE(_owned_by_slab.at(block_index));
  }

  void check_bitmaps() const {
    ASSERT_EQ(_block_cache.count(), (int)_owned_by_cache.count_one_bits());
    CHeapBitMap bm(num_blocks, mtTest, false);
    bm.set_from(_owned_by_caller);
    bm.set_intersection(_owned_by_cache);
    ASSERT_TRUE(bm.is_empty());
    bm.set_from(_owned_by_caller);
    bm.set_intersection(_owned_by_slab);
    ASSERT_TRUE(bm.is_empty());
    bm.set_from(_owned_by_cache);
    bm.set_intersection(_owned_by_slab);
    ASSERT_TRUE(bm.is_empty());
  }

public:

  Tester() :
    _slab_allocator(),
    _block_cache(_slab_allocator),
    _owned_by_caller(SlabAllocatorType::num_blocks, mtTest, true),
    _owned_by_cache(SlabAllocatorType::num_blocks, mtTest, true),
    _owned_by_slab(_slab_allocator.in_use_bitmap())
  {}

  void allocate_from_cache_and_test() {
    DEBUG_ONLY(_block_cache.verify_cache();)
    check_bitmaps();
    _slab_allocator.reset_counters();
    const int predicted_block_index = predict_lowest_free_block_index();
    const int c1 = _block_cache.count();
    const const_blockptr_t b = _block_cache.allocate_block();
    const int c2 = _block_cache.count();
    if (b != nullptr) {
      const int real_block_index = _slab_allocator.block_to_index(b);
      ASSERT_EQ(real_block_index, predicted_block_index);
      _owned_by_caller.set_bit(real_block_index);
      if (c1 == 0) {
        // Allocation succeeded
        // If cache was empty before allocation, we should have pre-fetched some entries
        ASSERT_GE(c2, 0);
        ASSERT_LE(c2, CacheType::num_prefetch_on_underflow + 1);
        ASSERT_EQ(_slab_allocator.last_allocated(), c2 + 1);
      } else {
        // If cache was not empty before allocation, we should have taken a block from the
        // cache without bothering the slab
        ASSERT_EQ(c2, c1 - 1);
        ASSERT_EQ(_slab_allocator.last_allocated(), 0);
      }
    } else {
      // Allocation failed
      // All blocks should be in use by caller. Nothing should be left in cache and in slab.
      ASSERT_EQ(c2, 0);
      ASSERT_TRUE(_owned_by_slab.is_empty());
      ASSERT_TRUE(_owned_by_cache.is_empty());
      ASSERT_TRUE(_owned_by_caller.is_full());
    }
    ASSERT_EQ(_slab_allocator.last_deallocated(), 0); // just a sanity check
    DEBUG_ONLY(_block_cache.verify_cache();)
    recalc_owned_by_cache_bitmap();
  }

  void deallocate_to_cache_and_test(int block_index = -1) {
    DEBUG_ONLY(_block_cache.verify_cache();)
    check_bitmaps();
    _slab_allocator.reset_counters();
    if (block_index == -1) {
      block_index = get_random_in_use_block_index();
      assert(block_index != -1, "no blocks in use?");
    }
    check_block_in_use(block_index);
    const int c1 = _block_cache.count();
    _block_cache.deallocate_block(_slab_allocator.index_to_block(block_index));
    const int c2 = _block_cache.count();
    _owned_by_caller.clear_bit(block_index);
    if (c1 == cachesize) {
      // overflow, should have evicted
      ASSERT_EQ(_slab_allocator.last_deallocated(), CacheType::num_evict_on_overflow + 1);
      ASSERT_EQ(c2, cachesize - CacheType::num_evict_on_overflow);
    } else {
      ASSERT_EQ(c2, c1 + 1);
    }
    DEBUG_ONLY(_block_cache.verify_cache();)
    recalc_owned_by_cache_bitmap();
  }

  void bulk_deallocate_to_cache_and_test(const_blockptr_t blocks[], int num) {
    DEBUG_ONLY(_block_cache.verify_cache();)
    check_bitmaps();
    _slab_allocator.reset_counters();
    for (int i = 0; i < num; i++) {
      const int block_index = _slab_allocator.block_to_index(blocks[i]);
      check_block_in_use(block_index);
      _owned_by_caller.clear_bit(block_index);
    }
    const int c1 = _block_cache.count();
    _block_cache.bulk_deallocate_block(blocks, num);
    const int c2 = _block_cache.count();
    const int num_overflow = (c1 + num) - cachesize;
    if (num_overflow > 0) {
      // overflow
      ASSERT_EQ(_slab_allocator.last_deallocated(), num_overflow + CacheType::num_evict_on_overflow);
      ASSERT_EQ(c2, cachesize - CacheType::num_evict_on_overflow);
    } else {
      ASSERT_EQ(c2, c1 + num);
    }
    DEBUG_ONLY(_block_cache.verify_cache();)
    recalc_owned_by_cache_bitmap();
  }

  void tick_and_test(bool busy_since_last_tick) {
    DEBUG_ONLY(_block_cache.verify_cache();)
    check_bitmaps();
    _slab_allocator.reset_counters();
    const int c1 = _block_cache.count();
    _block_cache.tick();
    const int c2 = _block_cache.count();
    if (busy_since_last_tick) {
      // We expect the tick to have backed off and only evicted
      // a single item from the cache
      const int expected_evicted = MIN2(c1, CacheType::num_evict_on_tick_busy);
      ASSERT_EQ(c2, c1 - expected_evicted);
      ASSERT_EQ(_slab_allocator.last_deallocated(), expected_evicted);
    } else {
      // A non-busy cache would have all items removed on tick
      ASSERT_EQ(c2, 0);
      ASSERT_EQ(_slab_allocator.last_deallocated(), c1);
    }
    DEBUG_ONLY(_block_cache.verify_cache();)
    recalc_owned_by_cache_bitmap();
  }

  void test_allocate() {
    set_all_free();
    ASSERT_EQ(num_owned_by_caller(), 0);
    // Allocate till we used up the slab and a bit longer
    for (int i = 0; i < SlabAllocatorType::num_blocks + 2; i++) {
      allocate_from_cache_and_test();
      // Depending on prefetch state, cache population should wobble between 0 and number of prefetchs
      ASSERT_GE(_block_cache.count(), 0);
      ASSERT_LE(_block_cache.count(), CacheType::num_prefetch_on_underflow);
      if (::testing::Test::HasFailure()) {
        return;
      }
    }
    ASSERT_EQ(num_owned_by_caller(), SlabAllocatorType::num_blocks);
  }

  void test_deallocate() {
    set_all_in_use();
    // Deallocate till we returned all blocks to the cache, sequentially, low to high
    for (int i = 0; i < SlabAllocatorType::num_blocks; i++) {
      deallocate_to_cache_and_test(i);
      if (::testing::Test::HasFailure()) {
        return;
      }
    }
  }

  // mode=0 => lo->hi, mode=1 hi->lo, mode=2 random
  template <bool test_overflow, int mode>
  void test_bulk_deallocate0() {
    set_all_in_use();
    // Test bulk deallocate without overflow
    constexpr int N = cachesize + (test_overflow ? 1 : 0);
    const_blockptr_t arr[N];
    for (int i = 0; i < N; i++) {
      int block_index = i;
      switch (mode) {
        case 0: block_index = i; break;
        case 1: block_index = N - i; break;
        case 2: block_index = _random.next(SlabAllocatorType::num_blocks); break;
        default: ShouldNotReachHere();
      }
      arr[i] = _slab_allocator.index_to_block(block_index);
    }
    const int c1 = _block_cache.count();
    bulk_deallocate_to_cache_and_test(arr, N);
    const int c2 = _block_cache.count();
    if (test_overflow) {
      ASSERT_EQ(c2, cachesize - CacheType::num_evict_on_overflow);
      ASSERT_EQ(_slab_allocator.last_deallocated(), CacheType::num_evict_on_overflow + 1);
    } else {
      ASSERT_EQ(c2, c1 + N);
      ASSERT_EQ(_slab_allocator.last_deallocated(), 0);
    }
  }

  void test_bulk_deallocate_overflow_lo_to_hi() { test_bulk_deallocate0<true, 0>(); }
  void test_bulk_deallocate_overflow_hi_to_lo() { test_bulk_deallocate0<true, 1>(); }
  void test_bulk_deallocate_overflow_random() { test_bulk_deallocate0<true, 2>(); }
  void test_bulk_deallocate_nooverflow_lo_to_hi() { test_bulk_deallocate0<false, 0>(); }
  void test_bulk_deallocate_nooverflow_hi_to_lo() { test_bulk_deallocate0<false, 1>(); }
  void test_bulk_deallocate_nooverflow_random() { test_bulk_deallocate0<false, 2>(); }

  void test_tick_nonbusy() {
    set_all_in_use();
    // Fill cache
    for (int i = 0; i < cachesize; i++) {
      deallocate_to_cache_and_test();
    }
    // Tick. Cache is not busy so it should get cleared
    tick_and_test(false);
    ASSERT_EQ(_block_cache.count(), 0);
  }

  void test_tick_busy() {
    set_all_in_use();
    // Now tick tick tick until we are empty, then
    // tick again to test on an empty cache
    int c1 = _block_cache.count();
    while (c1 > 0) {
      keep_cache_busy();
      const int c2 = _block_cache.count();
      // Count should be the same, and nothing should have been allocated/deallocated at slab
      ASSERT_EQ(c2, c1);
      ASSERT_EQ(_slab_allocator.last_allocated(), 0);
      ASSERT_EQ(_slab_allocator.last_deallocated(), 0);
      // Now tick
      tick_and_test(true);
      const int c3 = _block_cache.count();
      // One block should have been evicted
      ASSERT_EQ(c3, c2 - 1);
      ASSERT_EQ(_slab_allocator.last_deallocated(), 1);
      c1 = c3;
    }

    // One tick more, on an empty cache, should not change anything
    keep_cache_busy();
    _block_cache.tick();
    const int c5 = _block_cache.count();
    ASSERT_EQ(c5, 0);
  }

  void test_random() {
    set_all_free();
    bool did_allocate_since_last_tick = false;
    for (int cycle = 0; cycle < 10000; cycle++) {
      const int op_select = _random.next(16);
      if (op_select < 12 && num_owned_by_caller() < SlabAllocatorType::num_blocks) {
        allocate_from_cache_and_test();
        did_allocate_since_last_tick = true;
      } else if (op_select < 14 && num_owned_by_caller() > 1) {
        deallocate_to_cache_and_test();
      } else if (op_select == 14 && num_owned_by_caller() > SlabAllocatorType::num_blocks / 2) {
        constexpr int max = CacheType::max_blocks_for_bulk_add;
        const_blockptr_t arr[max];
        const int limit = _random.tosscoin() ? MIN2(max, 3) : max;
        const int n = get_random_in_use_blocks(arr, MAX2(1, (int)_random.next(limit)));
        if (n > 0) {
          bulk_deallocate_to_cache_and_test(arr, n);
        }
      } else {
        tick_and_test(did_allocate_since_last_tick);
        did_allocate_since_last_tick = false;
      }
      if (::testing::Test::HasFailure()) {
        tty->print_cr("cycle %d op %d", cycle, op_select);
        return;
      }
    }
  }
};


#define CALL_TEST(XXX) test_##XXX()

#define DEFINE_TESTS(szclass, cachesize, testname) \
TEST_VM(Arena2_BlockCache, test_ ## testname ## _ ## szclass ## _ ## cachesize) { \
  Tester<szclass, cachesize> tester; \
  tester.CALL_TEST(testname); \
}

#define DEFINE_ALL_TESTS(szclass, cachesize) \
  DEFINE_TESTS(szclass, cachesize, allocate ) \
  DEFINE_TESTS(szclass, cachesize, deallocate ) \
  DEFINE_TESTS(szclass, cachesize, bulk_deallocate_overflow_lo_to_hi ) \
  DEFINE_TESTS(szclass, cachesize, bulk_deallocate_overflow_hi_to_lo ) \
  DEFINE_TESTS(szclass, cachesize, bulk_deallocate_overflow_random ) \
  DEFINE_TESTS(szclass, cachesize, bulk_deallocate_nooverflow_lo_to_hi ) \
  DEFINE_TESTS(szclass, cachesize, bulk_deallocate_nooverflow_hi_to_lo ) \
  DEFINE_TESTS(szclass, cachesize, bulk_deallocate_nooverflow_random ) \
  DEFINE_TESTS(szclass, cachesize, tick_busy ) \
  DEFINE_TESTS(szclass, cachesize, tick_nonbusy ) \
  DEFINE_TESTS(szclass, cachesize, random )

DEFINE_ALL_TESTS(256, 15)
DEFINE_ALL_TESTS(256, 7)
DEFINE_ALL_TESTS(256, 4)
DEFINE_ALL_TESTS(256, 2)
DEFINE_ALL_TESTS(1024, 7)
DEFINE_ALL_TESTS(32768, 7)











