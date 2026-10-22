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

#ifndef SHARE_MEMORY_ARENA_ARENABLOCKCACHE
#define SHARE_MEMORY_ARENA_ARENABLOCKCACHE

#include "c1/c1_LinearScan.hpp"
#include "memory/arena/arenaBlock.hpp"
#include "memory/arena/arenaSlabAllocator.hpp"
#include "utilities/align.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"

template <size_t szclass, int cachesize>
class ArenaBlockCache {
public:

  // These are public for gtests
  // how many entries to evict on overflow
  static constexpr int num_evict_on_overflow = cachesize / 2;
  // how many entries to prefetch on underflow
  static constexpr int num_prefetch_on_underflow = cachesize / 2;
  // how many entries to evict on a tick if cache is busy
  static constexpr int num_evict_on_tick_busy = 1;
  // how many entries to evict on a tick if cache is busy
  static constexpr int num_evict_on_tick_nonbusy = cachesize;

  NONCOPYABLE(ArenaBlockCache);

 private:
  STATIC_ASSERT(cachesize >= 2);
  STATIC_ASSERT(num_evict_on_overflow <= cachesize);
  STATIC_ASSERT(num_prefetch_on_underflow <= cachesize);
  STATIC_ASSERT(num_evict_on_tick_busy <= cachesize);
  STATIC_ASSERT(num_evict_on_tick_nonbusy <= cachesize);
  STATIC_ASSERT(ArenaSizeClassHelpers::is_valid_standard_size_class(szclass));

  struct alignas(DEFAULT_CACHE_LINE_SIZE) {
    // Blocks are ordered on insert, high-to-low
    // (const pointers since we never change block content from here)
    const_blockptr_t _blocks[cachesize];
    // Number of items in cache
    uint8_t _count;
    // true if block requests came in since the last tick
    bool _is_busy;
  } _hot;

  // The slab allocator to bother on overflow and underflow
  ArenaSlabAllocator& _slab_allocator;

#ifdef ASSERT
  void verify_block(const_blockptr_t block) const {
    assert(block != nullptr, "null");
    assert((address)block >= _slab_allocator.base() &&
           (address)block < _slab_allocator.end(), "Block outside of arena heap (" PTR_FORMAT ")", p2i(block));
    // A block pointer from the arena heap is always aligned to at least the
    // smallest block size, which is 256, so the lowest byte should always be zero
    assert(is_aligned(block, ArenaSizeClassHelpers::min_block_size), "not properly aligned");
  }

  void bulk_verify_block(const_blockptr_t* blocks, int num_blocks) const {
    for (int i = 0; i < num_blocks; i++) {
      verify_block(blocks[i]);
    }
  }

  void verify_block_unique(const const_blockptr_t* blocks, int num_blocks) const {
    for (int i = 1; i < num_blocks; i++) {
      for (int j = i + 1; j < num_blocks; j++) {
        assert(blocks[i] != blocks[j], "duplicate block");
      }
    }
  }

  template <bool lo_to_hi>
  void verify_block_order(const const_blockptr_t* blocks, int num_blocks) const {
    for (int i = 1; i < num_blocks; i++) {
      verify_block(blocks[i]);
      assert( (lo_to_hi && blocks[i] >= blocks[i - 1]) ||
              (!lo_to_hi && blocks[i] <= blocks[i - 1]), "Bad Order");
    }
  }
#endif

  void set_count(int n) {
    assert(n <= cachesize, "n too large");
    _hot._count = n;
  }

  // Given a block pointer, insert it to the cache. If cache is full
  // returns the highest-address block, nullptr otherwise.
  // Does *not* correct the count. May be invalid after the call.
  const_blockptr_t insert(const_blockptr_t block, int num_in_cache) {
    // Arenas chunks often happen to be allocated with ascending addresses,
    // and they are always rolled back in reverse order. That meshes nicely
    // with the order (hi to lo) of block addresses in this cache. Insert sort
    // in should usually work well.
    const int c = num_in_cache;
#ifdef ASSERT
    for (int i = 0; i < c; i++) {
      assert(_hot._blocks[i] != block, "block already in cache??");
    }
#endif
    int insert_pos = 0;
    while (insert_pos < c && _hot._blocks[insert_pos] >= block) {
      insert_pos++;
    }
    for (int i = insert_pos; i < c; i++) {
      const const_blockptr_t b2 = _hot._blocks[i];
      _hot._blocks[i] = block;
      block = b2;
    }
    if (c < cachesize) {
      _hot._blocks[c] = block;
      return nullptr;
    }
    return block;
  }

  // Used when high-address entries got evicted.
  // Given a position and a count, shifts entries [pos, pos+count) down
  // to cache position 0.
  // Does *not* correct the count. May be invalid after the call.
  void compress_cache(int pos, int num) {
    assert(pos < cachesize && pos > 0, "Sanity");
    assert(pos + num <= cachesize, "Sanity");
    for (int i = 0; i < num; i++) {
      _hot._blocks[i] = _hot._blocks[pos + i];
    }
    DEBUG_ONLY(zero_out_slots(num, cachesize - num);)
  }

  void zero_out_slots(int pos, int count) {
    for (int i = 0; i < count; i++) {
      _hot._blocks[pos + i] = nullptr;
    }
  }

public:

  ArenaBlockCache(ArenaSlabAllocator& slab_allocator) : _slab_allocator(slab_allocator) {
    reset();
  }

  int count() const { return _hot._count; }

