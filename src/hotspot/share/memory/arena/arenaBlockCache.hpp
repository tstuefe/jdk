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

#ifndef SHARE_MEMORY_ARENA_ARENABLOCKCACHE_HPP
#define SHARE_MEMORY_ARENA_ARENABLOCKCACHE_HPP

#include "memory/arena/arenaBlock.hpp"
#include "memory/arena/arenaSlab.hpp"
#include "utilities/align.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"



// The block cache is a vector of blocks of one block size.
// A vector for cache-friendly iteration and because the arena manager never writes to free blocks as a policy
// (avoids cache misses), so linked lists are out of question.
// The vector is small, since we don't need nor want to cache a large number of blocks per core. Cache sizes are
// custom-sized for each block size (with the lower sizes, 256, 1K, 32K having the largest caches since these see
// the largest amount of alloc churn).

// Like all other parts of the memory manager, the cache returns low-address blocks first.
// The vector is ordered in descending order, highest-addresses first, lowest last. That is because we have more
// allocation calls than deallocation. Allocation always happens as single blocks whereas we bulk-deallocate a couple
// of blocks on arena death or resource mark scope end. So we want allocation to be fast. Allocation takes the top
// (lowest) address from the vector and returns it, so in general, an allocation is really cheap, since vector does
// not need reshuffling.

class alignas(DEFAULT_PADDING_SIZE) ArenaBlockCache {
public:

  // one cache line size - one word for control members (count etc)
  static constexpr int max_cachesize = 7;

  static constexpr int max_blocks_for_bulk_deallocate = 16;

private:

  NONCOPYABLE(ArenaBlockCache);

  const uint8_t _cachesize;   // actual cache size
  uint8_t _count;             // Number of blocks in cache
  bool _is_busy;              // true if allocation requests came in since the last tick
  const_blockptr_t _blocks[max_cachesize];

  // Next cacheline, which is okay for now.
  ArenaSlab* const _slab;

#ifdef ASSERT
  void verify_block(const_blockptr_t block) const;
  void verify_blocks(const const_blockptr_t* blocks, int num_blocks) const;
  static void verify_block_order(const const_blockptr_t* blocks, int num_blocks, bool lo_to_hi);
#endif

  void set_count(int n) {
    assert(n >= 0 && n <= cachesize(), "n too large");
    _count = n;
  }

  // Given a block pointer, insert it to the cache. If cache is full, returns the highest-address
  // block, nullptr otherwise.
  // Does *not* correct the count. May be invalid after the call.
  const_blockptr_t insert(const_blockptr_t block, int num_in_cache) {
    // Arenas chunks often happen to be allocated with ascending addresses,
    // and they are always rolled back in reverse order. That meshes nicely
    // with the order (hi to lo) of block addresses in this cache. Insert sort
    // in should usually work well.
    const int c = num_in_cache;
#ifdef ASSERT
    for (int i = 0; i < c; i++) {
      assert(_blocks[i] != block, "block already in cache??");
    }
#endif
    if (c == cachesize()) {
      int pos = c - 1;
      while (pos >= 0 && _blocks[pos] < block) {
        pos--;
      }
      while (pos >= 0) {
        assert(_blocks[pos] > block, "block order?");
        const const_blockptr_t b2 = _blocks[pos];
        _blocks[pos] = block;
        block = b2;
        pos --;
      }
      return block;
    } else {
      assert(c < cachesize(), "Sanity");
      int pos = c;
      while (pos > 0 && _blocks[pos - 1] < block) {
        pos--;
      }
      while (pos < c) {
        assert(_blocks[pos] < block, "block order?");
        const const_blockptr_t b2 = _blocks[pos];
        _blocks[pos] = block;
        block = b2;
        pos ++;
      }
      _blocks[pos] = block;
      return nullptr;
    }
  }

  // Used when high-address entries got evicted.
  // Given a position and a count, shifts entries [pos, pos+count) down
  // to cache position 0.
  // Does *not* correct the count. May be invalid after the call.
  void compress_cache(int pos, int num) {
    if (pos == 0) {
      return;
    }
    assert(pos <= cachesize() && pos > 0, "Sanity");
    assert(pos + num <= cachesize(), "Sanity");
    for (int i = 0; i < num; i++) {
      _blocks[i] = _blocks[pos + i];
    }
    DEBUG_ONLY(zero_out_slots(num, cachesize() - num);)
  }

  void zero_out_slots(int pos, int count) {
    for (int i = 0; i < count; i++) {
      _blocks[pos + i] = nullptr;
    }
  }

public:


  ArenaBlockCache(ArenaSlab* slab, int cachesize);

  int cachesize() const { return _cachesize; }
  int count() const { return _count; }

