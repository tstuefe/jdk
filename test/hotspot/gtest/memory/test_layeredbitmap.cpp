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

class MyRandom {
  unsigned _r;
public:
  MyRandom(unsigned seed) : _r(seed) {}
  MyRandom() : _r(1500000001) {}
  unsigned next() { _r = os::next_random(_r); return _r; }
  unsigned next(unsigned boundary) { _r = next(); return _r % boundary; }
};

template <int num_summary_bits, int num_segment_bits>
struct Tester : public CHeapObj<mtTest> {
  typedef LayeredBitmap<num_summary_bits, num_segment_bits> BitmapType;
  typedef BitMap::idx_t IndexType;
  static constexpr IndexType num_bits = BitmapType::num_bits;

  typename BitmapType::Storage _layered_bitmap_storage;
  BitmapType _layered_bitmap;
  BitMap::bm_word_t _test_bitmap_storage[BitmapType::num_words];
  BitMapView _test_bitmap;
  MyRandom _random;
  mutable int _sometimes;

  Tester() : _layered_bitmap(&_layered_bitmap_storage, true),
             _test_bitmap(_test_bitmap_storage, num_bits), _sometimes(0) {
    memset(_test_bitmap_storage, 0, sizeof(_test_bitmap_storage));
  }

  void sometimes_verify() {
#ifdef ASSERT
    if ((_sometimes++ % VERIFICATION_INTERVAL) == 0) {
      _layered_bitmap.verify();
    }
#endif // ASSERT
  }

  void check_equality() const {
    ASSERT_EQ(_test_bitmap.is_empty(), _layered_bitmap.is_empty());
    ASSERT_EQ(_test_bitmap.is_full(), _layered_bitmap.is_full());
    ASSERT_EQ(_test_bitmap.count_one_bits(), _layered_bitmap.count_one_bits());
    ASSERT_EQ(memcmp(_test_bitmap_storage, _layered_bitmap_storage._bitmap_storage, num_bits / BitsPerByte), 0);
  }

  void set_bit_and_verify(IndexType bitno) {
    _test_bitmap.set_bit(bitno);
    _layered_bitmap.set_bit(bitno);
    check_equality();
    sometimes_verify();
  }

  void clear_bit_and_verify(IndexType bitno) {
    _test_bitmap.clear_bit(bitno);
    _layered_bitmap.clear_bit(bitno);
    check_equality();
    sometimes_verify();
  }

  void set_range_and_verify(IndexType beg, IndexType end) {
    _test_bitmap.set_range(beg, end);
    _layered_bitmap.set_range(beg, end);
    check_equality();
    sometimes_verify();
  }

  void clear_range_and_verify(IndexType beg, IndexType end) {
    _test_bitmap.clear_range(beg, end);
    _layered_bitmap.clear_range(beg, end);
    check_equality();
    sometimes_verify();
  }

  IndexType first_set_bit_and_verify(IndexType beg, IndexType end) {
    const IndexType idx = _layered_bitmap.find_first_set_bit(beg, end);
    EXPECT_EQ(idx, _test_bitmap.find_first_set_bit(beg, end));
    return idx;
  }

  IndexType first_clear_bit_and_verify(IndexType beg, IndexType end) {
    const IndexType idx = _layered_bitmap.find_first_clear_bit(beg, end);
    EXPECT_EQ(idx, _test_bitmap.find_first_clear_bit(beg, end));
    return idx;
  }

  void count_one_bit_and_verify(IndexType beg, IndexType end) {
    ASSERT_EQ(_test_bitmap.count_one_bits(beg, end), _layered_bitmap.count_one_bits(beg, end));
  }

  void clear_all_and_verify() {
    _test_bitmap.clear();
    _layered_bitmap.clear();
    ASSERT_EQ((IndexType)0, _layered_bitmap.count_one_bits());
    check_equality();
    sometimes_verify();
  }

  void fill_with_pattern_and_verify(BitMap::bm_word_t bit_pattern) {
    _layered_bitmap.propagate_bit_pattern(bit_pattern);
    for (IndexType idx = 0; idx < BitmapType::num_words; idx++) {
      _test_bitmap_storage[idx] = bit_pattern;
    }
    check_equality();
    sometimes_verify();
  }

