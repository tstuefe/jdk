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
#include "memory/arena/arenaMutex.hpp"
#include "memory/arena/arenaSlab.hpp"

#include "runtime/os.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"


#ifdef ARENA_STATS
ArenaSlab::SlabStatistics::SlabStatistics() :
  _num_standard_block_allocated(0),
  _num_standard_block_deallocated(0),
  _num_bulk_alloc_ops(0),
  _num_bulk_dealloc_ops(0),
  _num_huge_blocks_allocated(0),
  _num_huge_blocks_deallocated(0),
  _num_discard_ops(0)
{
}

void ArenaSlab::SlabStatistics::account_bulk_alloc(unsigned num) {
  _num_bulk_alloc_ops ++;
  _num_standard_block_allocated += num;
}

void ArenaSlab::SlabStatistics::account_bulk_dealloc(unsigned num) {
  _num_bulk_dealloc_ops ++;
  _num_standard_block_deallocated += num;
}

#endif

ArenaSlab::SlabHotData::SlabHotData() :
  _num_blocks_in_use(0),
  _num_blocks_in_use_max(0),
  _dirty_top(0),
#ifdef ARENA_STATS
  _stats(),
#endif
  _lock() {
}

// Helper for padded data allocation
template <class T>
static T* allocate_padded_from_metadata_heap(ArenaMetadataHeap* metadata_heap) {
  metadata_heap->align_hwm_to_padded_size(); // separate cache line
  const address p = metadata_heap->allocate(sizeof(T));
  metadata_heap->align_hwm_to_padded_size(); // separate cache line
  T* const p2 = new(p) T();
  return p2;
}

ArenaSlab::ArenaSlab(ArenaHeap* arena_heap,
                     ArenaMetadataHeap* metadata_heap,
                     const Parameters* params) :
  _arena_heap(arena_heap),
  _metadata_heap(metadata_heap),
  _block_size(params->block_size),
  _granule_size(params->granule_size),
  _num_blocks(params->num_blocks),
  _blocks_to_granule_shift(
    [params]() {
      const unsigned log2_block_size = exact_log2(params->block_size);
      const unsigned log2_granule_size = exact_log2(params->granule_size);
      assert(log2_block_size <= log2_granule_size, "Sanity");
      return log2_granule_size - log2_block_size;
    } ()),
  _num_granules(
    [&]() {
      return _num_blocks >> _blocks_to_granule_shift;
    } ()),
  _release_boundary(params->release_boundary >> _blocks_to_granule_shift), // parameter is in num blocks, we hold num granules
  _decay_factor(params->decay_factor),
  _base(arena_heap->reserve_slab_memory(_block_size, _granule_size, _num_blocks)),
  _block_range(_base, _block_size, _num_blocks),
  _granule_range(_base, _granule_size, _num_granules),
  _dirty_bitmap(metadata_heap, _num_granules),
  _in_use_bitmap(metadata_heap, _num_blocks),
  _commit_boundary(0),
  _hot(allocate_padded_from_metadata_heap<SlabHotData>(metadata_heap)),
  _per_tick(allocate_padded_from_metadata_heap<SlabPerTickData>(metadata_heap))
{
  DEBUG_ONLY(ArenaBlockHelpers::verify_valid_block_size(block_size()));
  assert(_num_blocks > 0, "Sanity");
  assert(is_aligned(_num_blocks,blocks_per_granule()), "Sanity");
  assert(is_power_of_2(_granule_size), "Sanity");
  assert(_granule_size >= _block_size, "Sanity");
  assert(_granule_size >= os::vm_page_size(), "Sanity");
  assert(is_aligned(_block_range.base(), _granule_size), "Sanity");

  // Set up release resident portion below release boundary:
  // this portion will never be released and will be, if possible, backed by
  // large pages (even if we don't use them for the burst portion of the slab).
  if (_release_boundary > 0) {
    assert(_release_boundary << _blocks_to_granule_shift == params->release_boundary,
           "Release Boundary not a common multiple of granule size");
    const const_blockptr_t release_boundary_pointer =
      _granule_range.index_to_block_incl(_release_boundary);
    assert(release_boundary_pointer <= end(), "Sanity");
    const bool resident_uses_large_pages = is_aligned(release_boundary_pointer, ArenaHeapBase::large_page_size);
    _arena_heap->commit_memory(base(), release_boundary_pointer);
    _arena_heap->advise_large_page_use(base(), release_boundary_pointer, resident_uses_large_pages);
    _commit_boundary = _release_boundary;
  }
}

