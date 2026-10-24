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


template <size_t szclass,
          int num_blocks,
          size_t granulesize,
          size_t pagesize,
          bool releaseable>
class ArenaSlab {

  STATIC_ASSERT(is_power_of_2(szclass));
  STATIC_ASSERT(is_power_of_2(granulesize));
  STATIC_ASSERT(is_power_of_2(pagesize));
  STATIC_ASSERT(is_power_of_2(num_blocks));
  STATIC_ASSERT(granulesize >= pagesize);
  STATIC_ASSERT(granulesize >= szclass);

  const BlockRange<szclass> _blockrange;
  static constexpr size_t _size = szclass * num_blocks;
  static constexpr int _num_granules = _size/granulesize;

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
    const BlockRange<granulesize> granule_range(_blockrange.base());
  }

public:

  ArenaSlab(const_blockptr_t base, size_t size)
    : _blockrange(base), _commit_boundary(base)
  {
  }

  // Allocate a number of blocks and return their addresses in the user-provided array;
  // returns the number of blocks actually allocated (which may be smaller if we cannot allocate)
  // The returned blocks are guaranteed to be ordered by address, low to high.
  int allocate_blocks(const_blockptr_t out[], int outlen) {
    BitMap::idx_t idx = 0;
    int n = 0;
    while (n < outlen) {
      idx = _blocks_in_use.find_first_clear_bit(idx, num_blocks);
      if (idx == num_blocks) {
        break;
      }
      out[n] = _blockrange.block_to_index(checked_cast<int>(idx));
      _blocks_in_use.set_bit(idx);
      idx++;
      n++;
    }
    return n;
  }

  // Given a list of blocks, return them to the associated slab
  void deallocate_blocks(const_blockptr_t blocks[], int num) {
    for (int i = 0; i < num; i++) {
      _blocks_in_use.clear_bit(i);
    }
  }

  void contains(const_blockptr_t block) const { return _blockrange.contains(block); }

};

#endif // SHARE_MEMORY_ARENA_ARENASLAB_HPP



















