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

#include "memory/arena/arenaBlock.hpp"
#include "memory/arena/arenaBlockCache.hpp"

#include "runtime/os.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/macros.hpp"
#include "utilities/ostream.hpp"

ArenaBlockCache::ArenaBlockCache(ArenaSlab* slab, int cachesize) :
  _cachesize(cachesize), _slab(slab)
{
  assert(cachesize > 0 && cachesize <= max_cachesize, "bad cache size (%d)", cachesize);
  assert(_slab != nullptr, "Sanity");
  assert(num_evict_on_overflow() <= cachesize, "Sanity");
  assert(num_prefetch_on_underflow() <= cachesize, "Sanity");
  assert(num_evict_on_tick_busy() <= cachesize, "Sanity");
  assert(num_evict_on_tick_nonbusy() <= cachesize, "Sanity");
  zero_out_slots(0, cachesize);
  set_count(0);
  _is_busy = false;
}

// A tick frees part of the cache or all of the cache, depending on whether the
// cache is busy or not.
void ArenaBlockCache::tick() {
  DEBUG_ONLY(verify_cache();)
  const int c = count();
  const int max_to_evict = _is_busy ? num_evict_on_tick_busy() : num_evict_on_tick_nonbusy();
  const int num_to_evict = MIN2(c, max_to_evict);
  if (num_to_evict > 0) {
    _slab->deallocate_blocks(_blocks, num_to_evict);
    const int c2 = c - num_to_evict;
    if (c2 > 0) {
      compress_cache(num_to_evict, c2);
    } else {
      DEBUG_ONLY(zero_out_slots(0, cachesize());)
    }
    set_count(c2);
  }
  _is_busy = false; // reset flag
}

#ifdef ASSERT
void ArenaBlockCache::verify_cache() const {
  const int c = count();
  assert(c >= 0 && c <= cachesize(), "Sanity");
  verify_blocks(_blocks, c);
  verify_block_order(_blocks, c, false);
  // Check that (at least in debug builds) the unused slots are zero'd out
  for (int i = c; i < cachesize(); i++) {
    assert(_blocks[i] == nullptr, "Sanity");
  }
}

void ArenaBlockCache::verify_block(const_blockptr_t block) const {
  _slab->verify_block(block);
}

void ArenaBlockCache::verify_blocks(const const_blockptr_t* blocks, int num_blocks) const {
  for (int i = 0; i < num_blocks; i++) {
    verify_block(blocks[i]);
    // We also verify uniqueness
    for (int j = i + 1; j < num_blocks; j++) {
      assert(blocks[i] != blocks[j], "duplicate block");
    }
  }
}

void ArenaBlockCache::verify_block_order(const const_blockptr_t* blocks, int num_blocks, bool lo_to_hi) {
  for (int i = 1; i < num_blocks; i++) {
    assert( (lo_to_hi && blocks[i] >= blocks[i - 1]) ||
            (!lo_to_hi && blocks[i] <= blocks[i - 1]), "Bad Order");
  }
}
#endif // ASSERT

void ArenaBlockCache::flush() {
  DEBUG_ONLY(verify_cache();)
  const int c = count();
  if (c > 0) {
    _slab->deallocate_blocks(_blocks, c);
  }
  set_count(0);
  zero_out_slots(0, cachesize());
  _is_busy = false;
}

///// Arena cache group

// Given a size class, allocate and initialize the cache for this size class in the given metadata heap and return
// its pointer
static ArenaBlockCache* allocate_cache(ArenaMetadataHeap* metadata_heap,
  ArenaSlabGroup* slab_group, block_size_class_t szclass, unsigned cachesize)
{
  assert(slab_group != nullptr, "Sanity");
  assert(metadata_heap != nullptr, "Sanity");
  assert(ArenaBlockHelpers::is_valid_size_class(szclass), "Sanity");

  // caches are cache-line-aligned
  metadata_heap->align_hwm_to_padded_size();
  ArenaBlockCache* const storage = metadata_heap->allocate(sizeof(ArenaBlockCache));
  metadata_heap->align_hwm_to_padded_size();

  ArenaSlab* const slab = slab_group->slab_for_size_class(szclass);
  assert(slab != nullptr, "Sanity");

  ArenaBlockCache* const cache = new (storage) ArenaBlockCache(slab, cachesize);

  return cache;
}

ArenaCacheGroup::ArenaCacheGroup(ArenaMetadataHeap* metadataHeap, ArenaSlabGroup* slabGroup) :
  _cache_lut (
    [metadataHeap, slabGroup]() {

      // We allocate only those caches we actually need from the heap, then populate the lookup table.
      // The lookup table contains for every size class (including invalid) a pointer to the cache.
      // Some size classes are not valid: for those size classes, we simply use the cache of the next upper
      // valid size class.
      // This way, we can use a simple log2(chunk size) lookup for the cache and if the chunk size e.g. is 4KB,
      // we automatically use the 32K cache and return a 32K chunks. This avoids having to do range checks on every
      // allocation.
      metadataHeap->align_hwm_to_padded_size();
      ArenaBlockCache** const lookup_table = (ArenaBlockCache**)metadataHeap->allocate(sizeof(_cache_lut));

      // cache sizes: [256, 64K) = 7, [64K-256K) = 3, [256K - 1M) = 1

      ArenaBlockCache* const cache_256 = allocate_cache(metadataHeap, slabGroup, ArenaBlockHelpers::szclass_256, 7 );
      ArenaBlockCache* const cache_1K = allocate_cache(metadataHeap, slabGroup, ArenaBlockHelpers::szclass_1K, 7 );
      ArenaBlockCache* const cache_32K = allocate_cache(metadataHeap, slabGroup, ArenaBlockHelpers::szclass_32K, 7 );
      ArenaBlockCache* const cache_64K = allocate_cache(metadataHeap, slabGroup, ArenaBlockHelpers::szclass_64K, 3 );
      ArenaBlockCache* const cache_128K = allocate_cache(metadataHeap, slabGroup, ArenaBlockHelpers::szclass_128K, 3 );
      ArenaBlockCache* const cache_256K = allocate_cache(metadataHeap, slabGroup, ArenaBlockHelpers::szclass_256K, 1 );
      ArenaBlockCache* const cache_512K = allocate_cache(metadataHeap, slabGroup, ArenaBlockHelpers::szclass_512K, 1 );
      ArenaBlockCache* const cache_1M = allocate_cache(metadataHeap, slabGroup, ArenaBlockHelpers::szclass_1M, 1 );

      // We route allocation requests < 256 to the 256 cache, < 1K to the 1K, < 32K to the 32K
      int slot = 0;
      while (slot <= ArenaBlockHelpers::szclass_256) {
        lookup_table[slot++] = cache_256K;
      }
      while (slot <= ArenaBlockHelpers::szclass_1K) {
        lookup_table[slot++] = cache_1K;
      }
      while (slot <= ArenaBlockHelpers::szclass_32K) {
        lookup_table[slot++] = cache_32K;
      }
      // beyond that, every size class has its own cache
      lookup_table[slot++] = cache_64K;
      lookup_table[slot++] = cache_128K;
      lookup_table[slot++] = cache_256K;
      lookup_table[slot++] = cache_512K;
      lookup_table[slot++] = cache_1M;

      assert(slot == num_slots, "Sanity");

      return lookup_table;

    } () ),
  _lock()
{
}
