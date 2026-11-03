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