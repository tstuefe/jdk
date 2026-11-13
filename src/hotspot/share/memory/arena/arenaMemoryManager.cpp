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

#include "logging/log.hpp"
#include "logging/logStream.hpp"

#include "memory/arena/arenaBlock.hpp"
#include "memory/arena/arenaBlockCache.hpp"
#include "memory/arena/arenaHeap.hpp"
#include "memory/arena/arenaMemoryManager.hpp"
#include "memory/arena/arenaMutex.hpp"
#include "memory/arena/arenaPerCore.hpp"
#include "memory/arena/arenaSlab.hpp"
#include "memory/arena/arenaTick.hpp"


#include "memory/arena.hpp"


#include "runtime/os.hpp"

#include "runtime/os.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/macros.hpp"
#include "utilities/ostream.hpp"


typedef const Chunk* const_chunkptr_t;

namespace {
  // A simple fixed-sized array of chunk pointers
  class Basket {
    unsigned _count;
    static constexpr unsigned max = ArenaBlockCache::max_blocks_for_bulk_deallocate;
    const_chunkptr_t _chunks[ArenaBlockCache::max_blocks_for_bulk_deallocate];

  public:

    Basket() : _count(0), _chunks() {}

    // Add block. Returns true if full.
    bool add(const_chunkptr_t c) {
      assert(_count < max, "must not be full");
      _chunks[_count++] = c;
      return _count == max;
    }

    unsigned count() const { return _count; }
    const const_chunkptr_t* v() const { return _chunks; }
    void reset() { _count = 0; }
  };
}

class ArenaMemoryManagerImpl : public CHeapObj<mtChunk>, public Tickable {
  ArenaHeap _arena_heap;
  ArenaMetadataHeap _metadata_heap;
  ArenaSlabGroup _slab_group;
  ArenaPerCoreDataHolder _per_core;
  ArenaTickThread* _ticker;

  static constexpr int seconds_per_tick = 1;
  DEBUG_ONLY(static constexpr int verify_tick_interval = 1;)

  // Helper. Given a basket, return its content to the cache or the slab
  // depending on who is responsible for receiving it.
  void deallocate_chunks_in_basket(ArenaCacheGroup& cache_group, block_size_class_t szclass, const Basket* basket);

public:

  ArenaMemoryManagerImpl() :
    _arena_heap(), _metadata_heap(),
    _slab_group(&_arena_heap, &_metadata_heap),
    _per_core(&_metadata_heap, &_slab_group, ArenaMaxCacheGroups),
    _ticker(nullptr)
  {
    log_info(arena, init)("Arena System Initialized.");
  }

  void start_ticker() {
    assert(_ticker == nullptr, "we only need one");
    _ticker = new ArenaTickThread(this, seconds_per_tick);
    _ticker->start();
  }

  void deallocate_chunks(const Chunk* first_chunk);

  const_blockptr_t allocate_chunk_storage(size_t requested_size, size_t& real_size);

  void tick(unsigned tick_number) override;

  void print_on(outputStream* st) const;

  DEBUG_ONLY(void verify() const);
};

// Helper. Given a basket, return its content to the cache or the slab
// depending on who is responsible for receiving it.
void ArenaMemoryManagerImpl::deallocate_chunks_in_basket(ArenaCacheGroup& cache_group, block_size_class_t szclass, const Basket* basket) {
  assert(basket != nullptr, "invalid basket");
  assert(basket->count() > 0, "basket empty?");
  assert(ArenaBlockHelpers::is_valid_size_class(szclass), "invalid block size");

  DEBUG_ONLY(ArenaBlockHelpers::verify_size_class(szclass));
  if (szclass < ArenaBlockHelpers::huge_size_class) {
    // a basket with 1..n standard blocks. These are returned to their respective caches.
    ArenaBlockCache* const cache = cache_group.cache_for_size_class(szclass);
    cache->bulk_deallocate_block((const_blockptr_t*) basket->v(), basket->count());
  } else {
    // a basket with 1..n huge blocks. These are never cached but returned directly to the huge block slab.
    ArenaSlab* const slab_for_huge_blocks = _slab_group.slab_for_size_class(szclass);
    for (unsigned i = 0; i < basket->count(); i++) {
      const const_chunkptr_t chunk = basket->v()[i];
      const size_t outer_chunk_size = chunk->length() + Chunk::aligned_overhead_size();
      DEBUG_ONLY(ArenaBlockHelpers::verify_valid_huge_size(outer_chunk_size);)
      slab_for_huge_blocks->deallocate_huge_block((const_blockptr_t) chunk, outer_chunk_size);
    }
  }
}

