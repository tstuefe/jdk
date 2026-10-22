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
  CHeapBitMap _in_use;
  int _last_allocated;
  int _last_deallocated;

public:

  FakeSlabAllocator() :
    _in_use(num_blocks, mtTest, true),
    _last_allocated(0), _last_deallocated(0)
  {
  }

  void deallocate_blocks(size_t szclass0, const_blockptr_t blocks[], int len) override {
    assert(szclass0 == szclass, "wrong block class");
    for (int i = 0; i < len; i++) {
      const const_blockptr_t b = blocks[i];
      assert(b != nullptr, "block is null");
      _in_use.clear_bit(block_to_index(b));
    }
    _last_deallocated = len;
  }

  int allocate_blocks(size_t szclass0, const_blockptr_t out[], int outlen) override {
    int allocated = 0;
    BitMap::idx_t slot = 0;
    while (allocated < outlen && slot < _in_use.size()) {
      slot = _in_use.find_first_clear_bit(slot);
      if (slot < _in_use.size()) {
        out[allocated] = index_to_block(slot);
        allocated ++;
        _in_use.set_bit(slot);
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

  static const_blockptr_t  index_to_block(int idx) {
    assert(idx >= 0 && idx < num_blocks, "invalid block index");
    return (const_blockptr_t) _fake_start + (idx * szclass);
  }

  static int  block_to_index(const_blockptr_t b) {
    assert(p2u(b) >= _fake_start && p2u(b) < _fake_end, "block out of range");
    return checked_cast<int>((p2u(b) - _fake_start) / szclass);
  }

  // Sets the whole slab as "in use"
  void set_all_in_use() { _in_use.set_range(0, _in_use.size()); }
  void set_all_free() { _in_use.clear_range(0, _in_use.size()); }
};


template <size_t szclass, int cachesize>
class Tester {
  typedef FakeSlabAllocator<szclass> SlabAllocatorType;
  typedef ArenaBlockCache<szclass, cachesize> CacheType;

  SlabAllocatorType _slab_allocator;
  CacheType _block_cache;

  // We use a bitmap to mimic block state: one bit per block in slab. 1 means the
  // block is in-use by the caller (outside of cache and slab); 0 means the block
  // is in cache or slab, we don't care.
  // This allows us to mimic expected allocation behavior wrt addresses: we expect
  // the cache to return to us the lowest free block, by address - doesn't matter
  // whether it comes from the cache or from the slab allocator. So we can use
  // count leading/trailing bits for that.
  CHeapBitMap _in_use;

  PredictableRandom _random;

  // Setup world as if cache were empty and all slabs were in-use by callers
  void set_all_in_use() {
    _block_cache.flush();
    _slab_allocator.set_all_in_use();
    _in_use.set_range(0, _in_use.size());
  }

  // Setup world as if cache were empty and all slabs were free (in slab)
  void set_all_free() {
    _block_cache.flush();
    _in_use.clear_range(0, _in_use.size());
    _slab_allocator.set_all_free();
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
    return get_random_bit<true>(_in_use);
  }

  int get_random_free_block_index() {
    return get_random_bit<false>(_in_use);
  }

  // Helper function for test_tick_busy:
  // Keep cache busy by allocating something
  // (but avoid overflow and underflow, count should not change, and nothing should have been
  //  allocated/deallocated from the slab)
  void keep_cache_busy() {
    _slab_allocator.reset_counters();
    const int c1 = _block_cache.count();
    if (c1 > 0) {
      allocate_from_cache_and_test();
      deallocate_to_cache_and_test();
    } else {
      deallocate_to_cache_and_test();
      allocate_from_cache_and_test();
    }
    const int c2 = _block_cache.count();
    ASSERT_EQ(c1, c2);
    ASSERT_EQ(_slab_allocator.last_allocated(), 0);
    ASSERT_EQ(_slab_allocator.last_deallocated(), 0);
  }

public:

  Tester() :
    _slab_allocator(),
    _block_cache(_slab_allocator),
    _in_use(SlabAllocatorType::num_blocks, mtTest, true)
  {}

  void allocate_from_cache_and_test(int* p_block_index = nullptr) {
    DEBUG_ONLY(_block_cache.verify_cache();)
    const int c1 = _block_cache.count();
    const const_blockptr_t b = _block_cache.allocate_block();
    const BitMap::idx_t lowest_free_slot = _in_use.find_first_clear_bit(0);
    if (lowest_free_slot == _in_use.size()) {
      // There should be no free block left
      ASSERT_EQ(b, (const_blockptr_t) nullptr);
      ASSERT_EQ(c1, 0);
    } else {
      // There should be blocks left; we should have gotten the lowest-address one
      ASSERT_EQ(b, _slab_allocator.index_to_block(checked_cast<int>(lowest_free_slot)));
      _in_use.set_bit(lowest_free_slot);
      if (c1 > 0) {
        ASSERT_EQ(_block_cache.count(), c1 - 1);
      } else {
        // Did we refill the cache?
        if (_slab_allocator.last_allocated() > 0) {
          // Note: we prefetched N items, put N-1 into the cache and returned one
          ASSERT_EQ(_block_cache.count(), _slab_allocator.last_allocated() - 1);
        } else {
          ASSERT_EQ(_block_cache.count(), 0);
        }
      }
    }
    DEBUG_ONLY(_block_cache.verify_cache();)
  }

  void deallocate_to_cache_and_test(int block_index = -1) {
    DEBUG_ONLY(_block_cache.verify_cache();)
    if (block_index == -1) {
      block_index = get_random_in_use_block_index();
      assert(block_index != -1, "no blocks in use?");
    }
    assert(_in_use.at(block_index), "block must be in use");
    const int c1 = _block_cache.count();
    _block_cache.deallocate_block(_slab_allocator.index_to_block(block_index));
    const int c2 = _block_cache.count();
    _in_use.clear_bit(block_index);
    if (c1 == cachesize) {
      // overflow, should have evicted
      ASSERT_EQ(CacheType::num_evict_on_overflow + 1, _slab_allocator.last_deallocated());
      ASSERT_EQ(c2, cachesize - CacheType::num_evict_on_overflow);
    } else {
      ASSERT_EQ(c2, c1 + 1);
    }
    DEBUG_ONLY(_block_cache.verify_cache();)
  }

  void bulk_deallocate_to_cache_and_test(const_blockptr_t blocks[], int num) {
    DEBUG_ONLY(_block_cache.verify_cache();)
    const int c1 = _block_cache.count();
    for (int i = 0; i < num; i++) {
      const int block_index = _slab_allocator.block_to_index(blocks[i]);
      assert(_in_use.at(block_index), "block must be in use");
      _in_use.clear_bit(block_index);
    }
    _block_cache.bulk_deallocate_block(blocks, num);
    const int c2 = _block_cache.count();
    const int num_overflow = (c1 + num) - cachesize;
    if (num_overflow > 0) {
      // overflow
      ASSERT_GE(_slab_allocator.last_deallocated(), num_overflow);
      ASSERT_LE(_slab_allocator.last_deallocated(), num_overflow + CacheType::num_evict_on_overflow);
      ASSERT_GE(c2, cachesize - CacheType::num_evict_on_overflow);
      ASSERT_LE(c2, cachesize);
    } else {
      ASSERT_EQ(c2, c1 + num);
    }
    DEBUG_ONLY(_block_cache.verify_cache();)
  }

  void tick_and_test(bool busy_since_last_tick) {
    DEBUG_ONLY(_block_cache.verify_cache();)
    _slab_allocator.reset_counters();
    const int c1 = _block_cache.count();
    _block_cache.tick();
    const int c2 = _block_cache.count();
    if (busy_since_last_tick) {
      // We expect the tick to have backed off and only evicted
      // a single item from the cache
      if (c1 > 0) { // non-empty cache
        ASSERT_EQ(c2, c1 - 1);
        ASSERT_EQ(_slab_allocator.last_deallocated(), 1);
      } else { // empty cache
        ASSERT_EQ(c2, 0);
        ASSERT_EQ(_slab_allocator.last_deallocated(), 0);
      }
    } else {
      // A non-busy cache would have all items removed on tick
      ASSERT_EQ(c2, 0);
      ASSERT_EQ(_slab_allocator.last_deallocated(), c1);
    }
    DEBUG_ONLY(_block_cache.verify_cache();)
  }

  void test_allocate() {
    set_all_free();
    ASSERT_EQ((int)_in_use.count_one_bits(), 0);
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
    ASSERT_EQ((int)_in_use.count_one_bits(), SlabAllocatorType::num_blocks);
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
    int num_in_use = 0;
    bool did_allocate_since_last_tick = false;
    for (int cycle = 0; cycle < 10000; cycle++) {
      assert(num_in_use >= 0 && num_in_use < SlabAllocatorType::num_blocks, "Sanity");
      const int op_select = _random.next(16);
      if (op_select < 12 && num_in_use < SlabAllocatorType::num_blocks) {
        allocate_from_cache_and_test();
        num_in_use++;
        did_allocate_since_last_tick = true;
      } else if (op_select < 14 && num_in_use > 1) {
        deallocate_to_cache_and_test();
        num_in_use--;
      } else if (op_select == 14 && num_in_use > SlabAllocatorType::num_blocks / 2) {
        const_blockptr_t arr[CacheType::max_blocks_for_bulk_add];
        const bool odd = _random.next(2) > 0;
        const int num_blocks = (int)_random.next(odd ? CacheType::max_blocks_for_bulk_add : 4);
        CHeapBitMap bm2(_in_use.size(), mtTest, false);
        bm2.set_from(_in_use);
        for (int i = 0; i < num_blocks; i++) {
          const BitMap::idx_t slot = get_random_bit<true>(bm2);
          if (slot < bm2.size()) {
            bm2.clear_bit(slot);
            arr[i] = _slab_allocator.index_to_block((int)slot);
          } else {
            break;
          }
        }
        bulk_deallocate_to_cache_and_test(arr, num_blocks);
        num_in_use -= num_blocks;
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











