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
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/macros.hpp"
#include "utilities/ostream.hpp"

#include "unittest.hpp"

#include "test_arena_helpers.hpp"

//#define LOG_PLEASE
#include "testutils.hpp"
#include "memory/arena/arenaSlab.hpp"


namespace {
  // This class models expected block movements in between slab, cache and caller-space
  template <size_t blocksize, int cachesize>
  class AllocationPredictionModel {

    using idx_t = BitMap::idx_t;

    const unsigned _num_blocks;
    CHeapBitMap _owned_by_caller;
    CHeapBitMap _owned_by_cache;
    CHeapBitMap _owned_by_slab;
    BlockRange<unsigned> _block_range;
    bool _busy;

    unsigned num_blocks() const { return _num_blocks; }

#ifdef ASSERT
    void verify() const {
      assert(!_owned_by_slab.intersects(_owned_by_caller), "Sanity");
      assert(!_owned_by_slab.intersects(_owned_by_cache), "Sanity");
      assert(!_owned_by_caller.intersects(_owned_by_cache), "Sanity");
      assert(_num_blocks == num_in_slab() + num_in_cache() + num_owned_by_caller(), "Sanity");
    }
#endif

    static idx_t find_lowest_address_block(const BitMap& bm) {
      return bm.find_first_set_bit(0, bm.size());
    }

    static idx_t find_lowest_address_block_expect_success(const BitMap& bm) {
      const idx_t idx = find_lowest_address_block(bm);
      assert(idx != bm.size(), "Sanity");
      return idx;
    }

    static idx_t find_highest_address_block(const BitMap& bm) {
      return bm.find_last_set_bit(0, bm.size());
    }

    static idx_t find_highest_address_block_expect_success(const BitMap& bm) {
      const idx_t idx = find_highest_address_block(bm);
      assert(idx != bm.size(), "Sanity");
      return idx;
    }

    // model deallocation of a bunch of blocks, given by a bitmap
    void model_deallocation(const BitMap& deallocated_blocks) {
      DEBUG_ONLY(verify());
      assert(!_owned_by_cache.intersects(deallocated_blocks), "Sanity");
      assert(!_owned_by_slab.intersects(deallocated_blocks), "Sanity");

      _owned_by_cache.set_union(deallocated_blocks);
      if (num_in_cache() > cachesize) {
        // Will overflow cache. Cache will evict. Use the union of cache
        // blocks and deallocated blocks to choose those N with the highest
        // address, which will move to the slab
        const int to_keep = cachesize - ArenaBlockCache::num_evict_on_overflow(cachesize);
        const int to_evict = num_in_cache() - to_keep;
        idx_t idx = num_blocks();
        for (int i = 0; i < to_evict; ++i) {
          const idx_t idx2 = _owned_by_cache.find_last_set_bit(0, idx);
          assert(idx2 != idx, "Sanity");
          _owned_by_slab.set_bit(idx2);
          _owned_by_cache.clear_bit(idx2);
          idx = idx2;
        }
      }
      _owned_by_caller.set_difference(deallocated_blocks);
    }

  public:

    AllocationPredictionModel(unsigned num_blocks, const_address block_base) :
      _num_blocks(num_blocks),
      _owned_by_caller(num_blocks, mtTest, true),
      _owned_by_cache(num_blocks, mtTest, true),
      _owned_by_slab(num_blocks, mtTest, true),
      _block_range(block_base, blocksize, num_blocks),
      _busy(false)
    {
      // At the start, all blocks are owned by the slab
      _owned_by_slab.set_range(0, num_blocks);
    }

    void init_all_owned_by_caller() {
      _owned_by_caller.set_range(0, num_blocks());
      _owned_by_cache.clear_range(0, num_blocks());
      _owned_by_slab.clear_range(0, num_blocks());
      _busy = false;
    }

    void init_all_owned_by_slab() {
      _owned_by_caller.clear_range(0, num_blocks());
      _owned_by_cache.clear_range(0, num_blocks());
      _owned_by_slab.set_range(0, num_blocks());
      _busy = false;
    }

