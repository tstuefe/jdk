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

#include "precompiled.hpp"
#include "logging/log.hpp"
#include "logging/logStream.hpp"
#include "oops/klass.hpp" // for sizeof(Klass)
#include "memory/metaspace/chunkManager.hpp"
#include "memory/metaspace/classLoaderMetaspaceImpl.hpp"
#include "memory/metaspace/internalStats.hpp"
#include "memory/metaspace/metaspaceArenaGrowthPolicy.hpp"
#include "memory/metaspace/metaspaceCommon.hpp"
#include "memory/metaspace/runningCounters.hpp"
#include "utilities/ostream.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"

#define LOGFMT         "CLMSImpl @" PTR_FORMAT " "
#define LOGFMT_ARGS    p2i(this)

namespace metaspace {

ClassLoaderMetaspaceImpl::ClassLoaderMetaspaceImpl(Metaspace::MetaspaceType space_type, size_t klass_alignment)
: _binlist_nc(), _blocktree_nc(), _blocktree_c(),
  _arena_nc(
      ChunkManager::chunkmanager_nonclass(),
      ArenaGrowthPolicy::policy_for_space_type(space_type, false),
      RunningCounters::used_nonclass_counter(),
      AllocationAlignmentWordSize,
      "non-class arena"
  ),
  _arena_c(
      ChunkManager::chunkmanager_class(),
      ArenaGrowthPolicy::policy_for_space_type(space_type, true),
      RunningCounters::used_class_counter(),
      klass_alignment,
      "class arena"
  )
{}

void ClassLoaderMetaspaceImpl::print_freeblocks_state(outputStream* st) const {
  st->print("class block tree: %d, %zu words;"
            " non-class block tree: %d, %zu words;"
            " non-class bin list: %d, %zu words)",
            _blocktree_c.count(), _blocktree_c.total_word_size(),
            _blocktree_nc.count(), _blocktree_nc.total_word_size(),
            _binlist_nc.count(), _binlist_nc.total_word_size());
}

MetaBlock ClassLoaderMetaspaceImpl::allocate_from_freeblocks(size_t word_size, bool is_class) {

  // If this is a class space allocation (in which case the size should be >= sizeof Klass, so not small)
  // we look into the class-space blocktree. Otherwise either in the non-class binlist (for small allocation
  // sizes) or the non-class blocktree.
  MetaBlock result;

  static const char* const from_s[3] = { "class block tree", "non-class block tree", "non-class bin list" };
  int from = 0;
  if (is_class) {
    assert(word_size >= sizeof(Klass), "Sanity");
    result = _blocktree_c.remove_block(word_size);
  } else {
    // Non-class allocation.
    // Small? Try binlist first.
    if (word_size < _binlist_nc.MaxWordSize) {
      result = _binlist_nc.remove_block(word_size);
      from = 1;
    }
    // Try blocktree for nc allocations...
    if (result.is_empty()) {
      result = _blocktree_nc.remove_block(word_size);
      from = 2;
    }
    // Failed? give up.
  }

  if (!result.is_empty()) {
    // The result block may be larger than what the caller wanted. Return the reminder
    // back to free blocks.
    MetaBlock remainder = result.split_off_tail(word_size);
    if (remainder.word_size() > minimum_allocation_words) {
      deallocate_to_free_blocks(remainder);
    }

    // accounting, logging
    DEBUG_ONLY(InternalStats::inc_num_allocs_from_deallocated_blocks();)
    LogTarget(Trace, metaspace) lt;
    if (lt.is_enabled()) {
      LogStream ls(lt);
      ls.print("returning " METABLOCKFORMAT ", taken from %s (state now: ",
               METABLOCKFORMATARGS(result), from_s[from]);
      print_freeblocks_state(&ls);
      ls.print(")");
    }
  }

  return result;
}

void ClassLoaderMetaspaceImpl::deallocate_to_free_blocks(MetaBlock block) {
  if (block.word_size() >= minimum_allocation_words) {

    // Book into class block tree iff block can be reused for class space. That
    // is only true if the block is located in class space, is correctly aligned and
    // larger than Klass.
    const bool is_class_space = Metaspace::is_in_class_space(block.base());
    const bool aligned_for_klass = is_aligned(block.base(), _klass_alignment);
    const bool large_enough_for_klass = block.word_size() >= sizeof(Klass);

    if (is_class_space && aligned_for_klass && large_enough_for_klass) {

      _blocktree_c.add_block(block);

    } else {
      // Otherwise, book for non-class usage
      if (block.word_size() >= _binlist_nc.MaxWordSize) {
        _blocktree_nc.add_block(block);
      } else {
        _binlist_nc.add_block(block);
      }
    }
  }
}

MetaBlock ClassLoaderMetaspaceImpl::allocate(size_t word_size, bool is_class) {
  MetaBlock result;

  // try free blocks first
  result = allocate_from_freeblocks(word_size, is_class);

  // Otherwise, relegate to arenas
  if (result.is_empty()) {
    MetaspaceArena& arena = is_class ? _arena_c : _arena_nc;

    MetaBlock wastage;
    result = arena.allocate(word_size, wastage);

    // process wastage
    if (!wastage.is_empty()) {
      deallocate_to_free_blocks(wastage);
    }
  }

#ifdef ASSERT
    const size_t required_alignment = is_class ? _klass_alignment : AllocationAlignmentWordSize;
    assert(is_aligned(result.base(), required_alignment) && result.word_size() == word_size,
           "result block wrong size or alignment for " METABLOCKFORMAT, METABLOCKFORMATARGS(result));
#endif
}

void ClassLoaderMetaspaceImpl::deallocate(MetaBlock block) {
  deallocate_to_free_blocks(block);
}

} // namespace metaspace
