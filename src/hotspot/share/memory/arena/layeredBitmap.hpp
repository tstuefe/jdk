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

#ifndef SHARE_MEMORY_ARENA_LAYERED_BITMAP_HPP
#define SHARE_MEMORY_ARENA_LAYERED_BITMAP_HPP

#include "utilities/bitMap.inline.hpp"
#include "utilities/debug.hpp"

template <BitMap::idx_t num_summary_bits, BitMap::idx_t num_segment_bits>
class LayeredBitmap {
public:
  typedef BitMap::bm_word_t WordType;
  typedef BitMap::idx_t IndexType;

  static constexpr IndexType num_bits = num_summary_bits * num_segment_bits;
  static constexpr IndexType num_segments = num_summary_bits;
  static constexpr size_t num_words = BitMap::calc_size_in_words(num_bits) * BytesPerWord;
  static constexpr size_t num_summary_words = BitMap::calc_size_in_words(num_summary_bits) * BytesPerWord;
  static constexpr size_t num_segment_words = BitMap::calc_size_in_words(num_segment_bits) * BytesPerWord;

  STATIC_ASSERT(num_summary_bits >= 64);
  STATIC_ASSERT(is_power_of_2(num_summary_bits));
  STATIC_ASSERT(num_segment_bits >= 64);
  STATIC_ASSERT(is_power_of_2(num_segment_bits));

#ifdef ASSERT
  static void verify_segment_index_excl(IndexType segidx) {
    assert(segidx < num_segments, "out of bounds (segidx %zu, num_segments %zu)",
      segidx, num_segments);
  }
  static void verify_segment_index_incl(IndexType segidx) {
    assert(segidx <= num_segments, "out of bounds (segidx %zu, num_segments %zu)",
      segidx, num_segments);
  }
  static void verify_index_excl(IndexType idx) {
    assert(idx < num_bits, "out of bounds (idx %zu, num_bits %zu)",
      idx, num_bits);
  }
  static void verify_index_incl(IndexType idx) {
    assert(idx <= num_bits, "out of bounds (idx %zu, num_bits %zu)",
      idx, num_bits);
  }
  static void verify_index_range_incl(IndexType begidx, IndexType endidx) {
    assert(begidx <= endidx, "Sanity");
    verify_index_incl(begidx);
    verify_index_incl(endidx);
  }
  static void verify_index_range_excl(IndexType begidx, IndexType endidx) {
    assert(begidx <= endidx, "Sanity");
    verify_index_excl(begidx);
    verify_index_excl(endidx);
  }
#endif // ASSERT

private:

  WordType _bitmap_storage[num_words];
  WordType _full_segments_bitmap_storage[num_summary_words];
  WordType _nonempty_segments_bitmap_storage[num_summary_words];

  BitMapView _bitmap;
  BitMapView _full_segments_bitmap;
  BitMapView _nonempty_segments_bitmap;

  // Given a bit index, return the index of enclosing segment
  // Passing num_bits is allowed and returns num_summary_bits
  static IndexType segment_index_by_index(IndexType idx) {
    verify_index_incl(idx);
    const IndexType segidx = idx / num_segment_bits;
    verify_index_excl(segidx);
    return segidx;
  }

  // Given a segment index, return idx of first bit.
  // Passing num_segments is allowed and returns num_bits
  static IndexType index_by_segment_index(IndexType segidx) {
    verify_segment_index_incl(segidx);
    const IndexType idx = segidx * num_segment_bits;
    verify_index_incl(idx);
    return idx;
  }

  // Given a bit idx, return bit idx of first bit in the same segment
  // Passing num_bits is allowed and returns num_bits
  static IndexType index_align_down_segment_start(IndexType idx) {
    verify_index_incl(idx);
    return align_down(idx, num_segment_bits);
  }

  // Given a bit idx, return bit idx of first bit in the next segment
  // Passing num_bits is allowed and returns num_bits
  static IndexType index_align_up_segment_start(IndexType idx) {
    verify_index_incl(idx);
    return align_up(idx, num_segment_bits);
  }

  // Given a segment number, return true if all bits in that segment are set.
  // Checks only the master bitmap, not the summary bitmap.
  bool master_bitmap_segment_full(IndexType segidx) const {
    verify_segment_index_excl(segidx);
    const IndexType begidx = index_by_segment_index(segidx);
    const IndexType endidx = index_by_segment_index(segidx + 1);
    return _bitmap.find_first_clear_bit(begidx, endidx) == endidx;
  }

