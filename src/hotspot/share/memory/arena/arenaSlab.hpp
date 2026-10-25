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

  const BlockRange<szclass> _blockrange;
  const BlockRange<granulesize> _granule_range;

  static constexpr size_t _size = szclass * num_blocks;
  static constexpr int _num_granules = _size/granulesize;

  // How much we overcommit when moving the commit boundary
  static constexpr int num_granules_overcommit = 1;

  // 1 if block is in use (owned by callers); 0 if not
  LayeredBitmap<num_blocks/BitsPerWord, BitsPerWord> _blocks_in_use;

  // 1 if granule is dirty; 0 if not
  LayeredBitmap<num_blocks/BitsPerWord, BitsPerWord> _granule_is_dirty;

  // The commit boundary. It only ever goes upward.
  // This is not the mechanism to release memory: we release memory with
  // madvise(DONT_NEED). We only need the commit boundary because we don't
  // want to commit the full memory upfront.
  const_blockptr_t _commit_boundary; // not yet used, comes later

  // Given a block index, returns its granule index
  int block_index_to_granule_index(int block_index) const {
    DEBUG_ONLY(_blockrange.check_is_valid_block_index(block_index);)
    const const_blockptr_t block = _blockrange.index_to_block(block_index);
    return _granule_range.index_of_containing_block(block);
  }

  void mark_granule_dirty(int granule_index) {
    _granule_is_dirty.set_bit(granule_index);
  }

  void mark_containing_granule_dirty(int block_index) {
    const int granule_index = block_index_to_granule_index(block_index);
    mark_granule_dirty(granule_index);
  }

  void ensure_committed(const_blockptr_t new_boundary) {
    DEBUG_ONLY(_granule_range.check_is_valid_block_incl(new_boundary);)
    new_boundary = MIN2(_granule_range.end(), new_boundary + num_granules_overcommit);
    commit_memory(_commit_boundary, new_boundary);
    _commit_boundary = new_boundary;
  }

public:

  ArenaSlab(
    const_blockptr_t base, size_t size,
    bool is_releasable)
    : _blockrange(base), _commit_boundary(base), _is_releasable(is_releasable)
  {
    assert(size == _size, "Range size mismatch");
  }

  const_blockptr_t base() const { return _blockrange.base(); }
  const_blockptr_t end() const { return _blockrange.end(); }

  // Allocate a number of blocks and return their addresses in the user-provided array;
  // returns the number of blocks actually allocated (which may be smaller if we cannot allocate)
  // The returned blocks are guaranteed to be ordered by address, low to high.
  int allocate_blocks(const_blockptr_t out[], int outlen) {
    BitMap::idx_t idx = 0;
    int n = 0;
    const_blockptr_t last_block = base();
    while (n < outlen) {
      idx = _blocks_in_use.find_first_clear_bit(idx, num_blocks);
      if (idx == num_blocks) {
        break;
      }
      const int block_index = checked_cast<int>(idx);
      const const_blockptr_t block = _blockrange.index_to_block(block_index);
      out[n] = block;

      _blocks_in_use.set_bit(idx);
      mark_containing_granule_dirty(block_index);

      // Since this function is only ever executed synchronized, addresses should
      // raise monotonously
      assert(last_block < block, "Order?");
      last_block = block;

      idx++;
      n++;
    }
    ensure_committed(last_block);
    return n;
  }

  // Allocate a contiguous range spanning multiple blocks. Only used for
  // huge blocks. Called rarely.
  const_blockptr_t allocate_huge_block(size_t size) {
    assert(size > 0, "num must be non-zero");
    const int num = align_up(size, szclass);

    // We only ever call this for huge blocks, not standard blocks - and there the granules
    // are exactly block sized.
    assert(szclass > ArenaBlockHelpers::max_standard_block_size, "only for huge blocks");
    assert(granulesize == szclass, "Sanity");

    // Find the lowest-address hole into which this allocation would fit
    BitMap::idx_t idx = 0;
    while (idx < num_blocks) {
      idx = _blocks_in_use.find_first_clear_bit(idx, num_blocks);
      const BitMap::idx_t idx2 = _blocks_in_use.find_first_set_bit(idx);
      const int rangelen = checked_cast<int>(idx2 - idx);
      if (rangelen >= num) { // success
        _blocks_in_use.set_range(idx, idx2);
        _granule_is_dirty.set_range(idx, idx2); // sizeof granule == sizeof block
        return _blockrange.index_to_block(checked_cast<int>(idx));
      }
    }

    return nullptr;
  }

  // Given a list of blocks, return them to the associated slab
  void deallocate_blocks(const_blockptr_t blocks[], int num) {
    for (int i = 0; i < num; i++) {
      const int block_index = _blockrange.block_to_index(blocks[i]);
      assert(_blocks_in_use.at(block_index), "double free?");
      _blocks_in_use.clear_bit(i);
    }
  }

  // huge blocks. Called rarely.
  const_blockptr_t deallocate_huge_block(const_blockptr_t block, size_t size) {
    // We only ever call this for huge blocks, not standard blocks - and there the granules
    // are exactly block sized. So we get away with treating granules as blocks and vice versa,
    // no aliasing effects.
    assert(szclass > ArenaBlockHelpers::max_standard_block_size, "only for huge blocks");
    assert(granulesize == szclass, "Sanity");

    DEBUG_ONLY(_blockrange.check_is_valid_block_(block);)
    assert(is_aligned(size, szclass), "Alignment mismatch");

    const int block_index = _blockrange.block_to_index(block);
    const int block_index2 = _blockrange.block_to_index_incl(block + size);

#ifdef ASSERT
    {
      // check that the full range of this huge block is marked as "dirty" and "in-use"
      CHeapBitMap tmp(num_blocks, mtTest, true);
      tmp.set_range(block_index, block_index2);
      CHeapBitMap tmp2(num_blocks, mtTest, true);
      _granule_is_dirty.convert_to_flat_bitmap(tmp2);
      assert(tmp2.contains(tmp), "range not fully marked as dirty");
      _blocks_in_use.convert_to_flat_bitmap(tmp2);
      assert(tmp2.contains(tmp), "range not fully marked as in-use");
    }
#endif

    // Find the lowest-address hole into which this allocation would fit
    BitMap::idx_t idx = 0;
    while (idx < num_blocks) {
      idx = _blocks_in_use.find_first_clear_bit(idx, num_blocks);
      const BitMap::idx_t idx2 = _blocks_in_use.find_first_set_bit(idx);
      const int rangelen = checked_cast<int>(idx2 - idx);
      if (rangelen >= num) { // success
        _blocks_in_use.set_range(idx, idx2);
        _granule_is_dirty.set_range(idx, idx2); // sizeof granule == sizeof block
        return _blockrange.index_to_block(checked_cast<int>(idx));
      }
    }

    return nullptr;
  }

  void contains(const_blockptr_t block) const { return _blockrange.contains(block); }

};

#endif // SHARE_MEMORY_ARENA_ARENASLAB_HPP



















