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
#include "memory/arena/arenaStatistics.hpp"
#include "memory/arena/layeredBitmap.hpp"
#include "runtime/mutex.hpp"
#include "runtime/os.hpp"
#include "utilities/debug.hpp"
#include "utilities/deferredStatic.hpp"
#include "utilities/globalDefinitions.hpp"

class ArenaSlabTester;

class ArenaSlab {
  friend class ArenaSlabTester;
public:
  typedef BitMap::idx_t IndexType;

  // factor out construction parameters to make construction of the
  // standard slab group more palatable.
  struct Parameters {
    // all sizes in bytes
    size_t block_size;
    size_t granule_size;
    // total number of blocks
    unsigned num_blocks;
    // Release boundary, in number of blocks
    // (must be a multiple of granule size)
    unsigned release_boundary;
    // if true, the resident section < release_boundary
    // will use large pages
    bool resident_section_uses_large_pages;
    // Between 0.0 and 1.0
    double decay_factor;
  };

private:

  // This is the step size in which the commit boundary moves upward.
  // Note: we don't uncommit to free memory. Instead, to free memory, we discard
  // it (e.g. madvise(dont_need) on linux). So we only commit at the start of
  // the JVM. We could keep matters simple and just commit the whole slab upfront.
  // But that may have implications on OSes that punish overcommitting. So we move
  // a "commit boundary" below everything is committed. However, we don't want to
  // do this too often since this it hits startup, mostly. So the granularity
  // of committing is kept deliberately pretty coarse.
  static constexpr size_t commit_granularity = 32 * M;

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
  const size_t _block_size;
  const size_t _granule_size;
  const unsigned _num_blocks;
  const unsigned _blocks_to_granule_shift;
  const unsigned _num_granules;
  // Release Boundary (in number-of-granules). Granules below this are committed up-front
  // and never released
  const unsigned _release_boundary;
  // decay factor [0.0, 1.0)
  const double _decay_factor;

  const_blockptr_t _base;
  const BlockRange<IndexType> _block_range;
  const BlockRange<IndexType> _granule_range;

  // 1 bit per granule; 1=granule is dirty
  LayeredBitmap _dirty_bitmap;

  // 1 bit per block; 1=block is in-use
  LayeredBitmap _in_use_bitmap;

  // Commit Boundary (in number-of-granules).
  // Note that this is not strictly const, but we modify it rarely
  // enough to place it together with the other read-only data
  IndexType _commit_boundary;

#ifdef ARENA_STATS
  struct SlabStatistics {
    //  Number of standard blocks allocated, deallocated
    uint64_t _num_standard_block_allocated;
    uint64_t _num_standard_block_deallocated;
    // How many bulk allocations/deallocations we processed
    uint64_t _num_bulk_alloc_ops;
    uint64_t _num_bulk_dealloc_ops;
    // How many huge blocks we allocated/deallocated in total
    uint64_t _num_huge_blocks_allocated;
    uint64_t _num_huge_blocks_deallocated;
    // How often we discarded memory
    uint64_t _num_discard_ops;
    void account_bulk_alloc(unsigned num);
    void account_bulk_dealloc(unsigned num);
    SlabStatistics();
  };
#endif

  // 2) hot data
  struct alignas(DEFAULT_PADDING_SIZE) SlabHotData {
    // Current number of blocks in-use
    unsigned _num_blocks_in_use;
    // Highest number of blocks in-use since the last tick
    unsigned _num_blocks_in_use_peak;
#ifdef ARENA_STATS
    SlabStatistics _stats;
#endif

    // The lock for this slab.
    PlatformMutex _lock;

    SlabHotData();
  };

  // 5) per-tick data
  struct alignas(DEFAULT_PADDING_SIZE) SlabPerTickData {
    // The retain target from the last cycle (see tick handling),
    // in number of blocks
    unsigned _retain_target;

    SlabPerTickData() : _retain_target(0) {};
  };
  SlabHotData* const _hot;
  SlabPerTickData* const _per_tick;

  // Given a block index, returns its granule index
  IndexType block_index_to_granule_index(IndexType block_index) const {
    DEBUG_ONLY(_block_range.verify_is_valid_block_index(block_index);)
    return block_index >> _blocks_to_granule_shift;
  }

  // Given a block index, returns its granule index
  IndexType block_index_to_granule_index_incl(IndexType block_index) const {
    DEBUG_ONLY(_block_range.verify_is_valid_block_index_incl(block_index);)
    return block_index >> _blocks_to_granule_shift;
  }

