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
#include "memory/metaspace/binList.hpp"
#include "memory/metaspace/blockTree.hpp"
#include "memory/metaspace/metablock.hpp"
#include "memory/metaspace/metaspaceArena.hpp"

class outputStream;

namespace metaspace {

class ClmsStats;

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

  MetaBlock allocate_from_free_blocks(size_t word_size, bool is_class);

  void deallocate_to_free_blocks(MetaBlock block);

  // Print state of free blocks
  void print_free_blocks_state(outputStream* st) const;

public:

  ClassLoaderMetaspaceImpl(Metaspace::MetaspaceType space_type, size_t klass_alignment);
  ~ClassLoaderMetaspaceImpl();

  MetaBlock allocate(size_t word_size, bool is_class);

  void deallocate(MetaBlock block);

  void add_to_statistics(ClmsStats* out) const;

  void usage_numbers(bool for_class,
                     size_t* p_used_words,
                     size_t* p_committed_words,
                     size_t* p_capacity_words) const;

  DEBUG_ONLY(void verify() const;)
};

} // namespace metaspace

#endif // SHARE_MEMORY_METASPACE_METASPACEARENA_HPP