  void basic_test() {

    ASSERT_EQ(BitmapType::num_bits / BitsPerWord, BitmapType::num_words);

    // set and clear first bit
    set_bit_and_verify(0);
    ASSERT_EQ((IndexType)1, _layered_bitmap.count_one_bits());
    clear_bit_and_verify(0);
    ASSERT_EQ((IndexType)0, _layered_bitmap.count_one_bits());

    // set and clear bit in second segment
    set_bit_and_verify(num_segment_bits);
    ASSERT_EQ((IndexType)1, _layered_bitmap.count_one_bits());
    clear_bit_and_verify(num_segment_bits);
    ASSERT_EQ((IndexType)0, _layered_bitmap.count_one_bits());

    // set and clear last bit
    set_bit_and_verify(num_bits - 1);
    ASSERT_EQ((IndexType)1, _layered_bitmap.count_one_bits());
    clear_bit_and_verify(num_bits - 1);
    ASSERT_EQ((IndexType)0, _layered_bitmap.count_one_bits());

    // set all
    set_range_and_verify(0, num_bits);
    ASSERT_TRUE(_layered_bitmap.is_full());
    ASSERT_FALSE(_layered_bitmap.is_empty());
    ASSERT_EQ(_layered_bitmap.count_one_bits(), (IndexType)num_bits);

    // clear all
    clear_range_and_verify(0, num_bits);
    ASSERT_FALSE(_layered_bitmap.is_full());
    ASSERT_TRUE(_layered_bitmap.is_empty());
    ASSERT_EQ(_layered_bitmap.count_one_bits(), (IndexType)0);

    // set range with empty range
    set_range_and_verify(0, 0);
    ASSERT_TRUE(_layered_bitmap.is_empty()); // nothing changed
    set_range_and_verify(num_bits / 2, num_bits / 2);
    ASSERT_TRUE(_layered_bitmap.is_empty()); // nothing changed
    set_range_and_verify(num_bits, num_bits);
    ASSERT_TRUE(_layered_bitmap.is_empty()); // nothing changed

    // find first set with empty range
    set_range_and_verify(0, num_bits);
    ASSERT_TRUE(_layered_bitmap.is_full());
    ASSERT_EQ(_layered_bitmap.find_first_set_bit(num_bits, num_bits), (IndexType)num_bits);
    ASSERT_EQ(_layered_bitmap.find_first_set_bit(55, 55), (IndexType)55);

    // clear with empty range
    set_range_and_verify(0, num_bits);
    ASSERT_TRUE(_layered_bitmap.is_full());

    clear_range_and_verify(0, 0);
    ASSERT_TRUE(_layered_bitmap.is_full()); // nothing changed
    clear_range_and_verify(num_bits / 2, num_bits / 2);
    ASSERT_TRUE(_layered_bitmap.is_full()); // nothing changed
    clear_range_and_verify(num_bits, num_bits);
    ASSERT_TRUE(_layered_bitmap.is_full()); // nothing changed
  }

  void first_set_bit_test(IndexType startidx, IndexType begidx, IndexType endidx) {
    const bool empty = begidx == endidx;
    ASSERT_EQ(first_set_bit_and_verify(startidx, endidx), begidx);
    ASSERT_EQ(first_set_bit_and_verify(startidx, num_bits), empty ? num_bits : begidx);
  }

  void first_set_bit_test(IndexType begidx, IndexType endidx) {
    clear_all_and_verify();
    for (IndexType begidx_0 = begidx; begidx_0 <= endidx; begidx_0++) {
      for (IndexType endidx_0 = begidx_0; endidx_0 <= endidx; endidx_0++) {
        set_range_and_verify(begidx_0, endidx_0);
        first_set_bit_test(0, begidx_0, endidx_0);
        first_set_bit_test(begidx_0 / 3, begidx_0, endidx_0);
        clear_range_and_verify(begidx_0, endidx_0);
        if (::testing::Test::HasFailure()) {
          tty->print_cr("%zu %zu", begidx_0, endidx_0);
          return;
        }
      }
    }
  }

  void first_set_bit_test() {
    first_set_bit_test(0, num_segment_bits * 2);
    first_set_bit_test(num_bits - num_segment_bits * 2, num_bits);
  }

  void first_clear_bit_test(IndexType startidx, IndexType begidx, IndexType endidx) {
    const bool empty = begidx == endidx;
    ASSERT_EQ(first_clear_bit_and_verify(startidx, endidx), begidx);
    ASSERT_EQ(first_clear_bit_and_verify(startidx, num_bits), empty ? num_bits : begidx);
  }

  void first_clear_bit_test(IndexType begidx, IndexType endidx) {
    set_range_and_verify(0, num_bits);
    for (IndexType begidx_0 = begidx; begidx_0 <= endidx; begidx_0++) {
      for (IndexType endidx_0 = begidx_0; endidx_0 <= endidx; endidx_0++) {
        clear_range_and_verify(begidx_0, endidx_0);
        first_clear_bit_test(0, begidx_0, endidx_0);
        first_clear_bit_test(begidx_0 / 3, begidx_0, endidx_0);
        set_range_and_verify(begidx_0, endidx_0);
        if (::testing::Test::HasFailure()) {
          tty->print_cr("%zu %zu", begidx_0, endidx_0);
          return;
        }
      }
    }
  }

  void first_clear_bit_test() {
    first_clear_bit_test(0, num_segment_bits * 2);
    first_clear_bit_test(num_bits - num_segment_bits * 2, num_bits);
  }