    unsigned num_in_slab() const { return _owned_by_slab.count_one_bits(); }
    unsigned num_in_cache() const { return _owned_by_cache.count_one_bits(); }
    unsigned num_owned_by_caller() const { return _owned_by_caller.count_one_bits(); }

    // Model allocation: Update bitmaps and return pointer of the block the allocation would
    // return.
    const_blockptr_t model_allocation() {
      DEBUG_ONLY(verify());
      _busy = true;

      if (_owned_by_caller.is_full()) {
        // all slabs are allocated, nothing free
        return nullptr;
      }

      if (num_in_cache() > 0) {
        // return the lowest-address block from the cache
        const BitMap::idx_t idx = find_lowest_address_block_expect_success(_owned_by_cache);
        _owned_by_cache.clear_bit(idx);
        _owned_by_caller.set_bit(idx);
        return _block_range.index_to_block(checked_cast<int>(idx));
      }

      // cache is empty; fetch from slab (and prefetch and pre-fill cache, too)
      assert(_owned_by_cache.is_empty(), "Sanity");
      assert(num_in_slab() > 0, "Sanity");
      const unsigned num_to_fetch_from_slab = MIN2(num_in_slab(),
        1u + ArenaBlockCache::num_prefetch_on_underflow(cachesize));
      for (unsigned i = 0; i < num_to_fetch_from_slab; ++i) {
        const idx_t idx = find_lowest_address_block_expect_success(_owned_by_slab);
        _owned_by_slab.clear_bit(idx);
        _owned_by_cache.set_bit(idx);
      }

      // we return the lowest-address one of the newly fetched blocks
      const idx_t idx = find_lowest_address_block_expect_success(_owned_by_cache);
      _owned_by_cache.clear_bit(idx);
      _owned_by_caller.set_bit(idx);

      return _block_range.index_to_block(checked_cast<int>(idx));
    }

    // model deallocation of a single block
    void model_deallocation(const_blockptr_t block) {
      DEBUG_ONLY(verify());
      const idx_t idx = _block_range.block_to_index(block);
      assert(_owned_by_caller.at(idx), "Sanity");
      CHeapBitMap tmp(num_blocks(), mtTest, true);
      tmp.set_bit(idx);
      model_deallocation(tmp);
    }

    // model deallocation of a bunch of blocks
    void model_bulk_deallocation(const const_blockptr_t block[], int num) {
      DEBUG_ONLY(verify());
      CHeapBitMap tmp(num_blocks(), mtTest, true);
      for (int i = 0; i < num; ++i) {
        const idx_t idx = _block_range.block_to_index(block[i]);
        assert(_owned_by_caller.at(idx), "Sanity");
        tmp.set_bit(idx);
      }
      model_deallocation(tmp);
    }

    // Model a tick.
    void model_tick() {
      DEBUG_ONLY(verify());
      // On each tick, cache will evict some of its highest-address blocks
      const unsigned to_evict =
          MIN2(num_in_cache(), (unsigned)(_busy ? ArenaBlockCache::num_evict_on_tick_busy(cachesize) :
                                                        ArenaBlockCache::num_evict_on_tick_nonbusy(cachesize)));
      for (unsigned i = 0; i < to_evict; ++i) {
        const idx_t idx = find_highest_address_block_expect_success(_owned_by_cache);
        _owned_by_cache.clear_bit(idx);
        _owned_by_slab.set_bit(idx);
      }
      _busy = false;
    }

    void reality_test(const ArenaBlockCache& cache) const {
      // Build up a bitmap that reflects the cache content
      CHeapBitMap tmp(num_blocks(), mtTest, true);
      const BlockRange<unsigned>& blockrange2 = _block_range;
      auto set_bit_for_block_in_cache = [&tmp,&blockrange2](const_blockptr_t block) {
        const idx_t idx = blockrange2.block_to_index(block);
        tmp.set_bit(idx);
      };
      cache.iterate(set_bit_for_block_in_cache);
      // compare it against our version
      ASSERT_TRUE(_owned_by_cache.is_same(tmp));
    }