  // Given a segment number, return true if at least one bit in that segment is cleared.
  // Checks only the master bitmap, not the summary bitmap.
  bool master_bitmap_segment_notfull(IndexType segidx) const {
    return !master_bitmap_segment_full(segidx);
  }

  // Given a segment number, return true if no bit in that segment is set.
  // Checks only the master bitmap, not the summary bitmap.
  bool master_bitmap_segment_empty(IndexType segidx) const {
    verify_segment_index_excl(segidx);
    const IndexType begidx = index_by_segment_index(segidx);
    const IndexType endidx = index_by_segment_index(segidx + 1);
    return _bitmap.find_first_set_bit(begidx, endidx) == endidx;
  }

  // Given a segment number, return true if at least one bit in that segment is set.
  // Checks only the master bitmap, not the summary bitmap.
  bool master_bitmap_segment_notempty(IndexType segidx) const {
    return !master_bitmap_segment_empty(segidx);
  }

public:

  LayeredBitmap(bool do_clear = false)
  : _bitmap(_bitmap_storage, num_bits),
    _full_segments_bitmap(_full_segments_bitmap_storage, num_summary_bits),
    _nonempty_segments_bitmap(_nonempty_segments_bitmap_storage, num_summary_bits)
  {
    if (do_clear) {
      memset(_bitmap_storage, 0, num_words * BytesPerWord);
      memset(_full_segments_bitmap_storage, 0, num_summary_words * BytesPerWord);
      memset(_nonempty_segments_bitmap_storage, 0, num_summary_words * BytesPerWord);
    }
  }

  void set_bit(IndexType idx) {
    assert(idx < num_bits, "Sanity");

    if (_bitmap.at(idx)) {
      return; // nothing changes
    }
    _bitmap.set_bit(idx);

    // Update summary bitmaps
    const IndexType segidx = segment_index_by_index(idx);

    if (!_nonempty_segments_bitmap.at(segidx)) { // avoid unnecessary writes
      _nonempty_segments_bitmap.set_bit(segidx);
    }

    assert(!_full_segments_bitmap.at(segidx), "Cannot have been full before.");
    if (master_bitmap_segment_full(segidx)) {
      _full_segments_bitmap.set_bit(segidx);
    }
  }

  void clear_bit(IndexType idx) {
    assert(idx < num_bits, "Sanity");

    if (!_bitmap.at(idx)) {
      return; // nothing changes
    }
    _bitmap.clear_bit(idx);

    // Update summary bitmaps
    const IndexType segidx = segment_index_by_index(idx);

    assert(_nonempty_segments_bitmap.at(segidx), "Cannot have been empty before");
    if (master_bitmap_segment_empty(segidx)) {
      _nonempty_segments_bitmap.clear_bit(segidx);
    }

    if (_full_segments_bitmap.at(segidx)) { // avoid unnecessary writes
      _full_segments_bitmap.clear_bit(segidx);
    }
  }

  // X=true: find first 1 bit
  // X=false: find first 0 bit
  template <bool X>
  IndexType find_first_X_bit(IndexType begidx, IndexType endidx) const {
    assert(begidx <= endidx, "Sanity");
    if (begidx == endidx) {
      return endidx;
    }
    // look up in summary first
    const IndexType segidx_beg = index_by_segment_index(begidx);
    const IndexType segidx_end = index_by_segment_index(endidx);
    const IndexType segidx = X ?
      _full_segments_bitmap.find_first_set_bit(segidx_beg, segidx_end) :
      _nonempty_segments_bitmap.find_first_clear_bit(segidx_beg, segidx_end);
    if (segidx == segidx_end) {
      return endidx;
    }
    // then in master bitmap
    const IndexType begidx_segment = index_by_segment_index(segidx);
    const IndexType endidx_segment = begidx_segment + num_segment_bits;
    const IndexType result = X ?
      _bitmap.find_first_set_bit(begidx_segment, endidx_segment) :
      _bitmap.find_first_clear_bit(begidx_segment, endidx_segment);
    assert(result == num_bits || at(result) == X, "Sanity");
    return result;
  }

  IndexType find_first_set_bit(IndexType beg, IndexType end) const {
    return find_first_X_bit<true>(beg, end);
  }

  IndexType find_first_clear_bit(IndexType beg, IndexType end) const {
    return find_first_X_bit<false>(beg, end);
  }