// Creates an unusable slot.
ArenaSlab::ArenaSlab() :
  _arena_heap(nullptr),
  _metadata_heap(nullptr),
  _block_size(0),
  _granule_size(0),
  _num_blocks(0),
  _blocks_to_granule_shift(0),
  _num_granules(0),
  _release_boundary(0),
  _decay_factor(0),
  _base(nullptr),
  _block_range(),
  _granule_range(),
  _dirty_bitmap(),
  _in_use_bitmap(),
  _commit_boundary(0),
  _hot(nullptr),
  _per_tick(nullptr) {
}

// Ensure this and all lower-address blocks are committed.
void ArenaSlab::ensure_committed(IndexType block_index) {
  DEBUG_ONLY(_block_range.verify_is_valid_block_index(block_index);)
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
    _arena_heap->advise_large_page_use(p1, p2, use_large_pages());
#endif
    _commit_boundary = new_commit_boundary_overshoot;
  }
}

// Allocate a number of blocks and return their addresses in the user-provided array;
// returns the number of blocks actually allocated (which may be smaller if we cannot allocate)
// The returned blocks are guaranteed to be ordered by address, low to high.
unsigned ArenaSlab::allocate_blocks(const_blockptr_t out[], unsigned outlen) {

  ArenaMutexLocker locker(_hot->_lock);

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
    mark_granule_dirty(granule_idx);

    assert(n == 0 || highest_block_idx < idx, "Should raise monotonously");
    highest_block_idx = MAX2(idx, highest_block_idx);

    const const_blockptr_t block = _block_range.index_to_block(idx);
    out[n] = block;
    idx++;
    n++;
  }
  if (n > 0) {
    // Make sure the returned memory is committed
    ensure_committed(highest_block_idx);
    // Bookkeeping
    increase_block_counter(n);
#ifdef ARENA_STATS
    _hot->_stats.account_bulk_alloc(n);
#endif
  }

  return n;
}

// Allocate a contiguous range spanning multiple blocks. Only used for
// huge blocks. Called rarely.
const_blockptr_t ArenaSlab::allocate_huge_block(size_t size) {

  ArenaMutexLocker locker(_hot->_lock);

  assert(size > 0, "num must be non-zero");
  DEBUG_ONLY(ArenaBlockHelpers::verify_valid_huge_size(size));

  const size_t size_aligned = align_up(size, block_size());
  const unsigned num = size_aligned / block_size();

  // We only ever call this for huge blocks, not standard blocks - and there the granules
  // are exactly block sized.
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
        mark_granule_range_dirty(idx, idx + num); // huge blocks : granule == block size

        // Move commit boundary up
        const IndexType highest_block_idx = idx + num - 1;
        ensure_committed(highest_block_idx);

        // Bookkeeping
        increase_block_counter(num);
#ifdef ARENA_STATS
        _hot->_stats._num_huge_blocks_allocated++;
#endif

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

  ArenaMutexLocker locker(_hot->_lock);

  for (unsigned i = 0; i < num; i++) {
    const IndexType block_index = _block_range.block_to_index(blocks[i]);
    assert(_in_use_bitmap.at(block_index), "double free?");
    _in_use_bitmap.clear_bit(block_index);
  }
  // Bookkeeping
  decrease_block_counter(num);
#ifdef ARENA_STATS
  _hot->_stats.account_bulk_dealloc(num);
#endif
}

