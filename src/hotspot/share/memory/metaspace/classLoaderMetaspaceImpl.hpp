/*
 * Copyright (c) 2023 Red Hat, Inc. All rights reserved.
 * Copyright (c) 2023, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_MEMORY_METASPACE_CLASSLOADERMETASPACEIMPL_HPP
#define SHARE_MEMORY_METASPACE_CLASSLOADERMETASPACEIMPL_HPP

#include "memory/allocation.hpp"
#include "memory/metaspace.hpp"
#include "memory/metaspace/counters.hpp"
#include "memory/metaspace/metachunkList.hpp"

#include "memory/metaspace/binList.hpp"
#include "memory/metaspace/blockTree.hpp"
#include "memory/metaspace/metaspaceArena.hpp"
#include "memory/metaspace/metablock.hpp"

class outputStream;
class Mutex;

namespace metaspace {

class ClassLoaderMetaspaceImpl: public CHeapObj<mtMetaspace> {

  //////// Free block management:

  // small blocks, aligned to minimal metaspace alignment. May or may not
  // live in class space.
  BinList32 _binlist_nc;

  // large blocks, aligned to minimal metaspace alignment. May or may not
  // live in class space.
  BlockTree _blocktree_nc;

  // large blocks > sizeof(Klass) and suitably aligned for Klass
  BlockTree _blocktree_c;

  /////// Arenas

  // Arena for non-class blocks (allocations will be aligned to minimal metaspace
  // alignment and live in non-class metaspace)
  MetaspaceArena _arena_nc;

  // Arena for class blocks (allocations will be aligned to Klass alignment and
  // live in class space)
  MetaspaceArena _arena_c;

  // Same as global Klass alignment, but separated for easier unit testing.
  const size_t _klass_alignment;

  MetaBlock allocate_from_freeblocks(size_t word_size, bool is_class);

  void deallocate_to_free_blocks(MetaBlock block);

  // Print state of free blocks
  void print_freeblocks_state(outputStream* st) const;

public:

  ClassLoaderMetaspaceImpl(Metaspace::MetaspaceType space_type, size_t klass_alignment);
  ~ClassLoaderMetaspaceImpl();

  MetaBlock allocate(size_t word_size, bool is_class);

  void deallocate(MetaBlock block);

};


/*
 * // Allocate memory from Metaspace.
// 1) Attempt to allocate from the free block list.
// 2) Attempt to allocate from the current chunk.
// 3) Attempt to enlarge the current chunk in place if it is too small.
// 4) Attempt to get a new chunk and allocate from that chunk.
// At any point, if we hit a commit limit, we return null.
MetaWord* MetaspaceArena::allocate(size_t requested_word_size) {
  UL2(trace, "requested " SIZE_FORMAT " words.", requested_word_size);

  MetaWord* p = nullptr;
  const size_t aligned_word_size = get_raw_word_size_for_requested_word_size(requested_word_size);

  // Before bothering the arena proper, attempt to re-use a block from the free blocks list
  if (_fbl != nullptr && !_fbl->is_empty()) {
    p = _fbl->remove_block(aligned_word_size);
    if (p != nullptr) {
      DEBUG_ONLY(InternalStats::inc_num_allocs_from_deallocated_blocks();)
      UL2(trace, "returning " PTR_FORMAT " - taken from fbl (now: %d, " SIZE_FORMAT ").",
          p2i(p), _fbl->count(), _fbl->total_size());
      assert_is_aligned_metaspace_pointer(p);
      // Note: free blocks in freeblock dictionary still count as "used" as far as statistics go;
      // therefore we have no need to adjust any usage counters (see epilogue of allocate_inner())
      // and can just return here.
      return p;
    }
  }

  // Primary allocation
  p = allocate_inner(aligned_word_size);

#ifdef ASSERT
  // Fence allocation
  if (p != nullptr && Settings::use_allocation_guard()) {
    STATIC_ASSERT(is_aligned(sizeof(Fence), BytesPerWord));
    MetaWord* guard = allocate_inner(sizeof(Fence) / BytesPerWord);
    if (guard != nullptr) {
      // Ignore allocation errors for the fence to keep coding simple. If this
      // happens (e.g. because right at this time we hit the Metaspace GC threshold)
      // we miss adding this one fence. Not a big deal. Note that his would
      // be pretty rare. Chances are much higher the primary allocation above
      // would have already failed).
      Fence* f = new(guard) Fence(_first_fence);
      _first_fence = f;
    }
  }
#endif // ASSERT

  return p;
}
 *
 */

} // namespace metaspace

#endif // SHARE_MEMORY_METASPACE_METASPACEARENA_HPP