    void reality_test(const ArenaSlab& slab) const {
      CHeapBitMap tmp(num_blocks(), mtTest, true);
      slab.get_in_use_map(tmp);
      CHeapBitMap tmp2(num_blocks(), mtTest, true);
      tmp2.set_range(0, num_blocks());
      tmp2.set_difference(tmp);
      ASSERT_TRUE(_owned_by_slab.is_same(tmp2));
    }

    // Retrieve a single block, randomly, that is in-use by caller;
    // nullptr if none are in use
    const_blockptr_t get_random_caller_owned_block(PredictableRandom& rand) const {
      const idx_t idx = ArenaTestHelpers::get_random_set_bit(rand, _owned_by_caller);
      return idx == num_blocks() ? nullptr : _block_range.index_to_block(idx);
    }

    // Retrieve an array of unique random blocks that are in use by caller;
    // returns number of blocks found.
    int get_random_caller_owned_blocks(PredictableRandom& rand, const_blockptr_t out[], int outlen) const {
      BitMap::idx_t* arr = NEW_C_HEAP_ARRAY(BitMap::idx_t, outlen, mtTest);
      const int num = ArenaTestHelpers::get_random_set_bits(rand, _owned_by_caller, arr, outlen);
      for (int i = 0; i < num; ++i) {
        out[i] = _block_range.index_to_block(arr[i]);
      }
      FREE_C_HEAP_ARRAY(arr);
      return num;
    }

  }; // end AllocationPredictionModel
}

namespace {
  template <int cachesize>
  class Tester {
    // For the slab. We don't test the slab in this test, so these values only matter
    // are a valid slab configuration
    static constexpr ArenaSlab::Parameters _params = {
      32 * K, ArenaHeapBase::large_page_size, 1024, 0, false, 0.0f
    };

    typedef AllocationPredictionModel<_params.block_size, cachesize> ModelType;

    ArenaHeap _arena_heap;
    ArenaMetadataHeap _metadata_heap;
    ArenaSlab _slab;
    ArenaBlockCache _block_cache;
    PredictableRandom _random;
    BlockRange<unsigned> _block_range;
    ModelType _model;

    size_t block_size() const { return _params.block_size; }
    size_t granule_size() const { return _params.granule_size; }
    unsigned num_blocks() const { return _params.num_blocks; }

    // Setup world as if cache were empty and all slabs were in-use by callers
    void set_all_allocated() {
      _block_cache.flush();
      _model.init_all_owned_by_caller();
      const unsigned saturate = num_blocks() + 1;
      const_blockptr_t* dontcare = NEW_C_HEAP_ARRAY(const_blockptr_t, saturate, mtTest);
      const unsigned fetched = _slab.allocate_blocks(dontcare, saturate);
      ASSERT_EQ(fetched, num_blocks());
      FREE_C_HEAP_ARRAY(dontcare);
    }

    // Helper function for test_tick_busy:
    // Keep cache busy by allocating something
    // (but avoid overflow and underflow, count should not change, and nothing should have been
    //  allocated/deallocated from the slab)
    void keep_cache_busy() {
      const int c1 = _block_cache.count();
      if (c1 > 0) {
        const const_blockptr_t b = allocate_from_cache_and_test_expect_success();
        deallocate_to_cache_and_test(b);
      } else {
        const const_blockptr_t b = _model.get_random_caller_owned_block(_random);
        ASSERT_NE(b, nullptr);
        deallocate_to_cache_and_test(b);
        const const_blockptr_t b2 = allocate_from_cache_and_test_expect_success();
        ASSERT_EQ(b, b2);
      }
      const int c2 = _block_cache.count(); // nothing should have changed
      ASSERT_EQ(c1, c2);
    }

    void reality_test() const {
      _model.reality_test(_block_cache);
      _model.reality_test(_slab);
    }

