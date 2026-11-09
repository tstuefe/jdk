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

#ifdef ARENA_STATS
ArenaBlockCache::CacheStatistics::CacheStatistics() :
  _num_blocks_allocated(0),
  _num_blocks_deallocated(0),
  _num_bulk_dealloc_ops(0),
  _num_overflows(0),
  _num_underflows(0),
  _num_busy_ticks(0),
  _num_idle_ticks(0) {
}
#endif // ARENA_STATS

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
#ifdef ARENA_STATS
  if (_is_busy) {
    _stats._num_busy_ticks++;
  } else {
    _stats._num_idle_ticks++;
  }
#endif
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

void ArenaBlockCache::print_on(const size_t block_size, outputStream* st) const {
  st->print_cr("ArenaBlockCache %zu", block_size);
  StreamIndentor indentor(st);
#define PRINT_HERE(fmt, ...) { st->print_cr(fmt, __VA_ARGS__); }
#define PRINT_COUNTER(desc, name) { PRINT_HERE("%s: " UINT64_FORMAT, desc, name); }
  PRINT_HERE("cache size: %u", cachesize());
  PRINT_HERE("count: %u", count());
#ifdef ARENA_STATS
  PRINT_COUNTER("Blocks allocs", _stats._num_blocks_allocated);
  PRINT_COUNTER("Blocks deallocs", _stats._num_blocks_deallocated);
  PRINT_COUNTER("Bulk deallocs", _stats._num_bulk_dealloc_ops);
  st->print_cr("Blocks per bulk dealloc, avg: %.2f",
    _stats._num_bulk_dealloc_ops > 0 ?
      _stats._num_blocks_deallocated /
      (double)_stats._num_bulk_dealloc_ops : 0.0);
  PRINT_COUNTER("Underflows", _stats._num_underflows);
  PRINT_COUNTER("Overflows", _stats._num_overflows);
  PRINT_HERE("Busy ticks: %u", _stats._num_busy_ticks);
  PRINT_HERE("Idle ticks: %u", _stats._num_idle_ticks);
#endif
#undef PRINT_COUNTER
#undef PRINT_HERE
}

///// Arena cache group

ArenaCacheGroup::ArenaCacheGroup(ArenaSlabGroup* slab_group) :
  _caches {
    ArenaBlockCache(slab_group->slab_for_size_class(ArenaBlockHelpers::szclass_256), 7u),
    ArenaBlockCache(slab_group->slab_for_size_class(ArenaBlockHelpers::szclass_1K), 7u),
    ArenaBlockCache(slab_group->slab_for_size_class(ArenaBlockHelpers::szclass_32K), 7u),
    ArenaBlockCache(slab_group->slab_for_size_class(ArenaBlockHelpers::szclass_64K), 3u),
    ArenaBlockCache(slab_group->slab_for_size_class(ArenaBlockHelpers::szclass_128K), 3u),
    ArenaBlockCache(slab_group->slab_for_size_class(ArenaBlockHelpers::szclass_256K), 3u),
    ArenaBlockCache(slab_group->slab_for_size_class(ArenaBlockHelpers::szclass_512K), 1u),
    ArenaBlockCache(slab_group->slab_for_size_class(ArenaBlockHelpers::szclass_1M), 1u),
  }
{
}

// Flush all caches
void ArenaCacheGroup::flush() {
  for (unsigned i = 0; i < num_caches; i++) {
    at(i)->flush();
  }
}

// Tick all caches
void ArenaCacheGroup::tick() {
  for (unsigned i = 0; i < num_caches; i++) {
    at(i)->tick();
  }
}

// Tick all caches
void ArenaCacheGroup::print_on(outputStream* st) const {
  for (unsigned i = 0; i < num_caches; i++) {
    const size_t block_size = ArenaBlockHelpers::size_class_to_block_size(i);
    at(i)->print_on(block_size, st);
  }
}

#ifdef ASSERT
void ArenaCacheGroup::verify() const {
  for (unsigned i = 0; i < num_caches; i++) {
    at(i)->verify_cache();
  }
}
#endif