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


#ifndef SHARE_MEMORY_ARENA_ARENAHEAP_HPP
#define SHARE_MEMORY_ARENA_ARENAHEAP_HPP


#include "memory/arena/arenaBlock.hpp"
#include "memory/arena/arenaStatistics.hpp"
#include "runtime/atomic.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/ostream.hpp"

// Base class offers ways to commit, uncommit and thp-advise
// ranges of memory. It has no concept of (commit, used) boundaries.
// That is because the Slabs impose their own memory layout.
class ArenaHeapBase {
  // Reserved space boundaries
  const address _base;
  const address _end;

  // Some statistics
#ifdef ARENA_STATS
  Atomic<unsigned> _num_commit_ops;
  Atomic<unsigned> _num_discard_ops;
#endif

protected:

  DEBUG_ONLY(void verify() const);
  void print_on(const char* name, outputStream* st) const;

  ArenaHeapBase(size_t max_size);

public:

  NONCOPYABLE(ArenaHeapBase);

  // For now. Todo, needs refinement.
  static constexpr size_t large_page_size = 2 * M;

  ~ArenaHeapBase();

  bool reserved_contains(const_blockptr_t block) const {
    return block >= base() && block < end();
  }

  // Commit memory. Note that in the arena system, this is a rare operation. We don't reclaim
  // via uncommitting, so once committed memory stays committed.
  void commit_memory(const_blockptr_t from, const_blockptr_t to);

  // Given a memory range, enforce or inhibit huge pages (Linux only atm).
  void advise_large_page_use(const_blockptr_t from, const_blockptr_t to, bool use_large_pages);

  // Release memory. The memory stays reserved and can be accessed without re-committing.
  void discard_memory(const_blockptr_t from, const_blockptr_t to);

  address base() const { return _base; }
  address end() const { return _end; }
  size_t reserved_size() const { return _end - _base; }

};

class ArenaHeap : public ArenaHeapBase {

  static constexpr size_t max_size_default = 4 * G;

  // We hand out uncommitted memory. The slabs themselves take care of committing.
  const_blockptr_t _hwm;

public:

  ArenaHeap(size_t max_size = max_size_default);

  // Reserves memory for a slab. Slab start will be aligned to granule size.
  // Slab will *not* be committed.
  const_blockptr_t reserve_slab_memory(size_t block_size, size_t granule_size, unsigned num_blocks);

  const_blockptr_t hwm() const { return _hwm; }

  void print_on(outputStream* st) const;

  DEBUG_ONLY(void verify() const);

};

class ArenaMetadataHeap : public ArenaHeapBase {

  static constexpr size_t max_size_default = 64 * M; // likely a lot less

  // We pre-commit the metadata heap. We then allocate all metadata (which happens
  // at VM startup), after which - see finalize() - we shrink the commit area to
  // what we actually allocated, and possibly adivise THP use.
  // That way we reduce the number of mmap calls to just three (initial reservation
  // and two for precommit/finalize)
  address _hwm;

public:

  ArenaMetadataHeap(size_t max_size = max_size_default);

  // Reserves memory at the hwm.
  address allocate(size_t size);

  // Done after all allocations from the metadata heap are finished.
  void finalize();

  // Align high-water-mark up to match alignment.
  void align_hwm_up(size_t alignment);

  void align_hwm_to_large_page_size();
  void align_hwm_to_page_size();
  void align_hwm_to_padded_size(); // just enough to avoid false sharing

  template <typename T>
  void align_hwm_to() { align_hwm_up(alignof(T)); }

  const_address hwm() const { return _hwm; }

  void print_on(outputStream* st) const;

  DEBUG_ONLY(void verify() const);
};


#endif // SHARE_MEMORY_ARENA_ARENAHEAP_HPP