// huge blocks. Called rarely.
void ArenaSlab::deallocate_huge_block(const_blockptr_t block, size_t size) {

  ArenaMutexLocker locker(_hot->_lock);

  assert(size > 0, "num must be non-zero");
  DEBUG_ONLY(ArenaBlockHelpers::verify_valid_huge_size(size));

  // We only ever call this for huge blocks, not standard blocks - and there the granules
  // are exactly block sized. So we get away with treating granules as blocks and vice versa.
  assert(_granule_size == block_size(), "Sanity");

  DEBUG_ONLY(_block_range.verify_is_valid_block(block);)
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

  // Bookkeeping
  decrease_block_counter(idx2 - idx);
#ifdef ARENA_STATS
  _hot->_stats._num_huge_blocks_deallocated ++;
#endif
}

void ArenaSlab::mark_granule_dirty(IndexType granule_idx) {
  _dirty_bitmap.set_bit(granule_idx);
  if (_hot->_dirty_top < (granule_idx + 1)) {
    _hot->_dirty_top = checked_cast<unsigned>(granule_idx + 1);
  }
}

void ArenaSlab::mark_granule_range_dirty(IndexType granule_begidx, IndexType granule_endidx) {
  _dirty_bitmap.set_range(granule_begidx, granule_endidx);
  if (_hot->_dirty_top < granule_endidx) {
    _hot->_dirty_top = checked_cast<unsigned>(granule_endidx);
  }
}

void ArenaSlab::increase_block_counter(unsigned num) {
  _hot->_num_blocks_in_use += num;
  assert(_hot->_num_blocks_in_use <= num_blocks(), "Sanity");
  _hot->_num_blocks_in_use_max = MAX2(_hot->_num_blocks_in_use, _hot->_num_blocks_in_use_max);
}

void ArenaSlab::decrease_block_counter(unsigned num) {
  assert(_hot->_num_blocks_in_use >= num, "Sanity");
  _hot->_num_blocks_in_use -= num;
}

