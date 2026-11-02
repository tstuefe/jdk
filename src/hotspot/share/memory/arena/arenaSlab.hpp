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

#ifndef SHARE_MEMORY_ARENA_ARENASLAB_HPP
#define SHARE_MEMORY_ARENA_ARENASLAB_HPP

#include "memory/arena/arenaBlock.hpp"
#include "memory/arena/arenaHeap.hpp"
#include "memory/arena/layeredBitmap.hpp"
#include "runtime/mutex.hpp"
#include "runtime/os.hpp"
#include "utilities/debug.hpp"
#include "utilities/deferredStatic.hpp"
#include "utilities/globalDefinitions.hpp"

class ArenaSlab {
public:
  typedef BitMap::idx_t IndexType;

private:

  // How much we overcommit when moving the commit boundary
  static constexpr IndexType num_granules_overcommit = 1;

  // We split data associated with the slab into different groups. These data
  // are kept separately from each other in its own cache lines:
  // 1) read-only data:
  //   Data that is read-only post init.
  //   These are housed together with read-only data from other slabs
  //   (see SlabGroup)
  // 2) hot data:
  //   Data that change on every allocation/deallocation.
  //   This includes the Mutex itself as well as the allocation counters.
  //   Effectively, these cachelines are handed over from core to core
  //   when a slab is accessed by different cores in succession.
  // 3) in-use bitmap:
  //   Data changes per allocation.
  // 4) is-dirty bitmap:
  //   mostly read-only.
  // 5) tick-only:
  //   Updated on a per-tick base.
  //


  // 1. Read-only data

  // The arena heap is used for the slab memory
  ArenaHeap* const _arena_heap;
  // The metadata heap is used for the bitmaps
  ArenaMetadataHeap* const _metadata_heap;
  const unsigned _block_size;
  const unsigned _granule_size;
  const unsigned _num_blocks;
  const unsigned _blocks_to_granule_shift;
  const unsigned _num_granules;
  // decay factor in percent, see decaying.
  // Number is in percentage, e.g. 75 = 0.75
  const uint8_t _decay_factor;
  // Release Boundary (in number-of-granules). Granules below this are committed up-front
  // and never released
  const unsigned _release_boundary;

  const_blockptr_t _base;
  const BlockRange<IndexType> _block_range;
  const BlockRange<IndexType> _granule_range;

  // 1 bit per granule; 1=granule is dirty
  LayeredBitmap _dirty_bitmap;

  // 1 bit per block; 1=block is in-use
  LayeredBitmap _in_use_bitmap;

  // The commit boundary - index of the first uncommitted granule.
  // Only modified a couple of times, and only moves upward. We never
  // uncommit.
  // Note that this is not strictly const, but we modify it rarely
  // enough to place it together with the other read-only data
  IndexType _commit_boundary;

  // 2) hot data
  struct alignas(DEFAULT_PADDING_SIZE) SlabHotData {
    // Current number of blocks in-use
    unsigned _num_blocks_in_use;
    // Highest number of blocks in-use since the last tick
    unsigned _num_blocks_in_use_max;
    // Add more if needed, e.g. per allocation/deallocation statistics

    // The lock for this slab.
    PlatformMutex _lock;

    SlabHotData();
  };

  // 5) per-tick data
  struct alignas(DEFAULT_PADDING_SIZE) SlabPerTickData {
    // The retain target from the last cycle (see tick handling)
    IndexType _retain_target;

    SlabPerTickData() : _retain_target(0) {};
  };
  SlabHotData* const _hot;
  SlabPerTickData* const _per_tick;

  // Given a block index, returns its granule index
  IndexType block_index_to_granule_index(IndexType block_index) const {
    DEBUG_ONLY(_block_range.check_is_valid_block_index(block_index);)
    return block_index >> _blocks_to_granule_shift;
  }

  // Given a block index, returns its granule index
  IndexType block_index_to_granule_index_incl(IndexType block_index) const {
    DEBUG_ONLY(_block_range.check_is_valid_block_index_incl(block_index);)
    return block_index >> _blocks_to_granule_shift;
  }

  // Given a granule index, returns the block index of the first block in that granule
  IndexType granule_index_to_first_block_index(IndexType granule_idx) const {
    DEBUG_ONLY(_granule_range.check_is_valid_block_index(granule_idx);)
    return granule_idx << _blocks_to_granule_shift;
  }

  // Given a granule index, returns the block index of the first block in that granule
  IndexType granule_index_to_first_block_index_incl(IndexType granule_idx) const {
    DEBUG_ONLY(_granule_range.check_is_valid_block_index_incl(granule_idx);)
    return granule_idx << _blocks_to_granule_shift;
  }

