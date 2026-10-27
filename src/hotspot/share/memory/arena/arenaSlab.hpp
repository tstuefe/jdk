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

#include <iterator>

#include "interpreter/oopMapCache.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"
#include "memory/arena/arenaBlock.hpp"
#include "memory/arena/layeredBitmap.hpp"

class ArenaSlabBase {
protected:

  static void commit_memory(const_blockptr_t from, const_blockptr_t to);
  static void discard_memory(const_blockptr_t from, const_blockptr_t to);

};

class ArenaSlab : public ArenaSlabBase {
public:
  typedef BitMap::idx_t IndexType;

private:

  // How much we overcommit when moving the commit boundary
  static constexpr IndexType num_granules_overcommit = 1;

  // Immutable data (Note, the bitmap objects are also immutable, since their geometry
  // never changes; only the maps themselves, and those are stored separately)
  const unsigned _block_size;
  const unsigned _granule_size;
  const unsigned _num_blocks;
  const unsigned _blocks_to_granule_shift;
  const unsigned _num_granules;
  const bool _can_be_released;
  const bool _allow_multi_block_allocations;
  const BlockRange<IndexType> _granule_range;
  const BlockRange<IndexType> _block_range;
  // 1 bit per granule; 1=granule is dirty
  LayeredBitmap _dirty_bitmap;
  // 1 bit per block; 1=block is in-use
  LayeredBitmap _in_use_bitmap;

  // Mutable data (store somewhere else?)
  // The commit boundary. It only ever goes upward.
  // This is not the mechanism to release memory: we release memory with
  // madvise(DONT_NEED). We only need the commit boundary because we don't
  // want to commit the full memory upfront.
  const_blockptr_t _commit_boundary;

  // for the decay
  unsigned _release_target;

  // Given a block index, returns its granule index
  IndexType block_index_to_granule_index(IndexType block_index) const {
    DEBUG_ONLY(_block_range.check_is_valid_block_index(block_index);)
    return block_index >> _blocks_to_granule_shift;
  }

  // Given a granule index, returns the block index of the first block in that granule
  IndexType granule_index_to_first_block_index(IndexType granule_idx) const {
    DEBUG_ONLY(_granule_range.check_is_valid_block_index(granule_idx);)
    return granule_idx << _blocks_to_granule_shift;
  }

  // Ensure this and all lower-address blocks are committed.
  void ensure_committed(const_blockptr_t block) {
    DEBUG_ONLY(_granule_range.check_is_valid_block_incl(block);)
    const const_blockptr_t granule_boundary = align_up(block, _granule_size);
    const const_blockptr_t new_commit_boundary = MIN2(_granule_range.end(), granule_boundary);
    if (new_commit_boundary > _commit_boundary) {
      const const_blockptr_t new_commit_boundary_over =
        MIN2(_granule_range.end(),
          new_commit_boundary + (num_granules_overcommit * _granule_size));
      commit_memory(_commit_boundary, new_commit_boundary_over);
      _commit_boundary = new_commit_boundary_over;
    }
  }

public:

  struct Storage {
    address _blocks;
    size_t _blocks_bytes;
    address _meta;
    size_t _meta_bytes;
  };

  // Calculate the space needs of this slab and updates storage_info->_blocks_bytes and
  // storage_info->_meta_bytes.
  // The returned values are the pure storage needs without any alignment
  static void calculate_storage_needs(size_t block_size, unsigned num_blocks, Storage* storage_info) {
    storage_info->_blocks_bytes = block_size * num_blocks;
    storage_info->_meta_bytes =
      LayeredBitmap::bytes_needed_for_bitmaps(num_blocks) * 2;
  }

  ArenaSlab(const Storage* storage_info,
            unsigned block_size, unsigned granule_size,
            unsigned num_blocks, bool can_be_released, bool allow_multi_block_allocations) :
    _block_size(block_size),
    _granule_size(granule_size),
    _num_blocks(num_blocks),
    _blocks_to_granule_shift(exact_log2(block_size) - exact_log2(granule_size)),
    _num_granules(num_blocks >> _blocks_to_granule_shift),
    _can_be_released(can_be_released),
    _allow_multi_block_allocations(allow_multi_block_allocations),
    _granule_range(storage_info->_blocks, granule_size, _num_granules),
    _block_range(_granule_range.base(), block_size, num_blocks),
    _dirty_bitmap(storage_info->_meta,
                  LayeredBitmap::bytes_needed_for_bitmaps(_num_granules), _num_granules),
    _in_use_bitmap((address)_dirty_bitmap.end(), LayeredBitmap::bytes_needed_for_bitmaps(num_blocks), num_blocks),
    _commit_boundary(_block_range.base()),
    _release_target(0)
  {
    assert(is_aligned(_num_blocks,blocks_per_granule()), "Sanity");
    assert(is_power_of_2(_block_size), "Sanity");
    assert(is_power_of_2(_granule_size), "Sanity");
    assert(is_power_of_2(num_blocks), "Sanity");
    assert(_num_blocks > 0, "Sanity");
    assert(_granule_size >= _block_size, "Sanity");

    assert(storage_info->_blocks_bytes >= _block_size * _num_blocks, "Sanity");
    assert(storage_info->_meta_bytes >=
      LayeredBitmap::bytes_needed_for_bitmaps(_num_blocks) +
      LayeredBitmap::bytes_needed_for_bitmaps(num_granules()), "Sanity");
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
    BitMap::idx_t idx = 0;
    unsigned n = 0;
    const_blockptr_t highest_address_block = base();
    while (n < outlen) {
      idx = _in_use_bitmap.find_first_clear_bit(idx, _num_blocks);
      if (idx == num_blocks()) {
        break; // slab is full
      }
      // Update bitmaps
      _in_use_bitmap.set_bit(idx);
      const BitMap::idx_t granule_idx = block_index_to_granule_index(idx);
      _dirty_bitmap.set_bit(granule_idx);

      const const_blockptr_t block = _block_range.index_to_block(idx);
      assert(highest_address_block < block, "Sanity"); // This method should be called under lock protection
      out[n] = block;
      highest_address_block = block;
      idx++; n++;
    }
    // Make sure the returned memory is committed
    ensure_committed(highest_address_block);
    return n;
  }

