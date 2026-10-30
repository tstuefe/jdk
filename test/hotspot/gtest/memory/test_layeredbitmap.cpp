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
#include "memory/arena/layeredBitmap.hpp"
#include "runtime/os.hpp"
#include "utilities/bitMap.inline.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/macros.hpp"
#include "unittest.hpp"

//#define LOG_PLEASE
#include "testutils.hpp"

#define VERIFICATION_INTERVAL 3

namespace {
  struct Tester : public CHeapObj<mtTest> {
    typedef LayeredBitmap BitmapType;
    typedef LayeredBitmap::IndexType IndexType;

    const size_t _storage_size;
    const address _storage;
    BitmapType _layered_bitmap;

    BitMap::bm_word_t _test_bitmap_storage[LayeredBitmap::max_num_bits() / BitsPerWord];
    BitMapView _test_bitmap;
    PredictableRandom _random;
    mutable int _sometimes;
    static constexpr unsigned num_bits_per_segment = BitsPerWord;

    static size_t calc_storage_size(unsigned num_bits) {
      return align_up(LayeredBitmap::bytes_needed(num_bits) + 1, // canary
             os::vm_page_size());
    }

    static address allocate_storage_for_bitmap(unsigned num_bits) {
      const size_t needed = calc_storage_size(num_bits);
      const address storage = (address)os::reserve_memory(needed, mtTest, false);
      assert(storage != nullptr, "oom");
      const bool b = os::commit_memory((char*)storage, needed, false);
      assert(b, "OOM");
      return storage;
    }

    explicit Tester(unsigned num_bits) :
      _storage_size(calc_storage_size(num_bits)),
      _storage(allocate_storage_for_bitmap(num_bits)),
      _layered_bitmap(_storage, _storage_size, num_bits),
      _test_bitmap_storage {0},
      _test_bitmap(_test_bitmap_storage, num_bits),
      _random(),
      _sometimes(0)
    {
      assert(num_bits <= LayeredBitmap::max_num_bits(), "num_bits too big");
      const size_t canary_pos = LayeredBitmap::bytes_needed(num_bits);
      _storage[canary_pos] = 'A';
      _test_bitmap.clear();
    }

    ~Tester() {
      const size_t canary_pos = LayeredBitmap::bytes_needed(_layered_bitmap.num_bits());
      EXPECT_EQ(_storage[canary_pos], 'A');
      os::release_memory((char*)_storage, _storage_size);
    }

    unsigned num_bits() const {
      return _layered_bitmap.num_bits();
    }

    void sometimes_verify() const {
#ifdef ASSERT
      if ((_sometimes++ % VERIFICATION_INTERVAL) == 0) {
        _layered_bitmap.verify();
      }
#endif // ASSERT
    }

    void check_equality() const {
      CHeapBitMap tmp(num_bits(), mtTest, false);
      _layered_bitmap.convert_to_flat_bitmap(tmp);
      ASSERT_TRUE(_test_bitmap.is_same(tmp));
    }

#define POST_CHECK \
    check_equality(); \
    sometimes_verify(); \

#define VOID_XXX_AND_VERIFY_0(function) \
  void function ## _and_verify() { \
    _layered_bitmap.function(); \
    _test_bitmap.function(); \
    POST_CHECK \
  }

#define VOID_XXX_AND_VERIFY_1(function, ArgType, arg) \
  void function ## _and_verify(ArgType arg) { \
    _layered_bitmap.function(arg); \
    _test_bitmap.function(arg); \
    POST_CHECK \
  }

#define VOID_XXX_AND_VERIFY_2(function, ArgType1, arg1, ArgType2, arg2) \
  void function ## _and_verify(ArgType1 arg1, ArgType2 arg2) { \
    _layered_bitmap.function(arg1, arg2); \
    _test_bitmap.function(arg1, arg2); \
    POST_CHECK \
  }

#define NONVOID_XXX_AND_VERIFY_0(function, ReturnType) \
  ReturnType function ## _and_verify() { \
    const ReturnType v1 = _layered_bitmap.function(); \
    const ReturnType v2 = _test_bitmap.function(); \
    EXPECT_EQ(v1, v2); \
    POST_CHECK \
    return v1; \
  }

