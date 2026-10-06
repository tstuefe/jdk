/*
 * Copyright (c) 2026 IBM. All rights reserved.
 * Copyright (c) 2026, Oracle and/or its affiliates. All rights reserved.
 *
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
 */

#include "memory/arena.hpp"
#include "memory/arena_chunk_vector.hpp"
#include "runtime/os.hpp"

#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"
#include "unittest.hpp"

//#define LOG_PLEASE
#include "testutils.hpp"

class FakeChunks {
  static constexpr size_t CHUNK_OUTER_SIZE = 0x80;
  static constexpr int NUM = K * 64;
  address _chunks;
public:
  FakeChunks() {
    _chunks = (address) os::malloc(CHUNK_OUTER_SIZE * NUM, mtTest);
    assert(_chunks != nullptr, "sanity");
    for (int i = 0; i < NUM; i++) {
      ::new(at(i)) Chunk(CHUNK_OUTER_SIZE - Chunk::aligned_overhead_size());
    }
  }
  ~FakeChunks() {
    os::free(_chunks);
  }
  static constexpr int max() { return NUM; }
  Chunk* at(int index) const { return (Chunk*)(_chunks + (index * CHUNK_OUTER_SIZE)); }
};

struct Tester {
  FakeChunks _fake_chunks;
  typedef ArenaChunkVector<int> TestVectorType;

  TestVectorType _v;
  // Shadows vector content. Index into _shadow is item order in vector.
  // E.g. push push push pop push results in { 1, 2, 3, 5 }
  int* _shadow; // shadows extra_info per vector entry.
  int _generation;  // monotonously rising; index into fake_chunks
  int _hi;   // index into shadow; one past highest valid entry

  explicit Tester(int initial_capacity) :
    _v(initial_capacity),
    _shadow(nullptr),
    _generation(0), _hi(0)
  {
    _shadow = NEW_C_HEAP_ARRAY(int, FakeChunks::max(), mtTest);
    memset(_shadow, 0, sizeof(int) * FakeChunks::max());
  }

  ~Tester() {
    FREE_C_HEAP_ARRAY(_shadow);
  }

  void push_and_test() {
    LOG_HERE("push_and_test _hi %d", _hi);
    assert(_generation < _fake_chunks.max(), "out of fake chunks");
    Chunk* c = _fake_chunks.at(_generation);
    _v.push_hot(c, _generation);
    _shadow[_hi] = _generation;
    _hi ++;
    _generation ++;
    assert_expectations();
  }

  void pop_and_test() {
    LOG_HERE("pop_and_test _hi %d", _hi);
    const int num1 = _v.num();
    Chunk* c = _v.pop_hot();
    if (_hi == 0) {
      ASSERT_EQ(c, nullptr);
      ASSERT_EQ(0, _v.num());
    } else {
      const int expected_gen = _shadow[_hi - 1];
      ASSERT_EQ(c, _fake_chunks.at(expected_gen));
      ASSERT_EQ(num1 - 1, _v.num());
      _hi--;
    }
    assert_expectations();
  }

  void push_and_test(int num) {
    for (int i = 0; i < num; i++) {
      push_and_test();
    }
    assert_expectations(true);
  }

  void pop_and_test(int num) {
    for (int i = 0; i < num; i++) {
      pop_and_test();
    }
    assert_expectations(true);
  }

  void clear_older_than_and_test(int generation) {
    LOG_HERE("clear_older_than_and_test(%d), _hi %d", generation, _hi);
    auto remover = [generation](const TestVectorType::Item* item) { return item->extra_info < generation; };
    _v.bulk_remove_coldest_first(remover);
    int new_lo = 0;
    while (new_lo < _hi && _shadow[new_lo] < generation) {
      new_lo++;
    }
    if (new_lo > 0) {
      memmove(_shadow, _shadow + new_lo, sizeof(int) * (_hi - new_lo));
      _hi -= new_lo;
    }
    assert_expectations(true);
  }

