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
#include "unittest.hpp"

#define LOG_PLEASE
#include "testutils.hpp"

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

  BitmapType _layered_bitmap;
  CHeapBitMap _test_bitmap;
  MyRandom _random;

  static constexpr IndexType num_bits = BitmapType::num_bits;

  Tester() : _layered_bitmap(true), _test_bitmap(num_bits, mtTest, true)
  {}

  void check_equality() const {
    ASSERT_EQ(_test_bitmap.is_empty(), _layered_bitmap.is_empty());
    ASSERT_EQ(_test_bitmap.is_full(), _layered_bitmap.is_full());
    ASSERT_EQ(_test_bitmap.count_one_bits(), _layered_bitmap.count_one_bits());
    for (IndexType i = 0; i < num_bits; i++) {
      ASSERT_EQ(_test_bitmap.at(i), _layered_bitmap.at(i));
    }
  }

  void set_bit_and_verify(IndexType bitno) {
    _test_bitmap.set_bit(bitno);
    _layered_bitmap.set_bit(bitno);
    check_equality();
    DEBUG_ONLY(_layered_bitmap.verify());
  }

  void clear_bit_and_verify(IndexType bitno) {
    _test_bitmap.clear_bit(bitno);
    _layered_bitmap.clear_bit(bitno);
    check_equality();
    DEBUG_ONLY(_layered_bitmap.verify());
  }

  void set_range_and_verify(IndexType beg, IndexType end) {
    _test_bitmap.set_range(beg, end);
    _layered_bitmap.set_range(beg, end);
    check_equality();
    DEBUG_ONLY(_layered_bitmap.verify());
  }

  void clear_range_and_verify(IndexType beg, IndexType end) {
    _test_bitmap.clear_range(beg, end);
    _layered_bitmap.clear_range(beg, end);
    check_equality();
    DEBUG_ONLY(_layered_bitmap.verify());
  }

  void first_set_bit_and_verify(IndexType beg, IndexType end) {
    ASSERT_EQ(_test_bitmap.find_first_set_bit(beg, end), _layered_bitmap.find_first_set_bit(beg, end));
  }

  void first_clear_bit_and_verify(IndexType beg, IndexType end) {
    ASSERT_EQ(_test_bitmap.find_first_clear_bit(beg, end), _layered_bitmap.find_first_clear_bit(beg, end));
  }

  void count_one_bit_and_verify(IndexType beg, IndexType end) {
    ASSERT_EQ(_test_bitmap.count_one_bits(beg, end), _layered_bitmap.count_one_bits(beg, end));
  }

  void test_basic() {
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
    ASSERT_TRUE(_layered_bitmap.is_empty());
    set_range_and_verify(num_bits / 2, num_bits / 2);
    ASSERT_TRUE(_layered_bitmap.is_empty());
    set_range_and_verify(num_bits, num_bits);
    ASSERT_TRUE(_layered_bitmap.is_empty());

    // find first set with empty range
    set_range_and_verify(0, num_bits);
    ASSERT_TRUE(_layered_bitmap.is_full());
    ASSERT_EQ(_layered_bitmap.find_first_set_bit(num_bits, num_bits), (IndexType)num_bits);
    ASSERT_EQ(_layered_bitmap.find_first_set_bit(55, 55), (IndexType)55);

    // find first cleared with empty range
    clear_range_and_verify(0, num_bits);
    ASSERT_TRUE(_layered_bitmap.is_empty());
    ASSERT_EQ(_layered_bitmap.find_first_clear_bit(num_bits, num_bits), (IndexType)num_bits);
    ASSERT_EQ(_layered_bitmap.find_first_clear_bit(55, 55), (IndexType)55);
  }

  void test_random() {
    constexpr int iterations = 50000;
    for (int i = 0; i < iterations; i++) {
      const unsigned what = _random.next(7);
      IndexType i1 = _random.next(num_bits);
      IndexType i2 = _random.next(num_bits);
      if (i1 > i2) {
        const IndexType tmp = i1;
        i1 = i2;
        i2 = tmp;
      }
      LOG_HERE("%u %zu %zu", what, i1, i2);
      switch (what) {
        case 0: set_bit_and_verify(i1); break;
        case 1: clear_bit_and_verify(i1); break;
        case 2: set_range_and_verify(i1, i2); break;
        case 3: clear_range_and_verify(i1, i2); break;
        case 4: first_set_bit_and_verify(i1, i2); break;
        case 5: first_clear_bit_and_verify(i1, i2); break;
        case 6: count_one_bit_and_verify(i1, i2); break;
        default: ShouldNotReachHere();
      }
    }
    check_equality();
    DEBUG_ONLY(_layered_bitmap.verify());
  }
};

#define DEFINE_RANDOM_TEST(num_bits, num_summary_bits) \
TEST_VM(LayeredBitmap, test_random_ ## num_bits ## _ ## num_summary_bits) { \
  Tester<num_bits, num_summary_bits>* tester = new Tester<num_bits, num_summary_bits>(); \
  tester->test_random(); \
  delete tester; \
}

#define DEFINE_BASIC_TEST(num_bits, num_summary_bits) \
TEST_VM(LayeredBitmap, test_cornercase_ ## num_bits ## _ ## num_summary_bits) { \
  Tester<num_bits, num_summary_bits>* tester = new Tester<num_bits, num_summary_bits>(); \
  tester->test_basic(); \
  delete tester; \
}


DEFINE_BASIC_TEST(64, 64)
DEFINE_BASIC_TEST(64, 131072)
DEFINE_BASIC_TEST(512, 512)

DEFINE_RANDOM_TEST(64, 64)
DEFINE_RANDOM_TEST(64, 131072)
DEFINE_RANDOM_TEST(512, 512)