#define NONVOID_XXX_AND_VERIFY_1(function, ReturnType, ArgType, arg) \
  ReturnType function ## _and_verify(ArgType arg) { \
    const ReturnType v1 = _layered_bitmap.function(arg); \
    const ReturnType v2 = _test_bitmap.function(arg); \
    EXPECT_EQ(v1, v2); \
    POST_CHECK \
    return v1; \
  }

#define NONVOID_XXX_AND_VERIFY_2(function, ReturnType, ArgType1, arg1, ArgType2, arg2) \
  ReturnType function ## _and_verify(ArgType1 arg1, ArgType2 arg2) { \
    const ReturnType v1 = _layered_bitmap.function(arg1, arg2); \
    const ReturnType v2 = _test_bitmap.function(arg1, arg2); \
    EXPECT_EQ(v1, v2); \
    POST_CHECK \
    return v1; \
  }

    VOID_XXX_AND_VERIFY_1(set_bit, IndexType, bitno);
    VOID_XXX_AND_VERIFY_1(clear_bit, IndexType, bitno);
    VOID_XXX_AND_VERIFY_2(set_range, IndexType, beg, IndexType, end);
    VOID_XXX_AND_VERIFY_2(clear_range, IndexType, beg, IndexType, end);

    NONVOID_XXX_AND_VERIFY_2(find_first_set_bit, IndexType, IndexType, beg, IndexType, end);
    NONVOID_XXX_AND_VERIFY_2(find_first_clear_bit, IndexType, IndexType, beg, IndexType, end);
    NONVOID_XXX_AND_VERIFY_2(find_last_set_bit, IndexType, IndexType, beg, IndexType, end);
    NONVOID_XXX_AND_VERIFY_2(find_last_clear_bit, IndexType, IndexType, beg, IndexType, end);
    NONVOID_XXX_AND_VERIFY_2(count_one_bits, IndexType, IndexType, beg, IndexType, end);

    NONVOID_XXX_AND_VERIFY_1(at, bool, IndexType, idx);

    VOID_XXX_AND_VERIFY_0(clear);
    NONVOID_XXX_AND_VERIFY_0(is_empty, bool);
    NONVOID_XXX_AND_VERIFY_0(is_full, bool);

    NONVOID_XXX_AND_VERIFY_1(is_same, bool, const BitMap&, other);
    NONVOID_XXX_AND_VERIFY_1(set_union_with_result, bool, const BitMap&, other);
    NONVOID_XXX_AND_VERIFY_1(set_difference_with_result, bool, const BitMap&, other);
    NONVOID_XXX_AND_VERIFY_1(set_intersection_with_result, bool, const BitMap&, other);
    NONVOID_XXX_AND_VERIFY_1(contains, bool, const BitMap&, other);
    NONVOID_XXX_AND_VERIFY_1(intersects, bool, const BitMap&, other);
    VOID_XXX_AND_VERIFY_1(set_from, const BitMap&, other);

    void fill_with_pattern(BitMap::bm_word_t bit_pattern) {
      _layered_bitmap.propagate_bit_pattern(bit_pattern);
      for (IndexType i = 0; i < _test_bitmap.size_in_words(); i++) {
        _test_bitmap_storage[i] = bit_pattern;
      }
      assert(_layered_bitmap.is_same(_test_bitmap), "Sanity");
    }

    bool is_only_range_set(IndexType beg, IndexType end) {
      IndexType idx = find_first_set_bit_and_verify(0, num_bits());
      if (idx != beg) {
        return false;
      }
      idx = find_first_clear_bit_and_verify(idx, num_bits());
      if (idx != end) {
        return false;
      }
      idx = find_first_set_bit_and_verify(idx, num_bits());
      return idx == num_bits();
    }

    void basic_test() {

      // set and clear first bit
      set_bit_and_verify(0);
      ASSERT_EQ((IndexType)1, _layered_bitmap.count_one_bits());
      ASSERT_TRUE(is_only_range_set(0, 1));
      clear_bit_and_verify(0);
      ASSERT_EQ((IndexType)0, _layered_bitmap.count_one_bits());
      ASSERT_TRUE(_layered_bitmap.is_empty());

      // set and clear bit in second segment
      if (num_bits() > num_bits_per_segment + 2) {
        set_bit_and_verify(num_bits_per_segment);
        ASSERT_TRUE(is_only_range_set(num_bits_per_segment,
                                      num_bits_per_segment + 1));
        clear_bit_and_verify(_layered_bitmap.num_bits_per_segment());
        ASSERT_TRUE(_layered_bitmap.is_empty());
      }

      // set and clear last bit
      set_bit_and_verify(num_bits() - 1);
      ASSERT_TRUE(is_only_range_set(num_bits() - 1, num_bits()));
      clear_bit_and_verify(num_bits() - 1);
      ASSERT_TRUE(_layered_bitmap.is_empty());

      // set all
      set_range_and_verify(0, num_bits());
      ASSERT_TRUE(_layered_bitmap.is_full());
      ASSERT_TRUE(is_only_range_set(0, num_bits()));
      ASSERT_FALSE(_layered_bitmap.is_empty());
      ASSERT_EQ(_layered_bitmap.count_one_bits(), num_bits());

      // clear all
      clear_range_and_verify(0, num_bits());
      ASSERT_FALSE(_layered_bitmap.is_full());
      ASSERT_TRUE(_layered_bitmap.is_empty());
      ASSERT_EQ(_layered_bitmap.count_one_bits(), 0u);

      // set range with empty range
      set_range_and_verify(0, 0);
      ASSERT_TRUE(_layered_bitmap.is_empty()); // nothing changed
      set_range_and_verify(num_bits() / 2, num_bits() / 2);
      ASSERT_TRUE(_layered_bitmap.is_empty()); // nothing changed
      set_range_and_verify(num_bits(), num_bits());
      ASSERT_TRUE(_layered_bitmap.is_empty()); // nothing changed

      // find first set with empty range
      set_range_and_verify(0, num_bits());
      ASSERT_TRUE(_layered_bitmap.is_full());
      ASSERT_EQ(_layered_bitmap.find_first_set_bit(num_bits(), num_bits()), num_bits());
      ASSERT_EQ(_layered_bitmap.find_first_set_bit(num_bits() / 2, num_bits() / 2), num_bits() / 2);

      // clear with empty range
      set_range_and_verify(0, num_bits());
      ASSERT_TRUE(_layered_bitmap.is_full());

      clear_range_and_verify(0, 0);
      ASSERT_TRUE(_layered_bitmap.is_full()); // nothing changed
      clear_range_and_verify(num_bits() / 2, num_bits() / 2);
      ASSERT_TRUE(_layered_bitmap.is_full()); // nothing changed
      clear_range_and_verify(num_bits(), num_bits());
      ASSERT_TRUE(_layered_bitmap.is_full()); // nothing changed
    }

    // Tester scans from [scanrange_beg, scanrange_end)
    // and we prepared a (1|0) bitrun between [range_beg, range_end)
    template <bool first, bool set>
    void find_X_X_bit_test_helper(IndexType scanrange_beg, IndexType scanrange_end,
                                  IndexType range_beg, IndexType range_end) {
      const IndexType intersect_beg = MAX2(scanrange_beg, range_beg);
      const IndexType intersect_end = MIN2(scanrange_end, range_end);
      const bool non_empty_intersection = (intersect_beg < intersect_end);
      if (first) { // find first
        const IndexType expected_hit =
          non_empty_intersection ? intersect_beg : scanrange_end;
        if (set) {
          EXPECT_EQ(find_first_set_bit_and_verify(scanrange_beg, scanrange_end), expected_hit);
        } else {
          EXPECT_EQ(find_first_clear_bit_and_verify(scanrange_beg, scanrange_end), expected_hit);
        }
      } else { // find last
        const IndexType expected_hit =
          non_empty_intersection ? intersect_end - 1 : scanrange_end;
        if (set) {
          EXPECT_EQ(find_last_set_bit_and_verify(scanrange_beg, scanrange_end), expected_hit);
        } else {
          EXPECT_EQ(find_last_clear_bit_and_verify(scanrange_beg, scanrange_end), expected_hit);
        }
      }
      if (::testing::Test::HasFailure()) {
        tty->print_cr("find_%s_%s_bit scanrange [%zu, %zu) range [%zu, %zu)",
          first ? "first" : "last", set ? "set" : "clear",
          scanrange_beg, scanrange_end, range_beg, range_end);
        return;
      }
    }

    // Iterate through different combinations of ranges
    template <bool first, bool set>
    void find_X_X_bit_test(IndexType begidx, IndexType endidx) {
      for (IndexType range_beg = begidx; range_beg <= endidx; range_beg++) {
        for (IndexType range_end = range_beg; range_end <= endidx; range_end++) {
          clear_and_verify();
          if (set) {
            set_range_and_verify(range_beg, range_end);
          } else {
            set_range_and_verify(0, range_beg);
            set_range_and_verify(range_end, num_bits());
          }
          find_X_X_bit_test_helper<first, set>(0, num_bits(), range_beg, range_end);
          find_X_X_bit_test_helper<first, set>(0, num_bits() / 3, range_beg, range_end);
          find_X_X_bit_test_helper<first, set>(0, (num_bits() / 3) * 2, range_beg, range_end);
          find_X_X_bit_test_helper<first, set>(num_bits() - num_bits() / 3, num_bits(), range_beg, range_end);
          if (::testing::Test::HasFailure()) {
            return;
          }
        }
      }
    }

    template <bool first, bool set>
    void find_X_X_bit_test() {
      // The tests can be quite time consuming, so we just test the first and last 2 segments
      const IndexType range = MIN2(_layered_bitmap.num_bits(), (IndexType)_layered_bitmap.num_bits_per_segment() * 2);
      find_X_X_bit_test<first, set>(0, range);
      if (range < _layered_bitmap.num_bits()) {
        find_X_X_bit_test<first, set>(num_bits() - range, num_bits());
      }
    }

    void find_first_set_bit_test()    { find_X_X_bit_test<true, true>(); }
    void find_first_clear_bit_test()  { find_X_X_bit_test<true, false>(); }
    void find_last_set_bit_test()     { find_X_X_bit_test<false, true>(); }
    void find_last_clear_bit_test()   { find_X_X_bit_test<false, false>(); }

    template <bool set>
    void X_range_test(IndexType begidx, IndexType endidx) {
      for (IndexType begidx_0 = begidx; begidx_0 <= endidx; begidx_0++) {
        for (IndexType endidx_0 = begidx_0; endidx_0 <= endidx; endidx_0++) {
          for (int pattern = 0; pattern < 3; pattern++) {
            // pattern0: into empty bitmap
            // pattern1: into full bitmap
            // pattern2: into bitmap with alternating bits
            switch (pattern) {
              case 0: clear_and_verify(); break;
              case 1: set_range_and_verify(0, num_bits()); break;
              case 2: fill_with_pattern(0xAAAAAAAAAAAAAAAAULL); break;
              default: ShouldNotReachHere();
            }
            if (set) {
              set_range_and_verify(begidx_0, endidx_0);
            } else {
              clear_range_and_verify(begidx_0, endidx_0);
            }
            count_one_bits_and_verify(0, num_bits());
            count_one_bits_and_verify(0, begidx_0);
            count_one_bits_and_verify(begidx_0, num_bits());
            if (::testing::Test::HasFailure()) {
              tty->print_cr("%d %zu %zu", pattern, begidx_0, endidx_0);
              return;
            }
          }
        }
      }
    }

    template <bool set>
    void X_range_test() {
      const IndexType range = MIN2(_layered_bitmap.num_bits(), (IndexType)_layered_bitmap.num_bits_per_segment() * 2);
      X_range_test<set>(0, range);
      if (range < _layered_bitmap.num_bits()) {
        X_range_test<set>(num_bits() - range, num_bits());
      }
    }

    void set_range_test() { X_range_test<true>(); }
    void clear_range_test() { X_range_test<false>(); }

    void count_one_bits_test() {
      clear_and_verify();
      ASSERT_EQ(count_one_bits_and_verify(0, num_bits()), (IndexType)0);
      if (_layered_bitmap.num_segments() >= 3) {
        set_range_and_verify(13, 13 + 2 * _layered_bitmap.num_bits_per_segment());
        ASSERT_EQ(count_one_bits_and_verify(0, num_bits()), (IndexType)2 * LayeredBitmap::num_bits_per_segment());
      }
    }

    void convert_to_flat_bitmap_test() const {
      CHeapBitMap tmp(num_bits(), mtTest, false);
      _layered_bitmap.convert_to_flat_bitmap(tmp);
      ASSERT_TRUE(_layered_bitmap.is_same(tmp));
      ASSERT_TRUE(_test_bitmap.is_same(tmp));
    }

    void set_from_test() {
      // test conversion from/to bitmap, and is_same
      CHeapBitMap tmp(num_bits(), mtTest, false);
      for (IndexType idx = 0; idx < num_bits(); idx++) {
        tmp.at_put(idx, _random.tosscoin());
      }
      set_from_and_verify(tmp);

      CHeapBitMap tmp2(num_bits(), mtTest, false);
      _layered_bitmap.convert_to_flat_bitmap(tmp2);
      ASSERT_TRUE(tmp.is_same(tmp2));
      ASSERT_TRUE(_layered_bitmap.is_same(tmp));

      tmp.clear();
      clear_and_verify();
      ASSERT_TRUE(_layered_bitmap.is_same(tmp));

      set_bit_and_verify(0);
      ASSERT_FALSE(_layered_bitmap.is_same(tmp));
    }

    void set_union_test() {
      const IndexType some_idx = num_bits() / 3;

      CHeapBitMap tmp(num_bits(), mtTest, true);
      tmp.set_bit(some_idx);

      set_range_and_verify(0, num_bits());
      clear_bit_and_verify(some_idx);
      ASSERT_FALSE(is_full_and_verify());

      const bool changed = set_union_with_result_and_verify(tmp);
      ASSERT_TRUE(changed);
      ASSERT_TRUE(is_full_and_verify());

      const bool changed2 = set_union_with_result_and_verify(tmp);
      ASSERT_FALSE(changed2);
      ASSERT_TRUE(is_full_and_verify());
    }

    void set_difference_test() {
      const IndexType some_idx = num_bits() / 3;

      CHeapBitMap tmp(num_bits(), mtTest, true);
      tmp.set_bit(some_idx);

      set_range_and_verify(0, num_bits());
      ASSERT_TRUE(is_full_and_verify());

      const bool changed = set_difference_with_result_and_verify(tmp);
      ASSERT_TRUE(changed);
      ASSERT_FALSE(is_full_and_verify());
      ASSERT_FALSE(at_and_verify(some_idx));

      const bool changed2 = set_difference_with_result_and_verify(tmp);
      ASSERT_FALSE(changed2);
      ASSERT_FALSE(is_full_and_verify());
    }

    void set_intersection_test() {
      const IndexType some_idx1 = num_bits() / 5;
      const IndexType some_idx2 = num_bits() / 3;

      if (some_idx1 == some_idx2) {
        return;
      }
      CHeapBitMap tmp(num_bits(), mtTest, true);
      tmp.set_range(some_idx1, num_bits());

      set_range_and_verify(0, some_idx2);
      ASSERT_FALSE(is_full_and_verify());

      const bool changed = set_intersection_with_result_and_verify(tmp);
      ASSERT_TRUE(changed);
      ASSERT_TRUE(is_only_range_set(some_idx1, some_idx2));

      const bool changed2 = set_intersection_with_result_and_verify(tmp);
      ASSERT_FALSE(changed2);
    }

    void contains_test() {
      const IndexType some_idx = num_bits() / 3;
      CHeapBitMap tmp(num_bits(), mtTest, true);
      tmp.set_range(some_idx, num_bits());
      clear_and_verify();
      ASSERT_FALSE(contains_and_verify(tmp));
      set_range_and_verify(0, num_bits() - 1);
      ASSERT_FALSE(contains_and_verify(tmp));
      set_bit_and_verify(num_bits() - 1);
      ASSERT_TRUE(contains_and_verify(tmp));
    }

    void intersects_test() {
      const IndexType some_idx1 = num_bits() / 3;
      const IndexType some_idx2 = num_bits() / 2;

      if (some_idx1 == some_idx2) {
        return;
      }
      CHeapBitMap tmp(num_bits(), mtTest, true);
      tmp.set_range(some_idx1, some_idx2);

      clear_and_verify();
      ASSERT_FALSE(intersects_and_verify(tmp));

      set_bit_and_verify(some_idx1);
      ASSERT_TRUE(intersects_and_verify(tmp));

      tmp.clear_bit(some_idx1);
      ASSERT_FALSE(intersects_and_verify(tmp));
    }

    void random_test() {
      constexpr int iterations = 10000;
      if (num_bits() < 64) {
        return;
      }
      for (int i = 0; i < iterations; i++) {
        const unsigned what = _random.next(7);
        const IndexType idx = _random.next(num_bits());
        // either quite small, or quite large
        const unsigned rangesize = _random.next((_random.next(2) > 0) ? (num_bits() / 3) : 20);
        const bool direction = _random.next(2) > 0;
        IndexType begidx = idx, endidx = idx;
        if (direction) {
          endidx = clamp(endidx + rangesize, endidx, (IndexType)num_bits());
        } else {
          begidx -= MIN2((IndexType)rangesize, begidx);
        }
        switch (what) {
          case 0: set_bit_and_verify(begidx); break;
          case 1: clear_bit_and_verify(begidx); break;
          case 2: set_range_and_verify(begidx, endidx); break;
          case 3: clear_range_and_verify(begidx, endidx); break;
          case 4: find_first_set_bit_and_verify(begidx, endidx); break;
          case 5: find_first_clear_bit_and_verify(begidx, endidx); break;
          case 6: count_one_bits_and_verify(begidx, endidx); break;
          default: ShouldNotReachHere();
        }
        if (::testing::Test::HasFailure()) {
          tty->print_cr("%u %zu %zu", what, begidx, endidx);
          return;
        }
      }
      check_equality();
      sometimes_verify();
    }
  };
}

