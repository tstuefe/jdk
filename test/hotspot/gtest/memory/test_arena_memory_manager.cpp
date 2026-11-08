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
#include "memory/arena.hpp"

#include "memory/arena/arenaBlock.hpp"
#include "memory/arena/arenaMemoryManager.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/macros.hpp"
#include "utilities/ostream.hpp"

#include "unittest.hpp"

#include "test_arena_helpers.hpp"

//#define LOG_PLEASE
#include "testutils.hpp"
#include "memory/arena/arenaSlab.hpp"

static size_t chunk_size_to_block_size(size_t chunk_len) { return chunk_len + Chunk::aligned_overhead_size(); }
static size_t block_size_to_chunk_size(size_t block_size) { return block_size - Chunk::aligned_overhead_size(); }

namespace {
  class AllocatedChunks {
    static constexpr unsigned max_num_allocations = 1024;

    Chunk* _chunks[max_num_allocations];
    ArenaMetadataHeap _metadata_heap; // for the bitmap
    LayeredBitmap _chunk_map;

  public:
    explicit AllocatedChunks() :
      _chunks { nullptr },
      _metadata_heap(2 * M),
      _chunk_map(&_metadata_heap, max_num_allocations)
    {
      _chunk_map.clear();
    }

    // Given a number n, return a Chunk chain composed of random blocks from our
    // allocation list, and the real number returned (may be smaller).
    Chunk* get_random_chain(PredictableRandom& random, unsigned len, unsigned& real_len) {
      BitMap::idx_t idx = 0;
      unsigned have = 0;
      Chunk* first = nullptr;
      while (have < len && idx < max_num_allocations) {
        idx = ArenaTestHelpers::get_random_set_bit(random, _chunk_map);
        if (idx < max_num_allocations) {
          have++;
          Chunk* c = _chunks[idx];
          _chunks[idx] = nullptr;
          assert(c != nullptr, "Sanity");
          c->set_next(first);
          first = c;
          _chunk_map.clear_bit(idx);
        }
      }
      real_len = have;
      return first;
    }

    bool is_full() const { return _chunk_map.is_full(); }

    void add_chunk(Chunk* c) {
      assert(c != nullptr, "Sanity");
      const BitMap::idx_t idx = _chunk_map.find_first_clear_bit(0);
      const bool is_full = idx == max_num_allocations;
      assert(!is_full, "full?");
      assert(_chunks[idx] == nullptr, "out of sync");
      _chunks[idx] = c;
      _chunk_map.set_bit(idx);
    }
  };
}

namespace {
  class Tester {

    ArenaMemoryManager _memory_manager;
    AllocatedChunks _allocated_chunks;
    PredictableRandom _random;

    // How many blocks were allocated of a specific size
    unsigned _num_allocated_per_size[ArenaBlockHelpers::num_size_classes];

    const Chunk* allocate_chunk_and_test(size_t size) {
      size_t real_size = 0;
      Chunk* c = nullptr;
      Chunk* const p = _memory_manager.allocate_chunk_storage(size, real_size);
      if (size <= 32 * K) {
        // smaller blocks should not run out, we don't allocate enough for that to happen
        EXPECT_NE(p, nullptr);
      }
      if (p != nullptr) {
        EXPECT_GE(real_size, size);
        const size_t payload_len = block_size_to_chunk_size(real_size);

        // is payload writable?
        *((char*)p + Chunk::aligned_overhead_size()) = 'A';
        *((char*)p + real_size - 1) = 'A';

        // Create chunk and add it to our allocated chunks list
        c = ::new (p) Chunk(payload_len, false);
        c->set_next(nullptr);
        _allocated_chunks.add_chunk(c);

        // bookkeeping
        const block_size_class_t szclass =
            (real_size <= ArenaBlockHelpers::max_standard_block_size) ?
              ArenaBlockHelpers::valid_standard_block_size_to_size_class(real_size) :
              ArenaBlockHelpers::huge_size_class;
        _num_allocated_per_size[szclass]++;
      }
      DEBUG_ONLY(verify());
      return c;
    }

    unsigned deallocate_chunks_and_test(unsigned max) {
      unsigned num = 1 + _random.next(max);
      const Chunk* const c = _allocated_chunks.get_random_chain(_random, num, num);
      if (num > 0) {
        deallocate_chunk_chain_and_test(c);
      }
      return num;
    }

    void deallocate_chunk_chain_and_test(const Chunk* first) {
      _memory_manager.deallocate_chunks(first);
      DEBUG_ONLY(verify());
    }

#ifdef ASSERT
    void verify() const {
      _memory_manager.verify();
    }
#endif

  public:

    Tester() :
      _memory_manager(),
      _allocated_chunks(),
      _random(),
      _num_allocated_per_size { 0 }
    {
      _memory_manager.initialize();
    }

    ~Tester() {
      _memory_manager.cleanup();
    }