    const_blockptr_t allocate_from_cache() {
      DEBUG_ONLY(_block_cache.verify_cache();)
      reality_test();

      const const_blockptr_t prediction = _model.model_allocation();
      const const_blockptr_t b = _block_cache.allocate_block();
      EXPECT_EQ(prediction, b);

      reality_test();
      DEBUG_ONLY(_block_cache.verify_cache();)

      return b;
    }

  public:

    Tester() :
      _arena_heap(), _metadata_heap(),
      _slab(&_arena_heap, &_metadata_heap, &_params),
      _block_cache(&_slab, cachesize),
      _random(),
      _block_range(_slab.base(), _params.block_size, num_blocks()),
      _model(num_blocks(), _slab.base()) {
    }

    const_blockptr_t allocate_from_cache_and_test_expect_success() {
      const const_blockptr_t b = allocate_from_cache();
      EXPECT_NE(b, nullptr);
      return b;
    }

    void allocate_from_cache_and_test_expect_error() {
      const const_blockptr_t b = allocate_from_cache();
      ASSERT_EQ(b, nullptr);
    }

    void deallocate_to_cache_and_test(const_blockptr_t block = nullptr) {
      const_blockptr_t b = (block == nullptr) ? _model.get_random_caller_owned_block(_random) : block;
      bulk_deallocate_to_cache_and_test(&b, 1);
    }

    void bulk_deallocate_to_cache_and_test(const_blockptr_t blocks[], int num) {
      DEBUG_ONLY(_block_cache.verify_cache();)
      reality_test();

      _model.model_bulk_deallocation(blocks, num);
      _block_cache.bulk_deallocate_block(blocks, num);

      reality_test();
      DEBUG_ONLY(_block_cache.verify_cache();)
    }

    void tick_and_test() {
      DEBUG_ONLY(_block_cache.verify_cache();)
      reality_test();

      _model.model_tick();
      _block_cache.tick();

      reality_test();
      DEBUG_ONLY(_block_cache.verify_cache();)
    }

    ///////////////

    void test_allocate() {
      // Allocate till we used up the slab
      for (unsigned i = 0; i < num_blocks(); i++) {
        allocate_from_cache_and_test_expect_success();
        if (::testing::Test::HasFailure()) {
          return;
        }
      }
      // Allocate from empty slab
      allocate_from_cache_and_test_expect_error();
    }

    void test_deallocate() {
      set_all_allocated();
      // Deallocate till we returned all blocks to the cache, sequentially, low to high
      for (unsigned i = 0; i < num_blocks(); i++) {
        deallocate_to_cache_and_test(_block_range.index_to_block(i));
        if (::testing::Test::HasFailure()) {
          return;
        }
      }
    }

    // mode=0 => lo->hi, mode=1 hi->lo, mode=2 random
    template <bool test_overflow, int mode>
    void test_bulk_deallocate0() {
      set_all_allocated();
      // Test bulk deallocate without overflow
      constexpr int N = test_overflow ?
                          ArenaBlockCache::max_blocks_for_bulk_deallocate : cachesize;
      STATIC_ASSERT(!test_overflow || N > cachesize);
      const_blockptr_t arr[N];
      switch(mode) {
        case 0:
          for (int i = 0; i < N; i++) {
            arr[i] = _block_range.index_to_block(i);
          }
          break;
        case 1:
          for (int i = 0; i < N; i++) {
            arr[N - i - 1] = _block_range.index_to_block(i);
          }
          break;
        case 2: {
          const int n = _model.get_random_caller_owned_blocks(_random, arr, N);
          ASSERT_EQ(n, N);
        }
        break;
        default: ShouldNotReachHere();
      }
      bulk_deallocate_to_cache_and_test(arr, N);
    }