  // Allocate a contiguous range spanning multiple blocks. Only used for
  // huge blocks. Called rarely.
  const_blockptr_t allocate_huge_block(size_t size) {
    assert(_allow_multi_block_allocations, "Sanity");
    assert(size > 0, "num must be non-zero");
    assert(is_aligned(size, ArenaBlockHelpers::huge_block_granularity), "Size misaligned");

    const size_t size_aligned = align_up(size, block_size());
    const unsigned num = size_aligned / block_size();

    // We only ever call this for huge blocks, not standard blocks - and there the granules
    // are exactly block sized.
    assert(block_size() > ArenaBlockHelpers::max_standard_block_size, "only for huge blocks");
    assert(granule_size() == block_size(), "Sanity");

    // Find the lowest-address hole into which this allocation would fit
    BitMap::idx_t idx = 0, idx2 = 0;
    while (idx < num_blocks()) {
      idx = _in_use_bitmap.find_first_clear_bit(idx2, num_blocks());
      if (idx < num_blocks() - num) {
        idx2 = _in_use_bitmap.find_first_set_bit(idx, num_blocks());
        if (idx2 - idx >= num) {
          _in_use_bitmap.set_range(idx, idx2);
          _dirty_bitmap.set_range(idx, idx2); // huge blocks : granule == block size
        }
        const const_blockptr_t block = _block_range.index_to_block(idx);
        ensure_committed(block + size);
        return block;
      }
    }
    // nothing found
    return nullptr;
  }

  // Given a list of blocks, return them to the associated slab
  void deallocate_blocks(const_blockptr_t blocks[], unsigned num) {
    for (unsigned i = 0; i < num; i++) {
      const int block_index = _block_range.block_to_index(blocks[i]);
      assert(_in_use_bitmap.at(block_index), "double free?");
      _in_use_bitmap.clear_bit(i);
    }
  }

  // huge blocks. Called rarely.
  void deallocate_huge_block(const_blockptr_t block, size_t size) {
    // We only ever call this for huge blocks, not standard blocks - and there the granules
    // are exactly block sized. So we get away with treating granules as blocks and vice versa.
    assert(block_size() > ArenaBlockHelpers::max_standard_block_size, "only for huge blocks");
    assert(_granule_size == block_size(), "Sanity");

    DEBUG_ONLY(_block_range.check_is_valid_block(block);)
    assert(is_aligned(size, block_size()), "Alignment mismatch");

    const BitMap::idx_t idx = _block_range.block_to_index(block);
    const BitMap::idx_t idx2 = _block_range.block_to_index_incl(block + size);

#ifdef ASSERT
    {
      // check that the full range of this huge block is marked as "dirty" and "in-use"
      CHeapBitMap tmp(num_blocks(), mtTest, true);
      tmp.set_range(idx, idx2);
      assert(_dirty_bitmap.contains(tmp), "range not fully marked as dirty?");
      assert(_in_use_bitmap.contains(tmp), "range not fully marked as in-use?");
    }
#endif
    _in_use_bitmap.clear_range(idx, idx2);
  }

  bool contains(const_blockptr_t block) const {
    return _block_range.contains(block);
  }

#ifdef ASSERT
  void verify() const {
    assert(num_dirty_blocks() >= num_in_use_blocks(), "Sanity");
    assert(is_aligned(num_dirty_blocks(), blocks_per_granule()), "Sanity");
    _in_use_bitmap.verify();
    if (_can_be_released) {
      _dirty_bitmap.verify();
      // every in-use block must be dirty
      assert(_dirty_bitmap.contains(_in_use_bitmap), "Sanity");
      // the commit boundary must be equal or higher than the end of the highest dirty block
      const BitMap::idx_t granule_idx = _dirty_bitmap.find_last_set_bit(0, num_granules());
      if (granule_idx < num_granules()) {
        const BitMap::idx_t block_idx = granule_index_to_first_block_index(granule_idx);
        const const_blockptr_t block = _block_range.index_to_block(block_idx);
        assert((block + _block_size) <= _commit_boundary, "Commit boundary too low?");
      }
    }
  }
#endif

  unsigned num_in_use_blocks() const { return checked_cast<unsigned>(_in_use_bitmap.count_one_bits()); }
  unsigned num_dirty_granules() const { return checked_cast<unsigned>(_dirty_bitmap.count_one_bits()); }
  unsigned num_dirty_blocks() const { return checked_cast<unsigned>(_dirty_bitmap.count_one_bits()); }
};

#endif // SHARE_MEMORY_ARENA_ARENASLAB_HPP



