    void test_standard_alloc() {
      for (size_t size = 1; size <= ArenaBlockHelpers::max_standard_block_size; size *= 2) {
        const Chunk* const c = allocate_chunk_and_test(size);
        ASSERT_NE(c, nullptr);
        const size_t block_size = chunk_size_to_block_size(c->length());
        ASSERT_TRUE(ArenaBlockHelpers::is_valid_standard_block_size(block_size));
        if (block_size <= 256) {
          ASSERT_EQ(block_size, 256u);
        } else if (block_size <= K) {
          ASSERT_EQ(block_size, K);
        } else if (block_size <= 32 * K) {
          ASSERT_EQ(block_size, 32 * K);
        } else {
          ASSERT_EQ(block_size, size);
        }
      }
    }

    void test_huge_alloc_dealloc() {
      for (size_t size = M + 512 * K; size <= 10 * M; size += 512 * K) {
        const Chunk* const c = allocate_chunk_and_test(size);
        ASSERT_NE(c, nullptr);
        const size_t block_size = chunk_size_to_block_size(c->length());
        ASSERT_GE(block_size, size);
        ASSERT_TRUE(ArenaBlockHelpers::is_valid_huge_size(block_size));
      }
      while (deallocate_chunks_and_test(4));
    }

    template <size_t size>
    void test_chunk_chain_deallocation_samesize_0() {
      for (int i = 0; i < 100; i++) {
        const Chunk* const c = allocate_chunk_and_test(size);
        ASSERT_NE(c, nullptr);
      }
      while (deallocate_chunks_and_test(16));
    }

    void test_chunk_chain_deallocation_samesize() {
      test_chunk_chain_deallocation_samesize_0<256>();
      test_chunk_chain_deallocation_samesize_0<32 * K>();
    }

    void test_chunk_chain_deallocation_mixedsize() {
      for (int i = 0; i < 500; i++) {
        constexpr size_t max_block_size = 128 * K; // )
        constexpr size_t min_block_size = 128;     // [
        const size_t log2_block_size = exact_log2(min_block_size) +
                                       _random.next(exact_log2(max_block_size) - exact_log2(min_block_size));
        const Chunk* const c = allocate_chunk_and_test(nth_bit(log2_block_size));
        ASSERT_NE(c, nullptr);
      }
      while (deallocate_chunks_and_test(16));
    }

    void test_chunk_chain_deallocation_some_in_c_heap() {
      for (int i = 0; i < 500; i++) {
        constexpr size_t max_block_size = 128 * K; // )
        constexpr size_t min_block_size = 128;     // [
        const size_t log2_block_size = exact_log2(min_block_size) +
                                       _random.next(exact_log2(max_block_size) - exact_log2(min_block_size));
        const Chunk* const c = allocate_chunk_and_test(nth_bit(log2_block_size));
        ASSERT_NE(c, nullptr);
      }

      // Assemble a chunk list that has mixed C-heap chunks in them.
      bool stop = false;
      while (!stop) {
        unsigned len = 16;
        Chunk* const first = _allocated_chunks.get_random_chain(_random, len, len);
        if (first == nullptr) {
          stop = true;
        } else {
          // Mix in some C-heap chunks
          Chunk* insertion_point = first;
          while (insertion_point != nullptr) {
            if (_random.tosscoin()) {
              const size_t payload_size = _random.next(16) * BytesPerWord;
              const size_t real_size = Chunk::aligned_overhead_size() + payload_size;
              void* const p = os::malloc(real_size, mtTest);
              Chunk* const c = ::new(p) Chunk(payload_size, true);
              c->set_next(insertion_point->next());
              insertion_point->set_next(c);
            }
            insertion_point = insertion_point->next();
          }
        }
        deallocate_chunk_chain_and_test(first);
      }
    }

    void test_random() {
      for (int cycle = 0; cycle < 10000; cycle++) {
        const int op_select = _random.next(16);
        const size_t size = _random.next(16) > 14 ?
          M + _random.next(M * 10) : 1 + _random.next(64 * K);
        if (op_select < 12 && !_allocated_chunks.is_full()) {
          allocate_chunk_and_test(size);
        } else {
          deallocate_chunks_and_test(4);
        }
        if (::testing::Test::HasFailure()) {
          tty->print_cr("cycle %d op %d", cycle, op_select);
          return;
        }
      }
    }
  };

}


#define DEFINE_TESTS(seed, testname) \
TEST_VM(Arena2_ArenaMemoryManager, testname ) { \
  Tester tester; \
  tester. testname (); \
}

#define DEFINE_ALL_TESTS(seed) \
  DEFINE_TESTS(seed, test_standard_alloc ) \
  DEFINE_TESTS(seed, test_huge_alloc_dealloc ) \
  DEFINE_TESTS(seed, test_chunk_chain_deallocation_samesize ) \
  DEFINE_TESTS(seed, test_chunk_chain_deallocation_mixedsize ) \
  DEFINE_TESTS(seed, test_chunk_chain_deallocation_some_in_c_heap) \
  DEFINE_TESTS(seed, test_random )


DEFINE_ALL_TESTS(0xdeadbeef)