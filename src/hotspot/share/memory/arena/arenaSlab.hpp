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

#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"
#include "memory/arena/arenaBlock.hpp"
#include "memory/arena/layeredBitmap.hpp"

class ArenaSlabBase {
protected:

  static void commit_memory(const_blockptr_t from, const_blockptr_t to);
  static void discard_memory(const_blockptr_t from, const_blockptr_t to);

};

template <size_t szclass,
          int num_blocks,
          size_t granulesize>
class ArenaSlab : public ArenaSlabBase {

  STATIC_ASSERT(is_power_of_2(szclass));
  STATIC_ASSERT(is_power_of_2(granulesize));
  STATIC_ASSERT(is_power_of_2(num_blocks));
  STATIC_ASSERT(granulesize >= szclass);

  const BlockRange<BitMap::idx_t, szclass> _blockrange;
  const BlockRange<BitMap::idx_t, granulesize> _granule_range;

  static constexpr size_t blocksize = szclass;
  static constexpr size_t total_size = blocksize * num_blocks;
  static constexpr int num_granules = total_size/granulesize;
  static constexpr int blocks_per_granule = granulesize / blocksize;

  // How much we overcommit when moving the commit boundary
  static constexpr int num_granules_overcommit = 1;

  // 1 if block is in use (owned by callers); 0 if not
  LayeredBitmap<num_blocks/BitsPerWord, BitsPerWord> _in_use_bitmap;

  // 1 if granule is dirty; 0 if not
  LayeredBitmap<num_granules/BitsPerWord, BitsPerWord> _dirty_bitmap;

  // The commit boundary. It only ever goes upward.
  // This is not the mechanism to release memory: we release memory with
  // madvise(DONT_NEED). We only need the commit boundary because we don't
  // want to commit the full memory upfront.
  const_blockptr_t _commit_boundary; // not yet used, comes later

  // Given a block index, returns its granule index
  template <typename IndexType>
  IndexType block_index_to_granule_index(IndexType block_index) const {
    DEBUG_ONLY(_blockrange.check_is_valid_block_index(block_index);)
    return block_index / blocks_per_granule;
  }

  // Ensure this and all lower-address blocks are committed.
  void ensure_committed(const_blockptr_t block) {
    DEBUG_ONLY(_granule_range.check_is_valid_block_incl(block);)
    const const_blockptr_t granule_boundary = align_up(block, granulesize);
    const const_blockptr_t new_commit_boundary = MIN2(_granule_range.end(), granule_boundary);
    if (new_commit_boundary > _commit_boundary) {
      const const_blockptr_t new_commit_boundary_over =
        MIN2(_granule_range.end(),
          new_commit_boundary + (num_granules_overcommit * granulesize));
      commit_memory(_commit_boundary, new_commit_boundary_over);
      _commit_boundary = new_commit_boundary_over;
    }
  }

public:

  ArenaSlab(const_blockptr_t base, size_t size)
    : _blockrange(base), _commit_boundary(base)
  {
    assert(size == total_size, "Range size mismatch");
  }

  const_blockptr_t base() const { return _blockrange.base(); }
  const_blockptr_t end() const { return _blockrange.end(); }

  // Allocate a number of blocks and return their addresses in the user-provided array;
  // returns the number of blocks actually allocated (which may be smaller if we cannot allocate)
  // The returned blocks are guaranteed to be ordered by address, low to high.
  int allocate_blocks(const_blockptr_t out[], int outlen) {
    BitMap::idx_t idx = 0;
    int n = 0;
    const_blockptr_t highest_address_block = base();
    while (n < outlen) {
      idx = _in_use_bitmap.find_first_clear_bit(idx);
      if (idx == num_blocks) {
        break; // slab is full
      }
      // Update bitmaps
      _in_use_bitmap.set_bit(idx);
      const BitMap::idx_t granule_idx = block_index_to_granule_index(idx);
      _dirty_bitmap.set_bit(granule_idx);

      const const_blockptr_t block = _blockrange.index_to_block(idx);
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
    assert(size > 0, "num must be non-zero");
    assert(is_aligned(size, ArenaBlockHelpers::huge_block_granularity), "Size misaligned");

    const int num = align_up(size, szclass);

    // We only ever call this for huge blocks, not standard blocks - and there the granules
    // are exactly block sized.
    assert(szclass > ArenaBlockHelpers::max_standard_block_size, "only for huge blocks");
    assert(granulesize == szclass, "Sanity");

    // Find the lowest-address hole into which this allocation would fit
    BitMap::idx_t idx = 0, idx2 = 0;
    while (idx < num_blocks) {
      idx = _in_use_bitmap.find_first_clear_bit(idx2, num_blocks);
      if (idx < num_blocks - num) {
        idx2 = _in_use_bitmap.find_first_set_bit(idx);
        if (idx2 - idx >= num) {
          _in_use_bitmap.set_range(idx, idx2);
          _dirty_bitmap.set_range(idx, idx2); // huge blocks : granule == block size
        }
        const const_blockptr_t block = _blockrange.index_to_block(idx);
        ensure_committed(block + size);
        return block;
      }
    }
    // nothing found
    return nullptr;
  }

  // Given a list of blocks, return them to the associated slab
  void deallocate_blocks(const_blockptr_t blocks[], int num) {
    for (int i = 0; i < num; i++) {
      const int block_index = _blockrange.block_to_index(blocks[i]);
      assert(_in_use_bitmap.at(block_index), "double free?");
      _in_use_bitmap.clear_bit(i);
    }
  }

  // huge blocks. Called rarely.
  void deallocate_huge_block(const_blockptr_t block, size_t size) {
    // We only ever call this for huge blocks, not standard blocks - and there the granules
    // are exactly block sized. So we get away with treating granules as blocks and vice versa.
    assert(szclass > ArenaBlockHelpers::max_standard_block_size, "only for huge blocks");
    assert(granulesize == szclass, "Sanity");

    DEBUG_ONLY(_blockrange.check_is_valid_block_(block);)
    assert(is_aligned(size, szclass), "Alignment mismatch");

    const BitMap::idx_t idx = _blockrange.block_to_index(block);
    const BitMap::idx_t idx2 = _blockrange.block_to_index_incl(block + size);

#ifdef ASSERT
    {
      // check that the full range of this huge block is marked as "dirty" and "in-use"
      CHeapBitMap tmp(num_blocks, mtTest, true);
      tmp.set_range(idx, idx2);
      assert(_dirty_bitmap.contains(tmp), "range not fully marked as dirty?");
      assert(_in_use_bitmap.contains(tmp), "range not fully marked as in-use?");
    }
#endif

    _in_use_bitmap.clear(idx, idx2);
  }

  void contains(const_blockptr_t block) const { return _blockrange.contains(block); }

};

#endif // SHARE_MEMORY_ARENA_ARENASLAB_HPP



