  void deallocate_block(const_blockptr_t block) {
    DEBUG_ONLY(verify_block(block);)
    const int c = count();
    block = insert(block, c);
    if (block != nullptr) {
      // Overflow: We need to evict. We evict a few blocks from the cache as well
      // to reduce calls into the slab allocator.
      assert(c == cachesize, "Cache should be full");
      const_blockptr_t eviction_set [num_evict_on_overflow + 1];
      eviction_set[0] = block;
      for (int i = 0; i < num_evict_on_overflow; i++) {
        eviction_set[i + 1] = _hot._blocks[i];
      }
      _slab_allocator.deallocate_blocks(szclass, eviction_set, num_evict_on_overflow + 1);
      constexpr int c2 = cachesize - num_evict_on_overflow;
      compress_cache(num_evict_on_overflow, c2);
      set_count(c2);
    } else {
      set_count(c + 1);
    }
  }

  const_blockptr_t allocate_block() {
    // Remove block with lowest address. This is the top block and typically the hottest.
    int c = count();
    if (c == 0) {
      // Underflow: Refill cache from slab allocator. We prefetch a few blocks to reduce calls into
      // the slab allocator
      constexpr int to_fetch = num_prefetch_on_underflow + 1;
      const_blockptr_t tmp[to_fetch];
      const int num_fetched = _slab_allocator.allocate_blocks(szclass, tmp, to_fetch);
      if (num_fetched == 0) {
        return nullptr; // slab exhausted?
      }
      assert(num_fetched > 0 && num_fetched <= to_fetch, "sanity");
      DEBUG_ONLY(verify_block_order<true>(tmp, num_fetched);)
      DEBUG_ONLY(verify_block_unique(tmp, num_fetched);)
      // The slab allocator gives us the blocks in low-to-high address order. The first one goes to
      // the caller, the rest into the cache (we need to reverse order).
      const_blockptr_t result = tmp[0];
      for (int i = 1; i < num_fetched; i++) {
        const int j = num_fetched - i - 1;
        _hot._blocks[j] = tmp[i];
      }
      set_count(num_fetched - 1);
      return result;

    } // end: underflow handling

    // standard path: take single block from the cache.
    c --;
    const const_blockptr_t result = _hot._blocks[c];
    DEBUG_ONLY(_hot._blocks[c] = nullptr;)
    set_count(c);
    _hot._is_busy = true;
    return result;
  }

  static constexpr int max_blocks_for_bulk_add = 16;

  // Bulk-add a number of blocks to this cache.
  void bulk_deallocate_block(const_blockptr_t blocks[], int num_blocks) {
    assert(blocks != nullptr, "sanity");
    assert(num_blocks > 0, "sanity");
    assert(num_blocks <= max_blocks_for_bulk_add, "sanity");
    DEBUG_ONLY(bulk_verify_block(blocks, num_blocks);)

    constexpr int eviction_set_max_size = max_blocks_for_bulk_add + num_evict_on_overflow;
    const_blockptr_t evict_set [eviction_set_max_size];
    int num_in_eviction_set = 0;
    int num_in_cache = count();

    // First split the set (cache blocks, caller blocks) into those we want to keep and
    // those we need to evict.
    for (int i = 0; i < num_blocks; i++) {
      const const_blockptr_t evicted_block = insert(blocks[i], num_in_cache);
      if (evicted_block != nullptr) {
        evict_set[num_in_eviction_set] = evicted_block;
        num_in_eviction_set ++;
      } else {
        num_in_cache ++;
      }
    }
    if (num_in_eviction_set > 0) {
      // Overflow. We need to evict these  blocks. To reduce calls to the slab allocator,
      // we also evict a few blocks from the cache.
      assert(num_in_cache == cachesize, "sanity");
      assert(num_evict_on_overflow + num_in_eviction_set <= eviction_set_max_size, "sanity");
      for (int i = 0; i < num_evict_on_overflow; i++) {
        evict_set[num_in_eviction_set] = _hot._blocks[i];
        num_in_eviction_set++;
      }
      num_in_cache -= num_evict_on_overflow;
      // Then we evict the whole set.
      if (num_in_eviction_set > 0) {
        _slab_allocator.deallocate_blocks(szclass, evict_set, num_in_eviction_set);
      }
      // post-cleanups
      compress_cache(num_evict_on_overflow, num_in_cache);
    } else {
      assert(num_in_cache <= cachesize, "sanity");
    }
    // repair count
    set_count(num_in_cache);
  }

  // A tick frees part of the cache or all of the cache, depending on whether the
  // cache is busy or not.
  void tick() {
    DEBUG_ONLY(verify_cache();)
    const int c = count();
    const int num_to_evict_ideal = _hot._is_busy ? num_evict_on_tick_busy : num_evict_on_tick_nonbusy;
    const int num_to_evict = MIN2(c, num_to_evict_ideal);
    if (c > 0) {
      _slab_allocator.deallocate_blocks(szclass, _hot._blocks, num_to_evict);
      const int c2 = c - num_to_evict;
      compress_cache(num_to_evict, c2);
      set_count(c2);
      _hot._is_busy = false; // reset flag
    }
  }

#ifdef ASSERT
  void verify_cache() const {
    const int c = count();
    assert(c >= 0 && c <= cachesize, "Sanity");
    verify_block_order<false>(_hot._blocks, c);
    verify_block_unique(_hot._blocks, c);
    for (int i = 1; i < cachesize; i++) {
      if (i < c) {
        verify_block(_hot._blocks[i]);
      } else {
        assert(_hot._blocks[i] == nullptr, "Sanity");
      }
    }
  }
#endif // ASSERT

  void reset() {
    zero_out_slots(0, cachesize);
    _hot._is_busy = false;
    _hot._count = 0;
  }
};

#endif // SHARE_MEMORY_ARENA_ARENABLOCKCACHE