  void set_range_test(IndexType begidx, IndexType endidx) {
    for (IndexType begidx_0 = begidx; begidx_0 <= endidx; begidx_0++) {
      for (IndexType endidx_0 = begidx_0; endidx_0 <= endidx; endidx_0++) {
        const IndexType rangesize = endidx_0 - begidx_0;
        for (int pattern = 0; pattern < 3; pattern++) {
          // pattern0: into empty bitmap
          // pattern1: into full bitmap
          // pattern2: into bitmap with alternating bits
          switch (pattern) {
            case 0: clear_all_and_verify(); break;
            case 1: set_range_and_verify(0, num_bits); break;
            case 2: fill_with_pattern_and_verify(0xAAAAAAAAAAAAAAAAULL); break;
            default: ShouldNotReachHere();
          }
          set_range_and_verify(begidx_0, endidx_0);
          if (pattern == 0) {
            EXPECT_EQ(_layered_bitmap.count_one_bits(), rangesize);
          } else if (pattern == 1) {
            EXPECT_EQ(_layered_bitmap.count_one_bits(), num_bits);
          }
          EXPECT_EQ(_layered_bitmap.count_one_bits(begidx_0, endidx_0), rangesize);
          if (::testing::Test::HasFailure()) {
            tty->print_cr("%d %zu %zu", pattern, begidx_0, endidx_0);
            return;
          }
        }
      }
    }
  }

  void set_range_test() {
    set_range_test(0, 2 * num_segment_bits);
    set_range_test(num_bits - 2 * num_segment_bits, num_bits);
  }

  void clear_range_test(IndexType begidx, IndexType endidx) {
    for (IndexType begidx_0 = begidx; begidx_0 <= endidx; begidx_0++) {
      for (IndexType endidx_0 = begidx_0; endidx_0 <= endidx; endidx_0++) {
        const IndexType rangesize = endidx_0 - begidx_0;
        for (int pattern = 0; pattern < 3; pattern++) {
          // pattern0: into empty bitmap
          // pattern1: into full bitmap
          // pattern2: into bitmap with alternating bits
          switch (pattern) {
            case 0: clear_range_and_verify(0, num_bits); break;
            case 1: set_range_and_verify(0, num_bits); break;
            case 2: fill_with_pattern_and_verify(0xAAAAAAAAAAAAAAAAULL); break;
            default: ShouldNotReachHere();
          }
          clear_range_and_verify(begidx_0, endidx_0);
          if (pattern == 0) {
            EXPECT_EQ(_layered_bitmap.count_one_bits(), (IndexType)0);
          } else if (pattern == 1) {
            EXPECT_EQ(_layered_bitmap.count_one_bits(), num_bits - rangesize);
          }
          EXPECT_EQ(_layered_bitmap.count_one_bits(begidx_0, endidx_0), (IndexType)0);
          if (::testing::Test::HasFailure()) {
            tty->print_cr("%d %zu %zu", pattern, begidx_0, endidx_0);
            return;
          }
        }
      }
    }
  }

  void clear_range_test() {
    clear_range_test(0, 2 * num_segment_bits);
    clear_range_test(num_bits - 2 * num_segment_bits, num_bits);
  }

  void random_test() {
    constexpr int iterations = 10000;
    for (int i = 0; i < iterations; i++) {
      const unsigned what = _random.next(7);
      const IndexType idx = _random.next(num_bits);
      // either quite small, or quite large
      const unsigned rangesize = _random.next((_random.next(2) > 0) ? (num_bits / 3) : 20);
      const bool direction = _random.next(2) > 0;
      IndexType begidx = idx, endidx = idx;
      if (direction) {
        endidx = clamp(endidx + rangesize, endidx, num_bits);
      } else {
        begidx -= MIN2((IndexType)rangesize, begidx);
      }
      switch (what) {
        case 0: set_bit_and_verify(begidx); break;
        case 1: clear_bit_and_verify(begidx); break;
        case 2: set_range_and_verify(begidx, endidx); break;
        case 3: clear_range_and_verify(begidx, endidx); break;
        case 4: first_set_bit_and_verify(begidx, endidx); break;
        case 5: first_clear_bit_and_verify(begidx, endidx); break;
        case 6: count_one_bit_and_verify(begidx, endidx); break;
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

#define DEFINE_TESTS(num_bits, num_summary_bits, testname) \
TEST_VM(Arena2_LayeredBitmap, test_ ## testname ## _ ## num_bits ## _ ## num_summary_bits) { \
  Tester<num_bits, num_summary_bits>* tester = new Tester<num_bits, num_summary_bits>(); \
  tester->testname ## _test(); \
  delete tester; \
}

#define DEFINE_ALL_TESTS(num_summary_bits, num_segment_bits) \
  DEFINE_TESTS(num_summary_bits, num_segment_bits, basic ) \
  DEFINE_TESTS(num_summary_bits, num_segment_bits, set_range ) \
  DEFINE_TESTS(num_summary_bits, num_segment_bits, clear_range ) \
  DEFINE_TESTS(num_summary_bits, num_segment_bits, first_set_bit ) \
  DEFINE_TESTS(num_summary_bits, num_segment_bits, first_clear_bit ) \
  DEFINE_TESTS(num_summary_bits, num_segment_bits, random ) \


DEFINE_ALL_TESTS(64, 64)
DEFINE_ALL_TESTS(64, 128)
DEFINE_ALL_TESTS(128, 64)
DEFINE_ALL_TESTS(256, 64)