void ArenaSlab::release_dirty_granules(bool squeeze) {
  /* work in progress
  // Release mechanics:
  //
  // - We compute a retain target, in number of blocks, based on the peak in-use blocks
  //   since the last tick (HWM), the last tick's retain target RT(-1), and a decay
  //   factor DF (e.g. 0.75):
  //     RTnow = max ( HWM, RT-1 * DF ).
  //   This means:
  //     - peeks stay for one tick
  //     - after that, we decay geometrically
  // - We scan from the top (highest address first) for ranges of dirty granules that don't contain in-use blocks
  //   and are therefore in the release range candidate set
  // - We calculate a minimum range size "its-worth-the-mmap-call" size from the granule size. That minimum
  //   range is zero for squeeze releases, which occur every n ticks.
  // - If a range is larger than the minimum range size, we release it.
  // - We stop releasing memory at the release boundary (we never release the resident portion of a slab)
  // - We stop releasing memory if the retain target is reached

  // If we have no burst section (the whole slab is resident), there is nothing to release
  if (_release_boundary == num_granules()) {
    return;
  }

  const unsigned rt_last = _per_tick->_retain_target;
  const unsigned rt_now = MAX2((unsigned)(rt_last * _decay_factor), _hot->_num_blocks_in_use_max);

  const unsigned num_dirty_blocks = _dirty_bitmap.count_one_bits() << _blocks_to_granule_shift;
  const unsigned max_blocks_to_release = num_dirty_blocks - rt_now;

  // We don't release ranges smaller than this unless its a squeeze release
  const size_t squeeze_threshold = squeeze ? 0 : 256 * K;

  const IndexType granule_idx_hi = _hot->_dirty_top;
  const IndexType granule_idx_lo = _release_boundary;
  const IndexType block_idx_hi = granule_index_to_first_block_index_incl(granule_idx_hi);
  const IndexType block_idx_lo = granule_index_to_first_block_index_incl(_release_boundary);

  const LayeredBitmap& dirty_bitmap = _dirty_bitmap;
  const LayeredBitmap& in_use_bitmap = _in_use_bitmap;

  int blocks_to_release = checked_cast<int>(max_blocks_to_release);

  IndexType granule_idx = granule_idx_hi;
  while (granule_idx > granule_idx_lo && blocks_to_release > 0) {
    IndexType tmp = 0, tmp2 = 0;

    // Find range of dirty granules
    tmp = _dirty_bitmap.find_last_set_bit(granule_idx_lo, granule_idx);
    if (granule_idx == tmp) {
      // No dirty granules
      break;
    }
    tmp2 = _dirty_bitmap.find_last_clear_bit(granule_idx_lo, tmp);
    if (tmp2 == tmp) {
      tmp2 = granule_idx_lo;
    }
    const IndexType granule_idx_dirty_range_end = tmp + 1;
    const IndexType granule_idx_dirty_range_beg = tmp2;
    assert(granule_idx_dirty_range_beg < granule_idx_dirty_range_end, "Sanity");

    // Calculate the next iteration step start already, so we can continue wherever we want in this loop
    granule_idx = granule_idx_dirty_range_beg - 1;

    // For this range of dirty granule, find the last range of free blocks
    const IndexType block_idx_dirty_range_end = granule_index_to_first_block_index_incl(granule_idx_dirty_range_end);
    const IndexType block_idx_dirty_range_beg = granule_index_to_first_block_index(granule_idx_dirty_range_beg);
    tmp = _in_use_bitmap.find_last_clear_bit(block_idx_dirty_range_beg, block_idx_dirty_range_end);
    if (tmp == block_idx_dirty_range_end) {
      // This granule range has no free blocks
      continue;
    }
    tmp2 = _in_use_bitmap.find_last_set_bit(block_idx_dirty_range_beg, tmp);
    if (tmp2 == tmp) {
      tmp2 = block_idx_dirty_range_beg;
    }
    const IndexType block_idx_free_range_end = tmp + 1;
    const IndexType block_idx_free_range_beg = tmp;
    assert(block_idx_free_range_end > block_idx_free_range_beg, "Sanity");

    // Found a potential discardable block range.
    // Convert to a range of discardable granules: Only fully free and dirty granules can be released, and don't release
    // more than needed to meet the retain target.

    const IndexType granule_idx_discardable_end = block_index_to_granule_index_incl(block_idx_free_range_end);
    const IndexType granule_idx_discardable_beg = align_up(block_index_to_granule_index(block_idx_free_range_beg), blocks_per_granule());


    if (granule_idx_discardable_end <= granule_idx_discardable_beg) {
      continue; // Nope.
    }
    const size_t discardable_bytes =
      (granule_idx_discardable_end - granule_idx_discardable_beg) * granule_size();
    if (squeeze_threshold > discardable_bytes) {
      continue; // Not worth it.
    }

    // Cap
    const IndexType granule_idx_dirty_range_end = tmp + 1;
    const IndexType granule_idx_dirty_range_beg = tmp2;

    const IndexType blocks_idx_free_end = _in_use_bitmap.find_last_clear_bit()

    // Now find the lowest granule in that range that has in-use blocks
    //
    //
    //
    // Now iterate all runs of free blocks inside that section of dirty granules, bypassing in-use blocks
    IndexType block_idx = block_idx_dirty_range_end;
    while (block_idx > block_idx_dirty_range_beg) {
      tmp = _in_use_bitmap.find_last_clear_bit(block_idx_dirty_range_beg, block_idx);
      if (granule_idx == tmp) {
        break; // No more free blocks in that dirty granule section
      }
      tmp2 = _in_use_bitmap.find_last_set_bit(block_idx_dirty_range_beg, tmp);
      if (tmp2 == tmp) {
        tmp2 = block_idx_dirty_range_beg;
      }
      // Calculate range of blocks to release.
      const IndexType discard_block_range_end = tmp + 1;
      const IndexType discard_block_range_start = tmp2;
      const IndexType discard_granule_range_end = block_index_to_granule_index(discard_block_range_end);
      const IndexType discard_granule_range_start = align_up(block_index_to_granule_index(discard_block_range_start), blocks_per_granule());


      const
      if (((granule_idx_discard_end - granule_idx_discard_start) * granule_size()) > squeeze_threshold) {
        release_section(granule_idx_discard_start, granule_idx_discard_end);
      }

    }
    // Now iterate all 0-runs of in-use blocks in that range


    const IndexType granule_dirty_range_end = g_end + 1;
  }*/
}