  // Given a granule index, returns the block index of the first block in that granule
  IndexType granule_index_to_first_block_index(IndexType granule_idx) const {
    DEBUG_ONLY(_granule_range.verify_is_valid_block_index(granule_idx);)
    return granule_idx << _blocks_to_granule_shift;
  }

  // Given a granule index, returns the block index of the first block in that granule
  IndexType granule_index_to_first_block_index_incl(IndexType granule_idx) const {
    DEBUG_ONLY(_granule_range.verify_is_valid_block_index_incl(granule_idx);)
    return granule_idx << _blocks_to_granule_shift;
  }

  bool use_large_pages() const {
    return
    LINUX_ONLY(_granule_size >= ArenaHeap::large_page_size)
    NOT_LINUX(false)
    ;
  }

  // Ensure this and all lower-address blocks are committed.
  void ensure_committed(IndexType block_index);
  void mark_granule_dirty(IndexType granule_idx);
  void mark_granule_range_dirty(IndexType granule_begidx, IndexType granule_endidx);

  void increase_block_counter(unsigned num);
  void decrease_block_counter(unsigned num);

  // Release mechanics
  bool has_burst_section() const { return _release_boundary < num_granules(); }

  // Releases granules between [beg, end)
  void release_granule_memory(IndexType beg, IndexType end);

  // Given a release max R, a granule range [G_lo, G_hi) and a minimum release len MRL,
  // function will search [G_lo, G_hi), backwards - highest addresses first - for runs of
  // dirty but releasable granules that are longer than MRL, release them to the heap,
  // and clear their dirty flag. It will stop once the number of released granules reaches
  // R, and return the number of actually released granules.
  unsigned harvest_dirty_granules(unsigned release_max,
                                  IndexType granule_lo, IndexType granule_hi,
                                  unsigned min_release_len);

  void clear_dirty_granules(bool squeeze);

public:

  ArenaSlab(ArenaHeap* arena_heap,
            ArenaMetadataHeap* metadata_heap,
            const Parameters* params);

  // This constructor creates an inactive slab that cannot be used for anything.
  // We use it to fill in slots in the slab group for block sizes that are unused
  // in the current scheme.
  ArenaSlab();

  const_blockptr_t base() const { return _block_range.base(); }
  size_t size_in_bytes() const { return _block_range.size_in_bytes(); }
  const_blockptr_t end() const { return _block_range.end(); }
  size_t block_size() const { return _block_size; }
  size_t granule_size() const { return _granule_size; }
  unsigned num_blocks() const { return _num_blocks; }
  unsigned num_granules() const { return _num_granules; }
  unsigned release_boundary() const { return _release_boundary; }
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

  // Returns true if this is a valid slab
  bool is_valid() const { return _base != nullptr; }

#ifdef ASSERT
  void verify() const;
  void verify_block(const_blockptr_t block) const;
#endif

  // Mostly for tests:
  void get_in_use_map(BitMap& out) const { _in_use_bitmap.convert_to_flat_bitmap(out); }

  unsigned count_in_use_blocks() const  { return checked_cast<unsigned>(_in_use_bitmap.count_one_bits()); }
  unsigned count_dirty_granules() const { return checked_cast<unsigned>(_dirty_bitmap.count_one_bits()); }
  unsigned count_dirty_blocks() const   { return checked_cast<unsigned>(_dirty_bitmap.count_one_bits() * blocks_per_granule()); }

  void tick(unsigned tick_number);

  void print_on(outputStream* st) const;

};

// The ArenaSlabGroup holds one slab per block size. This object exists as
// singleton (outside of tests).
class ArenaSlabGroup {

  static constexpr unsigned num_slots = ArenaBlockHelpers::num_size_classes;

  ArenaSlab _slabs[num_slots];

public:

  // Creates a slab group with the given slab geometries (see params)
  ArenaSlabGroup(ArenaHeap* arena_heap,
                 ArenaMetadataHeap* metadata_heap,
                 const ArenaSlab::Parameters params[num_slots]);

  // Creates a slab group with the default geometries for hotspot
  ArenaSlabGroup(ArenaHeap* arena_heap,
                 ArenaMetadataHeap* metadata_heap);

  ArenaSlab* slab_for_size_class(block_size_class_t szclass) {
    assert(ArenaBlockHelpers::is_valid_size_class(szclass), "sanity");
    assert(szclass < num_slots, "sanity");
    ArenaSlab* const slab = &(_slabs[szclass]);
    assert(slab->is_valid(), "Slab for this size class not valid (%u)", szclass);
    return slab;
  }

  void tick(unsigned tick_number);

  void print_on(outputStream* st) const;

  DEBUG_ONLY(void verify() const);

};

#endif // SHARE_MEMORY_ARENA_ARENASLAB_HPP