// Allocate a single chunk of the given size. Chunk
// returned may be of larger size.
const_blockptr_t ArenaMemoryManagerImpl::allocate_chunk_storage(size_t requested_size, size_t& real_size) {

  const_blockptr_t result;
  size_t real;

  assert(requested_size > 0, "Sanity");

  if (requested_size <= ArenaBlockHelpers::max_standard_block_size) {
    // Translate the request size into a valid block size.
    const block_size_class_t szclass =
      ArenaBlockHelpers::unaligned_block_size_to_standard_size_class(requested_size);

    // Grab the per-core data for the core we are running on
    ArenaPerCoreDataHolder::Data* const per_core = _per_core.get_current_core_data();
    assert(per_core != nullptr, "invalid block size");

    {
      // Lock per-core data
      ArenaMutexLocker locker(per_core->_lock);

      // Grab cache and allocate a block from it
      ArenaBlockCache* const cache = per_core->_cache_group.cache_for_size_class(szclass);
      result = cache->allocate_block();
      real = ArenaBlockHelpers::size_class_to_block_size(szclass);
    }

  } else {
    // It is a huge block. We allocate it directly from the huge-block slab.
    ArenaSlab* const slab = _slab_group.slab_for_size_class(ArenaBlockHelpers::huge_size_class);
    real = align_up(requested_size, ArenaBlockHelpers::huge_block_granularity);
    result = slab->allocate_huge_block(real);
  }

  if (result != nullptr) {
    real_size = real;
  }
  return result;
}

void ArenaMemoryManagerImpl::deallocate_chunks(const Chunk* chunk) {

  // Minimize iterations over chain and minimize number of lockings:
  // We pull the core lock once for all chunks to be returned, regardless
  // of their sizes. We also minimize the number of cache deallocation
  // requests

  // Grab the per-core data for the core we are running on
  ArenaPerCoreDataHolder::Data* const per_core = _per_core.get_current_core_data();
  assert(per_core != nullptr, "invalid block size");

  // The cache group to return blocks to
  ArenaCacheGroup& cache_group = per_core->_cache_group;

  // The baskets we collect our pointers in.
  // Allocating this on stack is intended.
  Basket baskets[ArenaBlockHelpers::num_size_classes];

  {
    // Minimize iterations over chain and minimize number of lockings:
    // We pull the core lock once for all chunks to be returned, regardless
    // of their sizes. We also minimize the number of cache deallocation
    // requests

    // Lock per-core data
    ArenaMutexLocker locker(per_core->_lock);

    // First walk the chunk chain and fill the return baskets.
    {
      const Chunk* c = chunk;
      size_t last_outer_chunk_size = (size_t)-1;
      block_size_class_t szclass = (block_size_class_t)-1;
      Basket* current_basket = nullptr;
      constexpr size_t chunk_header_size = Chunk::aligned_overhead_size();
      while (c != nullptr) {
        const size_t outer_chunk_size = c->length() + chunk_header_size;
        const Chunk* const c_next = c->next();

        // We may rarely encounter chunks that are remnants from pre-initialization
        // time or had been allocated on the slab-exhausted fallback path. These
        // have to be returned to the C-Heap.
        if (c->on_c_heap()) {
          os::free((void*)c);
          c = c_next;
          continue;
        }

        // From here on, we are sure to have a chunk allocated via the memory manager.
        if (last_outer_chunk_size != outer_chunk_size) {
          if (ArenaBlockHelpers::is_valid_standard_block_size(outer_chunk_size)) {
            assert(is_aligned(c, outer_chunk_size), "invalid block alignment");
            szclass = ArenaBlockHelpers::valid_standard_block_size_to_size_class(outer_chunk_size);
          } else {
            assert(outer_chunk_size > ArenaBlockHelpers::max_standard_block_size, "invalid block size");
            assert(is_aligned(c, ArenaBlockHelpers::huge_block_granularity), "invalid block alignment");
            szclass = ArenaBlockHelpers::huge_size_class;
          }
          current_basket = baskets + szclass;
          last_outer_chunk_size = outer_chunk_size;
        }

        // Add to basket.
        const bool basket_is_full = current_basket->add(c);

        if (basket_is_full) { // prematurely full?
          // Rare. Must be a large arena.
          deallocate_chunks_in_basket(cache_group, szclass, current_basket);
          current_basket->reset();
        }
        c = c_next;
      }
    }

    // Now return all standard chunks we collected
    for (block_size_class_t szclass = 0; szclass <= ArenaBlockHelpers::highest_standard_size_class; szclass++) {
      const Basket* const basket = baskets + szclass;
      if (basket->count() > 0) {
        deallocate_chunks_in_basket(cache_group, szclass, basket);
      }
    }

    // Now return all huge blocks we collected.
    {
      const Basket* const huge_basket = baskets + ArenaBlockHelpers::highest_size_class;
      if (huge_basket->count() > 0) {
        deallocate_chunks_in_basket(cache_group, ArenaBlockHelpers::highest_size_class, huge_basket);
      }
    }
  }
}