void ArenaSlab::tick() {
  // Todo
}

void ArenaSlab::print_on(outputStream* st) const {
  st->print_cr("ArenaSlab %zu", block_size());
  StreamIndentor indentor(st);
#define PRINT_HERE(fmt, ...) { st->print_cr(fmt, __VA_ARGS__); }

  // Geometry:
  PRINT_HERE("block_size: %zu bytes", block_size());
  PRINT_HERE("granule_size: %zu (%u blocks)", granule_size(), blocks_per_granule());
  PRINT_HERE("max blocks: %u ", num_blocks());
  PRINT_HERE("release boundary: %u granules (%u blocks, %zu bytes)",
    checked_cast<unsigned>(_release_boundary),
    checked_cast<unsigned>(_release_boundary) * blocks_per_granule(),
    _release_boundary * _granule_size
    );
  const unsigned max_blocks_in_resident =
    checked_cast<unsigned>(granule_index_to_first_block_index_incl(_release_boundary));
  const unsigned max_blocks_in_burst =
    num_blocks() - max_blocks_in_resident;
  PRINT_HERE("max blocks in resident: %u ", max_blocks_in_resident);
  PRINT_HERE("max blocks in burst: %u ", max_blocks_in_burst);

  // In use statistics
  PRINT_HERE("in-use blocks: %u ", count_in_use_blocks());
  PRINT_HERE("blocks in use (historical peak): %u ", _hot->_num_blocks_in_use_max);

  // To get a glimpse at the decreasing density we expect:
  if (max_blocks_in_resident > 0) {
    PRINT_HERE("in-use blocks in resident section: %u",
      checked_cast<unsigned>(_in_use_bitmap.count_one_bits(0, max_blocks_in_resident))
    );
  }

  // Print in-use blocks in burst section (possibly also by quartile)
  if (max_blocks_in_burst > 0) {
    const unsigned in_burst =
      checked_cast<unsigned>(_in_use_bitmap.count_one_bits(max_blocks_in_resident, num_blocks()));
    PRINT_HERE("in-use blocks in burst: %u", in_burst);

    if (max_blocks_in_burst > 4) {
      const unsigned q = max_blocks_in_burst / 4;
      unsigned count[4];
      for (int i = 0; i < 4; i++) {
        const unsigned end_idx =
          (i == 3) ?
          num_blocks() :  // prevent rounding error on last quartile
          max_blocks_in_resident + (i + 1) * q;
        count[i] = checked_cast<unsigned>(
          _in_use_bitmap.count_one_bits(max_blocks_in_resident + i * q, end_idx));
      }
      PRINT_HERE("in-use blocks in burst per-quartile distribution: %u/%u/%u/%u",
        count[0], count[1], count[2], count[3]);
    }
  }

  // Dirty statistics
  const unsigned dirty_granules = count_dirty_granules();
  const unsigned dirty_blocks = count_dirty_blocks();
  PRINT_HERE("Dirty granules: %u (%u blocks, %zu bytes)",
    dirty_granules, dirty_blocks, dirty_granules * granule_size());

#ifdef ARENA_STATS
#define PRINT_COUNTER(desc, name) { PRINT_HERE("%s: " UINT64_FORMAT, desc, name); }
  PRINT_COUNTER("Standard blocks allocated", _hot->_stats._num_standard_block_allocated);
  PRINT_COUNTER("Bulk allocs", _hot->_stats._num_bulk_alloc_ops);
  st->print_cr("Blocks per bulk alloc, avg: %.2f",
    _hot->_stats._num_bulk_alloc_ops > 0 ?
      (double)_hot->_stats._num_standard_block_allocated /
      (double)_hot->_stats._num_bulk_alloc_ops : 0.0);
  PRINT_COUNTER("Standard blocks deallocated", _hot->_stats._num_standard_block_deallocated);
  PRINT_COUNTER("Bulk deallocs", _hot->_stats._num_bulk_dealloc_ops);
  st->print_cr("Blocks per bulk dealloc, avg: %.2f",
    _hot->_stats._num_bulk_dealloc_ops > 0 ?
      (double)_hot->_stats._num_standard_block_deallocated /
      (double)_hot->_stats._num_bulk_dealloc_ops : 0.0);
  PRINT_COUNTER("Huge blocks allocated", _hot->_stats._num_huge_blocks_allocated);
  PRINT_COUNTER("Huge blocks deallocated", _hot->_stats._num_huge_blocks_deallocated);
  PRINT_COUNTER("Num discard ops", _hot->_stats._num_discard_ops);
#undef PRINT_COUNTER
#endif
#undef PRINT_HERE
}