  bool use_large_pages() const {
    return
    LINUX_ONLY(_granule_size >= ArenaHeap::large_page_size)
    NOT_LINUX(false)
    ;
  }

  // Ensure this and all lower-address blocks are committed.
  void ensure_committed(IndexType block_index) {
    DEBUG_ONLY(_block_range.check_is_valid_block_index(block_index);)
    const IndexType granule_index = block_index_to_granule_index(block_index);
    const IndexType new_commit_boundary = MAX2(granule_index + 1, _commit_boundary);
    if (new_commit_boundary > _commit_boundary) {
      // We commit a bit more to avoid having to do this too often
      const IndexType new_commit_boundary_overshoot =
        MIN2((IndexType)num_granules(), new_commit_boundary + num_granules_overcommit);
      const const_blockptr_t p1 = _granule_range.index_to_block(_commit_boundary);
      const const_blockptr_t p2 = _granule_range.index_to_block_incl(new_commit_boundary_overshoot);
      _arena_heap->commit_memory(p1, p2);
#ifdef LINUX
      _arena_heap->advise_thp_use(p1, p2, use_large_pages());
#endif
      _commit_boundary = new_commit_boundary_overshoot;
    }
  }

  static uint8_t calc_decay_factor_percent(double factor) {
    assert(factor >= 0.0 && factor <= 1.0, "factor must be between 0.0 and 1.0");
    const unsigned i = (unsigned)((factor * 100.0) + 0.5);
    return checked_cast<uint8_t>(i);
  }

  void increase_block_counter(unsigned num) {
    _hot->_num_blocks_in_use += num;
    assert(_hot->_num_blocks_in_use <= num_blocks(), "Sanity");
    _hot->_num_blocks_in_use_max = MAX2(_hot->_num_blocks_in_use, _hot->_num_blocks_in_use_max);
  }

  void decrease_block_counter(unsigned num) {
    assert(_hot->_num_blocks_in_use >= num, "Sanity");
    _hot->_num_blocks_in_use -= num;
  }

  static unsigned calc_block_to_granule_shift(size_t block_size, size_t granule_size) {
    const unsigned l2_block_size = exact_log2(block_size);
    const unsigned l2_granule_size = exact_log2(granule_size);
    assert(l2_block_size <= l2_granule_size, "Sanity");
    return l2_granule_size - l2_block_size;
  }

  static unsigned calc_num_granules(size_t block_size, size_t granule_size, unsigned num_blocks) {
    return num_blocks >> calc_block_to_granule_shift(block_size, granule_size);
  }

public:

  ArenaSlab(ArenaHeap* arena_heap,
            ArenaMetadataHeap* metadata_heap,
            unsigned block_size, unsigned granule_size,
            unsigned num_blocks, unsigned release_boundary,
            double decay_factor);

  const_blockptr_t base() const { return _block_range.base(); }
  const_blockptr_t end() const { return _block_range.end(); }
  size_t block_size() const { return _block_size; }
  size_t granule_size() const { return _granule_size; }
  unsigned num_blocks() const { return _num_blocks; }
  unsigned num_granules() const { return _num_granules; }
  unsigned blocks_per_granule() const { return 1u << _blocks_to_granule_shift; }

  // Allocate a number of blocks and return their addresses in the user-provided array;
  // returns the number of blocks actually allocated (which may be smaller if we cannot allocate)
  // The returned blocks are guaranteed to be ordered by address, low to high.
  unsigned allocate_blocks(const_blockptr_t out[], unsigned outlen);

  // Given a list of blocks, return them to the associated slab
  void deallocate_blocks(const_blockptr_t blocks[], unsigned num);

  // Allocate a huge block (a contiguous range spanning multiple blocks).
  const_blockptr_t allocate_huge_block(size_t size);

  // Deallocate a huge block. Called rarely.
  void deallocate_huge_block(const_blockptr_t block, size_t size);

  bool contains(const_blockptr_t block) const {
    return _block_range.contains(block);
  }

#ifdef ASSERT
  void verify() const;
#endif

  // Mostly for tests:
  void get_in_use_map(BitMap& out) const { _in_use_bitmap.convert_to_flat_bitmap(out); }

  unsigned count_in_use_blocks() const  { return checked_cast<unsigned>(_in_use_bitmap.count_one_bits()); }
  unsigned count_dirty_granules() const { return checked_cast<unsigned>(_dirty_bitmap.count_one_bits()); }
  unsigned count_dirty_blocks() const   { return checked_cast<unsigned>(_dirty_bitmap.count_one_bits() * blocks_per_granule()); }

};

#endif // SHARE_MEMORY_ARENA_ARENASLAB_HPP
