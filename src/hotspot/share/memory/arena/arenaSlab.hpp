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
#include "runtime/os.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"


class ArenaSlab {
public:
  typedef BitMap::idx_t IndexType;

private:

  // How much we overcommit when moving the commit boundary
  static constexpr IndexType num_granules_overcommit = 1;

  // Immutable data (Note, the bitmap objects are also immutable, since their geometry
  // never changes; only the maps themselves, and those are stored separately)
  ArenaHeap* const _heap;
  const unsigned _block_size;
  const unsigned _granule_size;
  const unsigned _num_blocks;
  const unsigned _blocks_to_granule_shift;
  const unsigned _num_granules;
  // decay factor in percent, see decaying.
  // Number is in percentage, e.g. 75 = 0.75
  const uint8_t _decay_factor;
  // Blocks below this index are never released, and (if large pages are enabled)
  // backed by large pages
  const unsigned _release_boundary;
  const BlockRange<IndexType> _granule_range;
  const BlockRange<IndexType> _block_range;
  // 1 bit per granule; 1=granule is dirty
  LayeredBitmap _dirty_bitmap;
  // 1 bit per block; 1=block is in-use
  LayeredBitmap _in_use_bitmap;

  // Mutable data (we should probably store them together with the
  // bitmaps since those get changed together anyway)

  // The commit boundary - index of the first uncommitted granule.
  // Only ever moves upward. We release memory via madvise, which is
  // the actual heavy-duty release mechanism. The commit boundary is
  // only needed to be conservative about pushing the process commit
  // charge since the OS may interfere (eg on Linux with strict overcommit)
  IndexType _commit_boundary;

  // The retain target from the last cycle (see tick handling)
  IndexType _retain_target;