    void test_bulk_deallocate_overflow_lo_to_hi() { test_bulk_deallocate0<true, 0>(); }
    void test_bulk_deallocate_overflow_hi_to_lo() { test_bulk_deallocate0<true, 1>(); }
    void test_bulk_deallocate_overflow_random() { test_bulk_deallocate0<true, 2>(); }
    void test_bulk_deallocate_nooverflow_lo_to_hi() { test_bulk_deallocate0<false, 0>(); }
    void test_bulk_deallocate_nooverflow_hi_to_lo() { test_bulk_deallocate0<false, 1>(); }
    void test_bulk_deallocate_nooverflow_random() { test_bulk_deallocate0<false, 2>(); }

    void test_tick_nonbusy() {
      set_all_allocated();

      // Fill cache
      for (int i = 0; i < cachesize; i++) {
        deallocate_to_cache_and_test();
      }

      // Tick. Cache is not busy so it should get cleared
      tick_and_test();
      ASSERT_EQ(_block_cache.count(), 0);
    }

    void test_tick_busy() {
      set_all_allocated();

      // Fill cache
      for (int i = 0; i < cachesize; i++) {
        deallocate_to_cache_and_test();
      }

      // Now tick tick tick until we are empty, then
      // tick again to test on an empty cache
      int c1 = _block_cache.count();
      while (c1 > 0) {
        keep_cache_busy();
        tick_and_test();
        const int c2 = _block_cache.count();
        // One block should have been evicted
        ASSERT_EQ(c2, c1 - 1);
        c1 = c2;
      }

      // One tick more, on an empty cache, should not change anything
      keep_cache_busy();
      tick_and_test();
      const int c3 = _block_cache.count();
      ASSERT_EQ(c3, 0);
    }

    void test_random() {
      for (int cycle = 0; cycle < 10000; cycle++) {
        const int op_select = _random.next(16);
        if (op_select < 12 && _model.num_owned_by_caller() < num_blocks()) {
          allocate_from_cache_and_test_expect_success();
        } else if (op_select < 14 && _model.num_owned_by_caller() > 0) {
          deallocate_to_cache_and_test();
        } else if (op_select == 14 && _model.num_owned_by_caller() > num_blocks() / 2) {
          constexpr int max = ArenaBlockCache::max_blocks_for_bulk_deallocate;
          const_blockptr_t arr[max];
          const int limit = _random.tosscoin() ? MIN2(max, 3) : max;
          const int num = 1 + (int)_random.next(limit);
          const int n = _model.get_random_caller_owned_blocks(_random, arr, num);
          assert(n > 0, "Sanity");
          bulk_deallocate_to_cache_and_test(arr, n);
        } else {
          tick_and_test();
        }
        if (::testing::Test::HasFailure()) {
          tty->print_cr("cycle %d op %d", cycle, op_select);
          return;
        }
      }
    }
  };
}


#define CALL_TEST(XXX) test_##XXX()

#define DEFINE_TESTS(cachesize, testname) \
TEST_VM(Arena2_BlockCache, test_ ## testname ## _ ## cachesize) { \
  Tester<cachesize> tester; \
  tester.CALL_TEST(testname); \
}

#define DEFINE_ALL_TESTS(cachesize) \
  DEFINE_TESTS(cachesize, allocate ) \
  DEFINE_TESTS(cachesize, deallocate ) \
  DEFINE_TESTS(cachesize, bulk_deallocate_overflow_lo_to_hi ) \
  DEFINE_TESTS(cachesize, bulk_deallocate_overflow_hi_to_lo ) \
  DEFINE_TESTS(cachesize, bulk_deallocate_overflow_random ) \
  DEFINE_TESTS(cachesize, bulk_deallocate_nooverflow_lo_to_hi ) \
  DEFINE_TESTS(cachesize, bulk_deallocate_nooverflow_hi_to_lo ) \
  DEFINE_TESTS(cachesize, bulk_deallocate_nooverflow_random ) \
  DEFINE_TESTS(cachesize, tick_busy ) \
  DEFINE_TESTS(cachesize, tick_nonbusy ) \
  DEFINE_TESTS(cachesize, random )

DEFINE_ALL_TESTS(7)
DEFINE_ALL_TESTS(5)
DEFINE_ALL_TESTS(2)
DEFINE_ALL_TESTS(1)