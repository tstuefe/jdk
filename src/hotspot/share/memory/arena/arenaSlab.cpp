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

#include "memory/arena/arenaBlock.hpp"
#include "memory/arena/arenaSlab.hpp"
#include "runtime/os.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"


// Returns size, in bytes, a slab would occupy in the arena heap
size_t ArenaSlab::calc_arena_space(size_t block_size, unsigned num_blocks) {
  assert(is_power_of_2(block_size), "Sanity");
  assert(is_power_of_2(num_blocks), "Sanity");
  return block_size * num_blocks;
}

// Returns size, in bytes, needed for the slab metadata
size_t ArenaSlab::calc_metadata_space(size_t block_size, size_t granule_size, unsigned num_blocks) {
  const unsigned num_granules = calc_num_granules(block_size, granule_size, num_blocks);
  assert(is_power_of_2(num_blocks), "Sanity");
  return LayeredBitmap::bytes_needed(num_blocks) +
         LayeredBitmap::bytes_needed(num_granules);
}

ArenaSlab::ArenaSlab(ArenaHeap* heap,
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

// Allocate a number of blocks and return their addresses in the user-provided array;
// returns the number of blocks actually allocated (which may be smaller if we cannot allocate)
// The returned blocks are guaranteed to be ordered by address, low to high.
unsigned ArenaSlab::allocate_blocks(const_blockptr_t out[], unsigned outlen) {
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
const_blockptr_t ArenaSlab::allocate_huge_block(size_t size) {
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
void ArenaSlab::deallocate_blocks(const_blockptr_t blocks[], unsigned num) {
  for (unsigned i = 0; i < num; i++) {
    const IndexType block_index = _block_range.block_to_index(blocks[i]);
    assert(_in_use_bitmap.at(block_index), "double free?");
    _in_use_bitmap.clear_bit(block_index);
  }
  // Update counters
  decrease_block_counter(num);
}

// huge blocks. Called rarely.
void ArenaSlab::deallocate_huge_block(const_blockptr_t block, size_t size) {
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


#ifdef ASSERT
void ArenaSlab::verify() const {
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