void ArenaMemoryManagerImpl::tick(unsigned tick_number) {
  // We first tick the slabs, then the caches.
  _slab_group.tick(tick_number);
  _per_core.tick();
  LogTarget(Info, arena, stats) lt;
  if (lt.is_enabled()) {
    LogStream ls(lt);
    ls.print_cr("Arena Memory Manager, Tick %u", tick_number);
    print_on(&ls);
  }
#ifdef ASSERT
  if ((tick_number % verify_tick_interval) == 0) {
    verify();
  }
#endif
}

void ArenaMemoryManagerImpl::print_on(outputStream* st) const {
  st->print_cr("Arena Statistic:");
  _arena_heap.print_on(st);
  _metadata_heap.print_on(st);
  _slab_group.print_on(st);
  _per_core.print_on(st);
}

#ifdef ASSERT
void ArenaMemoryManager::verify() const {
  if (is_initialized()) {
    _impl->verify();
  }
}
#endif


ArenaMemoryManager::ArenaMemoryManager() :
  _impl(nullptr) {
}

void ArenaMemoryManager::initialize() {
  _impl = new ArenaMemoryManagerImpl();
}

void ArenaMemoryManager::start_ticker() {
  if (is_initialized()) {
    _impl->start_ticker();
  }
}

void ArenaMemoryManager::cleanup() {
  delete _impl;
  _impl = nullptr;
}

void ArenaMemoryManager::deallocate_chunks(const Chunk* first_chunk) const {
  assert(is_initialized(), "not initialized");
  return _impl->deallocate_chunks(first_chunk);
}

Chunk* ArenaMemoryManager::allocate_chunk_storage(size_t requested_size, size_t& real_size) const {
  assert(is_initialized(), "not initialized");
  return (Chunk*) _impl->allocate_chunk_storage(requested_size, real_size);
}

#ifdef ASSERT
void ArenaMemoryManagerImpl::verify() const {
  _arena_heap.verify();
  _metadata_heap.verify();
  _slab_group.verify();
  _per_core.verify();
}
#endif

void ArenaMemoryManager::tick(unsigned tick_number) const {
  if (is_initialized()) {
    _impl->tick(tick_number);
  }
}

void ArenaMemoryManager::print_on(outputStream* st) const {
  if (is_initialized()) {
    _impl->print_on(st);
  } else {
    st->print_cr("ArenaMemoryManager: not initialized");
  }
}

static ArenaMemoryManager _the_manager;

// Returns pointer to the global manager
ArenaMemoryManager& ArenaMemoryManager::instance() {
  return _the_manager;
}