  // Sets bit range [begidx, endidx)
  void set_range(IndexType begidx, IndexType endidx) {
    verify_index_range_incl(begidx, endidx);

    const IndexType num = endidx - begidx;
    if (num == 0) {
      return; // nothing changes
    }
    if (_bitmap.find_first_clear_bit(begidx, endidx) == endidx) {
      return; // nothing changes
    }

    // set in master bitmap
    _bitmap.set_range(begidx, endidx);

    // find affected segment range
    IndexType begidx_aligned = index_align_down_segment_start(begidx);
    IndexType endidx_aligned = index_align_up_segment_start(endidx);

    // update nonempty bitmap
    _nonempty_segments_bitmap.set_range(
      segment_index_by_index(begidx_aligned),
      segment_index_by_index(endidx_aligned));

    // update full bitmap
    if (begidx_aligned < begidx) {
      if (_bitmap.find_first_clear_bit(begidx_aligned, begidx) != begidx) {
        begidx_aligned += num_segment_bits;
      }
    }
    if (endidx_aligned > endidx) {
      if (_bitmap.find_first_clear_bit(endidx, endidx_aligned) != endidx_aligned) {
        endidx_aligned -= num_segment_bits;
      }
    }
    if (begidx_aligned < endidx_aligned) {
      _full_segments_bitmap.set_range(
        segment_index_by_index(begidx_aligned),
        segment_index_by_index(endidx_aligned));
    }
  }

  // Clear bit range [begidx, endidx)
  void clear_range(IndexType begidx, IndexType endidx) {
    verify_index_range_incl(begidx, endidx);

    const IndexType num = endidx - begidx;
    if (num == 0) {
      return; // nothing changes
    }
    if (_bitmap.find_first_set_bit(begidx, endidx) == endidx) {
      return; // nothing changes
    }

    // set in master bitmap
    _bitmap.clear_range(begidx, endidx);

    // find affected segment range
    IndexType begidx_aligned = index_align_down_segment_start(begidx);
    IndexType endidx_aligned = index_align_up_segment_start(endidx);

    // update nonempty bitmap
    _full_segments_bitmap.clear_range(
      segment_index_by_index(begidx_aligned),
      segment_index_by_index(endidx_aligned));

    // update full bitmap
    if (begidx_aligned < begidx) {
      if (_bitmap.find_first_set_bit(begidx_aligned, begidx) != begidx) {
        begidx_aligned += num_segment_bits;
      }
    }
    if (endidx_aligned > endidx) {
      if (_bitmap.find_first_set_bit(endidx, endidx_aligned) != endidx_aligned) {
        endidx_aligned -= num_segment_bits;
      }
    }
    if (begidx_aligned < endidx_aligned) {
      _nonempty_segments_bitmap.clear_range(
        segment_index_by_index(begidx_aligned),
        segment_index_by_index(endidx_aligned));
    }
  }

  bool at(IndexType idx) const {
    return _bitmap.at(idx);
  }

  uintx count_one_bits(IndexType begidx, IndexType endidx) const {
    return _bitmap.count_one_bits(begidx, endidx);
  }

  uintx count_one_bits() const {
    return _bitmap.count_one_bits(0, num_bits);
  }

  bool is_empty() const {
    return _nonempty_segments_bitmap.is_empty();
  }

  bool is_full() const {
    return _full_segments_bitmap.is_full();
  }

#ifdef ASSERT
  void verify() const {
    for (IndexType segidx = 0; segidx < num_segments; segidx++) {
      verify_segment_index_excl(segidx);
      const IndexType begidx_segment = index_by_segment_index(segidx);
      verify_index_excl(begidx_segment);
      const IndexType popcount = _bitmap.count_one_bits(begidx_segment, begidx_segment + num_segment_bits);
      if (popcount == 0) {
        assert(!master_bitmap_segment_full(segidx), "Sanity");
        assert(!master_bitmap_segment_notempty(segidx), "Sanity");
        assert(!_full_segments_bitmap.at(segidx), "Sanity");
        assert(!_nonempty_segments_bitmap.at(segidx), "Sanity");
      } else if (popcount == num_segment_bits) {
        assert(master_bitmap_segment_full(segidx), "Sanity");
        assert(master_bitmap_segment_notempty(segidx), "Sanity");
        assert(_full_segments_bitmap.at(segidx), "Sanity");
        assert(_nonempty_segments_bitmap.at(segidx), "Sanity");
      } else {
        assert(popcount < num_summary_bits, "Sanity");
        assert(!master_bitmap_segment_full(segidx), "Sanity");
        assert(master_bitmap_segment_notempty(segidx), "Sanity");
        assert(!_full_segments_bitmap.at(segidx), "Sanity");
        assert(_nonempty_segments_bitmap.at(segidx), "Sanity");
      }
    }
  }
#endif // ASSERT

};

#endif // SHARE_MEMORY_ARENA_LAYERED_BITMAP_HPP
