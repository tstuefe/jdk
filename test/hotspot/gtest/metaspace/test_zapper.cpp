/*
 * Copyright (c) 2025, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2025, Red Hat, Inc. All rights reserved.
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

#include "memory/metaspace/metaspaceZap.hpp"
//#define LOG_PLEASE
#include "metaspaceGtestCommon.hpp"

using metaspace::Zapper;

template <size_t size>
struct RangeWithGuards {
  uint64_t prefix;
  MetaWord d[size];
  uint64_t postfix;
  static constexpr uint64_t guardpattern = 0xFAFAFAFAFAFAFAFAULL;

  RangeWithGuards() : prefix(guardpattern), postfix(guardpattern) {
    zero();
  }

  void zero() {
    memset(d, 0, sizeof(d));
  }

  void zap_and_check_guards() {
    Zapper::zap_range(d, size);
    check_guards();
  }

  void check_guards() const {
    EXPECT_EQ(prefix, guardpattern);
    EXPECT_EQ(postfix, guardpattern);
  }
};

// Pre C++17, we need to define static constexpr members
template <size_t size>
constexpr uint64_t RangeWithGuards<size>::guardpattern;

template <size_t size>
static void do_test_zap_range() {
  RangeWithGuards<size> range;
  range.zap_and_check_guards();

  for (size_t i = 0; i < size; i++) {
    ASSERT_TRUE(Zapper::is_zapped_location(range.d + i)) << i;
  }
}

template <size_t size>
static void do_test_salted_pairs() {
  RangeWithGuards<size> range;
  range.zap_and_check_guards();

  // We expect zapping to happen in word pairs of pure and salted metaspace zap pattern
  const bool expect_salt_on_even_offsets = (uint64_t)range.d != Zapper::metaspace_zap;
  for (size_t i = 0; i < size; i++) {
    const bool expect_salt = expect_salt_on_even_offsets && (i % 2 == 1);
    if (expect_salt) {
      ASSERT_EQ((uint64_t)range.d[i], Zapper::metaspace_zap);
    } else {
      ASSERT_NE((uint64_t)range.d[i], Zapper::metaspace_zap);
    }
  }
}

template <size_t size>
static void do_test_mark_as_uninitialized() {
  RangeWithGuards<size> range;

  Zapper::mark_range_uninitialized(range.d, size);
  range.check_guards();
  for (size_t i = 0; i < size; i++) {
    ASSERT_EQ((uint64_t)range.d[i], Zapper::metaspace_uninitialized);
  }
}

template <size_t size>
static void do_test_range_is_fully_zapped() {
  RangeWithGuards<size> range;
  size_t first_nonzapped = 0;

  // range_is_fully_zapped optimizes somewhat. For large ranges we only check intervals of 1KB.
  // But we should always catch overwriters at start and end of range.

  // Overwrite at start
  for (size_t i = 0; i < MIN2(4UL, size); i++) {
    range.zap_and_check_guards();
    EXPECT_TRUE(Zapper::range_is_fully_zapped(range.d, size, first_nonzapped));
    range.d[i] = 0;
    EXPECT_FALSE(Zapper::range_is_fully_zapped(range.d, size, first_nonzapped));
    EXPECT_EQ(i, first_nonzapped);
  }

  // Overwrite at end
  if (size >= 8) {
    for (size_t i = size - 1; i > size - 1 - 4; i--) {
      range.zap_and_check_guards();
      EXPECT_TRUE(Zapper::range_is_fully_zapped(range.d, size, first_nonzapped));
      range.d[i] = 0;
      EXPECT_FALSE(Zapper::range_is_fully_zapped(range.d, size, first_nonzapped));
      EXPECT_EQ(i, first_nonzapped);
    }
  }
}

template <size_t size>
static void do_test_location_looks_zapped() {
  RangeWithGuards<size> range;

  if (size > Zapper::min_significance) {
    range.zap_and_check_guards();
    EXPECT_TRUE(Zapper::location_looks_zapped(range.d, size));
    range.zero();
    EXPECT_FALSE(Zapper::location_looks_zapped(range.d, size));
  } else {
    // range too small for definitive answer
    EXPECT_FALSE(Zapper::location_looks_zapped(range.d, size));
  }
}

template <size_t size, size_t headersize>
static void do_test_range_with_header_is_fully_zapped() {
  RangeWithGuards<size> range;
  size_t first_nonzapped = 0;

  range.zap_and_check_guards();

  STATIC_ASSERT(headersize <= size);
  struct SomeHeader {
    uint64_t someheaderfields[headersize];
  };

  // Simulate presence of some header at the start of the range
  SomeHeader* const hdr = (SomeHeader*)range.d;
  memset(hdr, 0, sizeof(SomeHeader));
  EXPECT_TRUE(Zapper::range_with_header_is_fully_zapped<SomeHeader>(hdr, size, first_nonzapped));
}

#define DEFINE_ONE_TEST(testname, size) \
TEST_VM(metaspaceZapper, testname ## _ ## size) { \
	do_ ## testname < size > (); \
}

#define DEFINE_TESTS(testname) \
	DEFINE_ONE_TEST(testname, 1) \
  DEFINE_ONE_TEST(testname, 5) \
  DEFINE_ONE_TEST(testname, 267) \
  DEFINE_ONE_TEST(testname, 499) \
  DEFINE_ONE_TEST(testname, 534)

DEFINE_TESTS(test_zap_range )
DEFINE_TESTS(test_salted_pairs)
DEFINE_TESTS(test_mark_as_uninitialized)
DEFINE_TESTS(test_range_is_fully_zapped)
DEFINE_TESTS(test_location_looks_zapped)

TEST_VM(metaspaceZapper, test_range_with_header_is_fully_zapped) {
  do_test_range_with_header_is_fully_zapped<1, 1>();
  do_test_range_with_header_is_fully_zapped<10, 7>();
  do_test_range_with_header_is_fully_zapped<K, 128>();
}