#ifdef ASSERT
void ArenaSlab::verify_block(const_blockptr_t block) const {
  assert(contains(block), "Sanity");
  assert(is_aligned(block, block_size()), "Sanity");
}

void ArenaSlab::verify() const {

  ArenaMutexLocker locker(_hot->_lock);

  assert(count_dirty_blocks() >= count_in_use_blocks(), "Sanity");
  assert(is_aligned(count_dirty_blocks(), blocks_per_granule()), "Sanity");
  _in_use_bitmap.verify();
  assert(_in_use_bitmap.count_one_bits() == _hot->_num_blocks_in_use, "Sanity");
  assert(_hot->_num_blocks_in_use_max >= _hot->_num_blocks_in_use, "Sanity");
  _dirty_bitmap.verify();

  const IndexType highest_dirty_granule = _dirty_bitmap.find_last_set_bit(0, num_granules());
  const unsigned expected_dirty_top =
    highest_dirty_granule < num_granules() ? checked_cast<unsigned>(highest_dirty_granule) + 1 : 0u;
  assert(expected_dirty_top == _hot->_dirty_top, "Sanity");

  // every in-use block must be dirty
  CHeapBitMap tmp(num_blocks(), mtTest, false);
  CHeapBitMap tmp2(num_blocks(), mtTest, false);
  _dirty_bitmap.convert_to_flat_bitmap_scaled(tmp, blocks_per_granule());
  _in_use_bitmap.convert_to_flat_bitmap(tmp2);
  assert(tmp.contains(tmp2), "Sanity");

  // Commit|Release Boundaries
  assert(_granule_range.is_valid_block_index_incl(_commit_boundary), "Sanity");
  assert(_granule_range.is_valid_block_index_incl(_release_boundary), "Sanity");

  // Below the release boundary, all blocks must be committed
  assert(_release_boundary <= _commit_boundary, "Commit boundary too high?");

  // the commit boundary must be equal or higher than the end of the highest dirty block
  const IndexType granule_idx = _dirty_bitmap.find_last_set_bit(0, num_granules());
  if (granule_idx < num_granules()) {
    assert(granule_idx < _commit_boundary, "Commit boundary too low?");
  }

}
#endif

// This table holds the geometries of the standard slabs hotspot uses.

