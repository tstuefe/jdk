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


#ifndef TEST_HOTSPOT_GTEST_MEMORY_TEST_ARENA_HELPERS_HPP
#define TEST_HOTSPOT_GTEST_MEMORY_TEST_ARENA_HELPERS_HPP

#include "concurrentTestRunner.inline.hpp"
#include "utilities/bitMap.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"
#include "testutils.hpp"
#include "jfr/recorder/checkpoint/types/traceid/jfrTraceIdBits.inline.hpp"

struct ArenaTestHelpers : public AllStatic {
  template <bool set>
  static BitMap::idx_t get_random_X_bit(PredictableRandom& rand, const BitMap& bm) {
    const BitMap::idx_t beg = rand.next(bm.size());
    BitMap::idx_t idx = set ? bm.find_first_set_bit(beg) : bm.find_first_clear_bit(beg);
    if (idx == bm.size()) {
      idx = set ? bm.find_first_set_bit(0, beg) : bm.find_first_clear_bit(0, beg);
      if (idx == beg) {
        idx = bm.size();
      }
    }
    return idx;
  }

  // Helper function, returns index of a random bit that is set in given bitmap, bm.size() if not found
  static BitMap::idx_t get_random_set_bit(PredictableRandom& rand, const BitMap& bm) {
    return get_random_X_bit<true>(rand, bm);
  }

  // Helper function, returns index of a random bit that is set in given bitmap, bm.size() if not found
  static BitMap::idx_t get_random_clear_bit(PredictableRandom& rand, const BitMap& bm) {
    return get_random_X_bit<false>(rand, bm);
  }

  // Helper function, returns [0..outlen) randomly chosen bits that are set in the given bitmap.
  static int get_random_set_bits(PredictableRandom& rand, const BitMap& bm, BitMap::idx_t out[], int outlen) {
    CHeapBitMap tmp(bm.size(), mtTest, false);
    int num = 0;
    tmp.set_from(bm);
    for (int i = 0; i < outlen; i++) {
      const BitMap::idx_t idx = get_random_set_bit(rand, bm);
      if (idx == bm.size()) {
        break;
      }
      tmp.set_bit(idx);
    }
    return num;
  }

};

#endif // TEST_HOTSPOT_GTEST_MEMORY_TEST_ARENA_HELPERS_HPP