#define DEFINE_TESTS(num_bits, testname) \
TEST_VM(Arena2_LayeredBitmap, test_ ## testname ## _ ## num_bits ## _ ## num_summary_bits) { \
  Tester* tester = new Tester(num_bits); \
  tester->testname ## _test(); \
  delete tester; \
}

#define DEFINE_ALL_TESTS(num_bits) \
  DEFINE_TESTS(num_bits, basic ) \
  DEFINE_TESTS(num_bits, set_range ) \
  DEFINE_TESTS(num_bits, clear_range ) \
  DEFINE_TESTS(num_bits, count_one_bits ) \
  DEFINE_TESTS(num_bits, find_first_set_bit ) \
  DEFINE_TESTS(num_bits, find_first_clear_bit ) \
  DEFINE_TESTS(num_bits, find_last_set_bit ) \
  DEFINE_TESTS(num_bits, find_last_clear_bit ) \
  DEFINE_TESTS(num_bits, convert_to_flat_bitmap ) \
  DEFINE_TESTS(num_bits, set_from ) \
  DEFINE_TESTS(num_bits, set_union ) \
  DEFINE_TESTS(num_bits, set_difference ) \
  DEFINE_TESTS(num_bits, set_intersection ) \
  DEFINE_TESTS(num_bits, contains ) \
  DEFINE_TESTS(num_bits, intersects ) \
  DEFINE_TESTS(num_bits, random ) \


DEFINE_ALL_TESTS(1)
DEFINE_ALL_TESTS(15)
DEFINE_ALL_TESTS(64)
DEFINE_ALL_TESTS(127)
DEFINE_ALL_TESTS(128)
DEFINE_ALL_TESTS(512)
DEFINE_ALL_TESTS(718)
DEFINE_ALL_TESTS(32768)