  void clear_oldest_n(int num) {
    LOG_HERE("clear_oldest_n %d", num);
    if (_hi == 0) {
      clear_older_than_and_test(INT_MAX); // should be a noop; any number should do
    } else {
      const int num1 = _v.num();
      const int new_lo = MIN2(_hi, num);
      const int generation = (new_lo == _hi) ? _generation : _shadow[new_lo];
      clear_older_than_and_test(generation);
      ASSERT_EQ(_v.num(), num1 - new_lo);
    }
  }

  void assert_expectations(bool paranoid = false) const {
    ASSERT_EQ(_v.num(), _hi);
    int num = 0;
    auto tester = [&num, this](const TestVectorType::Item* item) {
      ASSERT_LT(num, _hi);
      ASSERT_EQ(_shadow[num], item->extra_info);
      ASSERT_EQ(item->chunk, _fake_chunks.at(item->extra_info));
      num++;
    };
    NOT_DEBUG(_v.iterate_coldest_first(tester);)
    DEBUG_ONLY(_v.verify_coldest_first(tester, paranoid);)
    ASSERT_EQ(_v.num(), num);
    ASSERT_GE(_v.capacity(), _v.num());
  }

  void iterate_and_test() const {
    LOG_HERE_0("iterate_and_test");
    ASSERT_EQ(_v.num(), _hi);
    int num = 0;
    auto tester = [&num, this](const TestVectorType::Item* item) {
      ASSERT_LT(num, _hi);
      ASSERT_EQ(_shadow[num], item->extra_info);
      num++;
    };
    _v.iterate_coldest_first(tester);
    ASSERT_EQ(_v.num(), num);
  }

  // Test a bunch of simple operations combined
  void combined_test_simple() {
    push_and_test(10);
    for (int i = 0; i < 12; i++) {
      pop_and_test();
    }
    push_and_test(100);
    pop_and_test();
    clear_older_than_and_test(50);
    push_and_test(101);
    iterate_and_test();
    while (_v.num() > 0) {
      pop_and_test(17);
    }
  }

  // Test a bunch of simple operations combined
  void combined_test_random() {
    unsigned seed = 0x12345678;
    while (_generation < MIN2(1024,FakeChunks::max())) {
      seed = os::next_random(seed);
      const int op = seed % 100;
      if (op < 80) {
        push_and_test();
      } else if (op < 95) {
        pop_and_test();
      } else {
        clear_oldest_n(5);
      }
    }
  }
};

#define DEFINE_TEST(initial_cap) \
  TEST_VM(ArenaChunkVector, simple_ ## initial_cap) { \
    Tester tester(initial_cap); \
    tester.combined_test_simple(); \
  } \
  TEST_VM(ArenaChunkVector, random_ ## initial_cap) { \
    Tester tester(initial_cap); \
    tester.combined_test_random(); \
  }

DEFINE_TEST(1)
DEFINE_TEST(32)
DEFINE_TEST(192)
DEFINE_TEST(1024)

TEST_VM(ArenaChunkVector, test_pop) {
  // Test pop across 0 border
  Tester tester(128);
  tester.push_and_test(128);          // at cap; next push would grow
  ASSERT_EQ(tester._v.capacity(), 128);    // should not have grown
  tester.clear_oldest_n(64);          // Make space at the front...
  tester.push_and_test(64);           // and push anew. We should not have grown but wrapped.
  ASSERT_EQ(tester._v.capacity(), 128);    // should not have grown
  // Now pop repeatedly until vector is empty, this
  // should exercise popping with negative wrap-around
  tester.pop_and_test(63);
  tester.pop_and_test(2);
  tester.pop_and_test(63);
  ASSERT_EQ(tester._v.capacity(), 128);    // still at initial cap
  ASSERT_EQ(tester._v.num(), 0);
}
