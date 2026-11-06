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

#if 0
#include "memory/arena/arenaBlock.hpp"
#include "memory/arena/arenaPerCore.hpp"
#include "memory/arena/arenaBlockCache.hpp"
#include "memory/arena/arenaHeap.hpp"
#include "memory/arena/arenaSlab.hpp"
#include "memory/arena/arenaMutex.hpp"

#include "memory/arena/arenaMemoryManager.hpp"

// We need to know what a chunk is
#include <algorithm>

#include "ci/ciTypeFlow.hpp"
#include "memory/arena.hpp"


#include "runtime/os.hpp"

#include "runtime/os.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/macros.hpp"
#include "utilities/ostream.hpp"

class ArenaMemoryManagerImpl {

  ArenaHeap _arena_heap;
  ArenaMetadataHeap _metadata_heap;
  ArenaSlabGroup _slab_group;
  ArenaPerCoreDataHolder _per_core;

  template <size_t basket_max>
  void deallocate_chunks_0(Chunk* chunk);

public:

  ArenaMemoryManagerImpl() :
    _arena_heap(), _metadata_heap(),
    _slab_group(&_arena_heap, &_metadata_heap),
    _per_core(&_metadata_heap, &_slab_group)
  {
  }

  // Given a chain of chunks, deallocate all of them. After returning
  // from this functions, chunks are invalid and should not be accessed.
  // The arena_size_hint is a (upper ceiling) of the total size all chunks
  // together are expected to have.
  void deallocate_chunks(Chunk* chunk, size_t arena_size_hint);

  // Allocate a single chunk of the given size. Chunk
  // returned may be of larger size.
  Chunk* allocate_chunk(size_t requested_size);

};

template <size_t max>
class Basket {
  unsigned _count;
  const_blockptr_t _v[max];
public:

  Basket() : _count(0), _v{} {}

  // Add block. Returns true if full.
  bool add(const_blockptr_t v) {
    assert(_count < max, "must not be full");
    _v[_count++] = v;
    return _count == max;
  }

  unsigned count() const { return _count; }
  const_blockptr_t* v() const { return _v; }
  void reset() { _count = 0; }
};

template <size_t max>
class BasketGroup {
public:
  static constexpr unsigned num_baskets = ArenaBlockHelpers::num_valid_size_classes;
private:
  Basket<max> _baskets[ArenaBlockHelpers::num_valid_size_classes];
public:
  bool add(const_blockptr_t v, unsigned basket) {
    return _baskets[basket].add(v);
  }
  Basket<max>* basket(unsigned basket) {
    return _baskets[basket];
  }
};

template <size_t basket_max>
void ArenaMemoryManagerImpl::deallocate_chunks_0(Chunk* chunk) {
  // Minimize iterations over chain and minimize number of lockings:
  // We pull the core lock once for all chunks to be returned, regardless
  // of their sizes. We also minimize the number of cache deallocation
  // requests
  BasketGroup<basket_max> basket_group;
  for

}

// Given a chain of chunks, deallocate all of them. After returning
// from this functions, chunks are invalid and should not be accessed.
// The arena_size_hint is a (upper ceiling) of the total size all chunks
// together are expected to have.
void ArenaMemoryManagerImpl::deallocate_chunks(Chunk* chunk, size_t arena_size_hint) {
  constexpr size_t basket_size = ArenaBlockCache::max_cachesize + ArenaBlockCache::max_blocks_for_bulk_add;
  Basket<basket_size> basket;

  // We want to iterate over the chunks only once, and optimize return to the block caches.
  // About 80% of arenas are < 200K, and consist of only a starter block (256 or 1K) and 1..n 32K blocks.
  //

}

#endif