static constexpr ArenaSlab::Parameters global_slab_params[ArenaBlockHelpers::num_size_classes] = {

  // Todo. Instead of literals, use the constants from ArenaBlockHelpers

  // blocksize    granulesize   numblocks   release_boundary    decay_factor

  // 256 byte blocks
  // We keep all of them inside a single (large) page that is fully resident.
  // (Todo: this could actually share a single large page with the next slab, 1K)
  // (      which would require ArenaHeap to be able to recognise neighboring fully resident
  //        slabs - would save 2MB. Really Worth it?? )
  { 256, 2*M, 8192 /* 2M */,  8192,  0.0f },

  // 1K blocks
  // We keep all of them inside a single (large) page that is fully resident.
  // (Todo: this could actually share a single large page with the 256 byte slab)
{ K, 2*M, 2048 /* 2M */,  2048,  0.0f },

  // 32K blocks
  // This block size sees the highest allocation churn and the highest peak live and average live number.
  // Number of blocks: 32768 - enough to absorb 99.9% of all peaks
  // Release Boundary: 384   - Dacapo benchmarks (default datasize) show an avg peak live of 50, with tomcat
  //                           and spring in the 300s; 384 absorbs most of those scenario at a cost of 12MB.
  // Note that this scales with number of mutators (resource area chunks). The numbers are fine to absorb
  // a standard app's needs with resident and 99.9% of all apps needs with burst section; beyond that we
  // will use C-heap.
{ 32*K, 64*K, 32768 /* 1GB */,  384 /* 12 MB */,  0.0f },

  // 64K blocks: semi important
{ 64*K, 64*K, 1024 /* 64MB */ ,  8 /* 512 KB */,  0.0f },

  // From here on, we have very little average live blocks. Large block sizes get increasingly
  // uncommon with size. In all of these cases, granulesize == blocksize since reducing footprint becomes
  // more important than allocation speed. And we don't have a resident part, since we want those blocks to be
  // released to the OS completely, not cached.
{ 128*K, 128*K, 1024 /* 128 MB */,  0 /* no resident section */,  0.0f },
{ 256*K, 256*K, 1024 /* 256 MB */,  0 /* no resident section */,  0.0f },
{ 512*K, 512*K, 512  /* 256 MB */,  0 /* no resident section */,  0.0f },
{ 1*M, 1*M, 256      /* 256 MB */,  0 /* no resident section */,  0.0f },

  // 2MB blocks really are huge blocks (consisting of sequences of neighboring 2MB blocks). Keep the total size of this
  // slab large enough to absorb most of the abnormal allocations a compiler may do.
{ 2*M, 2*M, ArenaBlockHelpers::huge_block_max_size / (2 * M) /* 2GB = 1024 */,  0 /* no resident section */,  0.0f }

};


/// ArenaSlabGroup

ArenaSlabGroup::ArenaSlabGroup(ArenaHeap* arena_heap,
                               ArenaMetadataHeap* metadata_heap,
                               const ArenaSlab::Parameters params[num_slots]) :
  _slabs {
  { arena_heap, metadata_heap, &params[ArenaBlockHelpers::szclass_256] },
  { arena_heap, metadata_heap, &params[ArenaBlockHelpers::szclass_1K] },
 { arena_heap, metadata_heap, &params[ArenaBlockHelpers::szclass_32K] },
 { arena_heap, metadata_heap, &params[ArenaBlockHelpers::szclass_64K] },
 { arena_heap, metadata_heap, &params[ArenaBlockHelpers::szclass_128K] },
 { arena_heap, metadata_heap, &params[ArenaBlockHelpers::szclass_256K] },
 { arena_heap, metadata_heap, &params[ArenaBlockHelpers::szclass_512K] },
 { arena_heap, metadata_heap, &params[ArenaBlockHelpers::szclass_1M] },
 { arena_heap, metadata_heap, &params[ArenaBlockHelpers::szclass_2M] },
  }
{
}

// Creates a slab group with the default geometries for hotspot
ArenaSlabGroup::ArenaSlabGroup(ArenaHeap* arena_heap,
                               ArenaMetadataHeap* metadata_heap) :
  ArenaSlabGroup(arena_heap, metadata_heap, global_slab_params)
{
}

void ArenaSlabGroup::tick() {
  for (unsigned i = 0; i < num_slots; i++) {
    _slabs[i].tick();
  }
}

void ArenaSlabGroup::print_on(outputStream* st) const {
  st->print_cr("ArenaSlabGroup");
  StreamIndentor indentor(st);
  for (unsigned i = 0; i < num_slots; i++) {
    _slabs[i].print_on(st);
  }
}

#ifdef ASSERT
void ArenaSlabGroup::verify() const {
  for (unsigned i = 0; i < num_slots; i++) {
    _slabs[i].verify();
  }
}
#endif