  const_blockptr_t allocate_block() {
    // Remove block with lowest address. This is the top block and typically the hottest.
    _is_busy = true;
    int c = count();
    if (c == 0) {
      // Underflow: Refill cache from slab allocator. We prefetch a few blocks to reduce calls into
      // the slab allocator
      const int to_fetch = num_prefetch_on_underflow() + 1;
      const_blockptr_t tmp[max_cachesize + 1];
      const int num_fetched = _slab->allocate_blocks(tmp, to_fetch);
      if (num_fetched == 0) {
        return nullptr; // slab exhausted?
      }
      assert(num_fetched > 0 && num_fetched <= to_fetch, "sanity");
      DEBUG_ONLY(verify_blocks(tmp, num_fetched);)
      DEBUG_ONLY(verify_block_order(tmp, num_fetched, true);)
      // The slab allocator gives us the blocks in low-to-high address order. The first one goes to
      // the caller, the rest into the cache (we need to reverse order).
      const_blockptr_t result = tmp[0];
      for (int i = 1; i < num_fetched; i++) {
        const int j = num_fetched - i - 1;
        _blocks[j] = tmp[i];
      }
      set_count(num_fetched - 1);
      return result;

    } // end: underflow handling

    // standard path: take single block from the cache.
    c --;
    const const_blockptr_t result = _blocks[c];
    DEBUG_ONLY(_blocks[c] = nullptr;)
    set_count(c);
    return result;
  }

  // Bulk-add a number of blocks to this cache.
  void bulk_deallocate_block(const const_blockptr_t blocks[], int num_blocks) {
    assert(blocks != nullptr, "sanity");
    assert(num_blocks > 0, "sanity");
    assert(num_blocks <= max_blocks_for_bulk_deallocate, "sanity");
    DEBUG_ONLY(verify_blocks(blocks, num_blocks);)

    constexpr int eviction_set_max_size = max_blocks_for_bulk_deallocate + max_cachesize;
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
      assert(num_in_cache == cachesize(), "sanity");
      assert(num_evict_on_overflow() + num_in_eviction_set <= eviction_set_max_size, "sanity");
      for (int i = 0; i < num_evict_on_overflow(); i++) {
        evict_set[num_in_eviction_set] = _blocks[i];
        num_in_eviction_set++;
      }
      num_in_cache -= num_evict_on_overflow();
      // Then we evict the whole set.
      _slab->deallocate_blocks(evict_set, num_in_eviction_set);
      // post-cleanups
      compress_cache(num_evict_on_overflow(), num_in_cache);
    } else {
      assert(num_in_cache <= cachesize(), "sanity");
    }
    // repair count
    set_count(num_in_cache);
  }

  // A tick frees part of the cache or all of the cache, depending on whether the
  // cache is busy or not.
  void tick();

#ifdef ASSERT
  void verify_cache() const ;
#endif // ASSERT

  void flush();

  // Helper function for tests. Iterates through all blocks in the cache, highest-address first
  template <typename Function>
  void iterate(Function fun) const {
    for (int i = 0; i < count(); i++) {
      fun(_blocks[i]);
    }
  }

  // Tuning parameter

  // how many entries to evict on overflow
  static int num_evict_on_overflow(int cachesize)         { return cachesize / 2; }
  int num_evict_on_overflow() const                       { return num_evict_on_overflow(cachesize()); }

  // how many entries to prefetch on underflow
  static int num_prefetch_on_underflow(int cachesize)     { return MAX2(1,cachesize / 2); }
  int num_prefetch_on_underflow() const                   { return num_prefetch_on_underflow(cachesize()); }

  // how many entries to evict on a tick if cache is busy
  static int num_evict_on_tick_busy(int cachesize)        { (void)cachesize; return 1; }
  int num_evict_on_tick_busy() const                      { return num_evict_on_tick_busy(cachesize()); }

  // how many entries to evict on a tick if cache is idle
  static int num_evict_on_tick_nonbusy(int cachesize)     { return cachesize; }
  int num_evict_on_tick_nonbusy() const                   { return num_evict_on_tick_nonbusy(cachesize()); }

};

// A cache table holds one cache per block size
class ArenaCacheGroup {

  static constexpr unsigned num_caches = ArenaBlockHelpers::num_standard_size_classes;

  ArenaBlockCache _caches[num_caches];

  ArenaBlockCache* at(unsigned cache_index) {
    assert(cache_index < num_caches, "sanity");
    return _caches + cache_index;
  }

  const ArenaBlockCache* at(unsigned cache_index) const {
    assert(cache_index < num_caches, "sanity");
    return _caches + cache_index;
  }

public:

  ArenaCacheGroup(ArenaSlabGroup* slab_group);

  ArenaBlockCache* cache_for_size_class(block_size_class_t szclass) {
    return at(szclass);
  }

  // Flush all caches
  void flush();

  // Tick all caches
  void tick();

};

#endif // SHARE_MEMORY_ARENA_ARENABLOCKCACHE_HPP