  // Current number of blocks in-use
  unsigned _num_blocks_in_use;
  // Highest number of blocks in-use since the last tick
  unsigned _num_blocks_in_use_max;

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
    return _granule_size >= 2 * M; // Todo.
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
      _heap->commit_memory(p1, p2, use_large_pages());
      _commit_boundary = new_commit_boundary_overshoot;
    }
  }

  static uint8_t calc_decay_factor_percent(double factor) {
    assert(factor >= 0.0 && factor <= 1.0, "factor must be between 0.0 and 1.0");
    const unsigned i = (unsigned)((factor * 100.0) + 0.5);
    return checked_cast<uint8_t>(i);
  }

  void increase_block_counter(unsigned num) {
    _num_blocks_in_use += num;
    assert(_num_blocks_in_use <= num_blocks(), "Sanity");
    _num_blocks_in_use_max = MAX2(_num_blocks_in_use, _num_blocks_in_use_max);
  }

  void decrease_block_counter(unsigned num) {
    assert(_num_blocks_in_use >= num, "Sanity");
    _num_blocks_in_use -= num;
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

  // Returns size, in bytes, a slab would occupy in the arena heap
  static size_t calc_arena_space(size_t block_size, unsigned num_blocks) {
    assert(is_power_of_2(block_size), "Sanity");
    assert(is_power_of_2(num_blocks), "Sanity");
    return block_size * num_blocks;
  }

  // Returns size, in bytes, needed for the slab metadata
  static size_t calc_metadata_space(size_t block_size, size_t granule_size, unsigned num_blocks) {
    const unsigned num_granules = calc_num_granules(block_size, granule_size, num_blocks);
    assert(is_power_of_2(num_blocks), "Sanity");
    return LayeredBitmap::bytes_needed(num_blocks) +
           LayeredBitmap::bytes_needed(num_granules);
  }

  ArenaSlab(ArenaHeap* heap,
            const_address arena_base, size_t arena_size,
            address metadata_base, size_t metadata_size,
            unsigned block_size, unsigned granule_size,
            unsigned num_blocks, unsigned release_boundary,
            double decay_factor) :
    _heap(heap),
    _block_size(block_size),
    _granule_size(granule_size),
    _num_blocks(num_blocks),
    _blocks_to_granule_shift(calc_block_to_granule_shift(block_size, granule_size)),
    _num_granules(calc_num_granules(block_size, granule_size, num_blocks)),
    _decay_factor(calc_decay_factor_percent(decay_factor)),
    _release_boundary(release_boundary),
    _granule_range(arena_base, granule_size, _num_granules),
    _block_range(arena_base, block_size, num_blocks),
    _dirty_bitmap(metadata_base, LayeredBitmap::bytes_needed(_num_granules), _num_granules),
    _in_use_bitmap(metadata_base + LayeredBitmap::bytes_needed(_num_granules), LayeredBitmap::bytes_needed(num_blocks),
                   num_blocks),
    _commit_boundary(0),
    _retain_target(0),
    _num_blocks_in_use(0),
    _num_blocks_in_use_max(0)
  {
    assert(is_aligned(_num_blocks,blocks_per_granule()), "Sanity");
    assert(is_power_of_2(_block_size), "Sanity");
    assert(is_power_of_2(_granule_size), "Sanity");
    assert(is_power_of_2(num_blocks), "Sanity");
    assert(_num_blocks > 0, "Sanity");
    assert(_granule_size >= _block_size, "Sanity");
    assert(_granule_size >= os::vm_page_size(), "Sanity");

    assert(is_aligned(_block_range.base(), ArenaBlockHelpers::slab_alignment), "Sanity");

    // The release boundary must be aligned to be able to back it with huge pages
    assert(is_aligned(_block_range.index_to_block(_release_boundary),
             ArenaBlockHelpers::slab_alignment), "Sanity");
    assert(_release_boundary <= num_blocks, "Sanity");

    assert(arena_size >= _block_size * _num_blocks, "Sanity");
    assert(metadata_size >=
           LayeredBitmap::bytes_needed(_num_blocks) +
           LayeredBitmap::bytes_needed(num_granules()), "Sanity");
  }

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
  unsigned allocate_blocks(const_blockptr_t out[], unsigned outlen) {
    IndexType idx = 0, highest_block_idx = 0;
    unsigned n = 0;
    while (n < outlen) {
      idx = _in_use_bitmap.find_first_clear_bit(idx, _num_blocks);
      if (idx == num_blocks()) {
        break; // slab is full
      }
      // Update bitmaps
      _in_use_bitmap.set_bit(idx);
      const IndexType granule_idx = block_index_to_granule_index(idx);
      _dirty_bitmap.set_bit(granule_idx);

      assert(n == 0 || highest_block_idx < idx, "Should raise monotonously");
      highest_block_idx = MAX2(idx, highest_block_idx);

      const const_blockptr_t block = _block_range.index_to_block(idx);
      out[n] = block;
      idx++; n++;
    }
    // Make sure the returned memory is committed
    if (n > 0) {
      ensure_committed(highest_block_idx);
    }

    // Update counters
    increase_block_counter(n);

    return n;
  }

  // Allocate a contiguous range spanning multiple blocks. Only used for
  // huge blocks. Called rarely.
  const_blockptr_t allocate_huge_block(size_t size) {
    assert(size > 0, "num must be non-zero");
    assert(is_aligned(size, ArenaBlockHelpers::huge_block_granularity), "Size misaligned");

    const size_t size_aligned = align_up(size, block_size());
    const unsigned num = size_aligned / block_size();

    // We only ever call this for huge blocks, not standard blocks - and there the granules
    // are exactly block sized.
    assert(block_size() > ArenaBlockHelpers::max_standard_block_size, "only for huge blocks");
    assert(granule_size() == block_size(), "Sanity");

    // Find the lowest-address hole into which this allocation would fit
    IndexType idx = 0, idx2 = 0;
    while (idx < num_blocks()) {
      idx = _in_use_bitmap.find_first_clear_bit(idx2, num_blocks());
      if (idx < num_blocks()) {
        idx2 = _in_use_bitmap.find_first_set_bit(idx, num_blocks());
        if (idx2 - idx >= num) {
          // Found a hole large enough for this allocation
          _in_use_bitmap.set_range(idx, idx + num);
          _dirty_bitmap.set_range(idx, idx + num); // huge blocks : granule == block size

          // Move commit boundary up
          const IndexType highest_block_idx = idx + num - 1;
          ensure_committed(highest_block_idx);

          // Update counters
          increase_block_counter(num);

          const const_blockptr_t block = _block_range.index_to_block(idx);
          return block;
        }
        idx = idx2;
      }
    }
    // nothing found
    return nullptr;
  }

  // Given a list of blocks, return them to the associated slab
  void deallocate_blocks(const_blockptr_t blocks[], unsigned num) {
    for (unsigned i = 0; i < num; i++) {
      const IndexType block_index = _block_range.block_to_index(blocks[i]);
      assert(_in_use_bitmap.at(block_index), "double free?");
      _in_use_bitmap.clear_bit(block_index);
    }
    // Update counters
    decrease_block_counter(num);
  }

  // huge blocks. Called rarely.
  void deallocate_huge_block(const_blockptr_t block, size_t size) {
    // We only ever call this for huge blocks, not standard blocks - and there the granules
    // are exactly block sized. So we get away with treating granules as blocks and vice versa.
    assert(block_size() > ArenaBlockHelpers::max_standard_block_size, "only for huge blocks");
    assert(_granule_size == block_size(), "Sanity");

    DEBUG_ONLY(_block_range.check_is_valid_block(block);)
    assert(is_aligned(size, block_size()), "Alignment mismatch");

    const IndexType idx = _block_range.block_to_index(block);
    const IndexType idx2 = _block_range.block_to_index_incl(block + size);

#ifdef ASSERT
    {
      // check that the full range of this huge block is marked as "dirty" and "in-use"
      CHeapBitMap tmp(num_blocks(), mtInternal, true);
      tmp.set_range(idx, idx2);
      assert(_dirty_bitmap.contains(tmp), "range not fully marked as dirty?");
      assert(_in_use_bitmap.contains(tmp), "range not fully marked as in-use?");
    }
#endif
    _in_use_bitmap.clear_range(idx, idx2);

    // Update counters
    decrease_block_counter(idx2 - idx);
  }

  bool contains(const_blockptr_t block) const {
    return _block_range.contains(block);
  }

#ifdef ASSERT
  void verify() const {
    assert(count_dirty_blocks() >= count_in_use_blocks(), "Sanity");
    assert(is_aligned(count_dirty_blocks(), blocks_per_granule()), "Sanity");
    _in_use_bitmap.verify();
    assert(_in_use_bitmap.count_one_bits() == _num_blocks_in_use, "Sanity");
    assert(_num_blocks_in_use_max >= _num_blocks_in_use, "Sanity");
    _dirty_bitmap.verify();
    // every in-use block must be dirty
    CHeapBitMap tmp(num_blocks(), mtTest, false);
    CHeapBitMap tmp2(num_blocks(), mtTest, false);
    _dirty_bitmap.convert_to_flat_bitmap_scaled(tmp, blocks_per_granule());
    _in_use_bitmap.convert_to_flat_bitmap(tmp2);
    assert(tmp.contains(tmp2), "Sanity");
    // the commit boundary must be equal or higher than the end of the highest dirty block
    const IndexType granule_idx = _dirty_bitmap.find_last_set_bit(0, num_granules());
    if (granule_idx < num_granules()) {
      assert(granule_idx < _commit_boundary, "Commit boundary too low?");
    }
  }
#endif

  unsigned count_in_use_blocks() const  { return checked_cast<unsigned>(_in_use_bitmap.count_one_bits()); }
  unsigned count_dirty_granules() const { return checked_cast<unsigned>(_dirty_bitmap.count_one_bits()); }
  unsigned count_dirty_blocks() const   { return checked_cast<unsigned>(_dirty_bitmap.count_one_bits() * blocks_per_granule()); }

};

#endif // SHARE_MEMORY_ARENA_ARENASLAB_HPP
