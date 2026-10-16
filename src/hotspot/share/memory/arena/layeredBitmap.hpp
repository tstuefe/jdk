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

private:

  WordType _bitmap_storage[num_words];
  WordType _full_segments_bitmap_storage[num_summary_words];
  WordType _nonempty_segments_bitmap_storage[num_summary_words];

  BitMapView _bitmap;
  BitMapView _full_segments_bitmap;
  BitMapView _nonempty_segments_bitmap;

#ifdef ASSERT
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
  static void verify_index_range_aligned_to_segment_boundaries(IndexType begidx, IndexType endidx) {
    verify_index_range_incl(begidx, endidx);
    assert(is_aligned(begidx, num_segment_bits), "not a segment boundary (%zu, %zu", begidx, num_segment_bits);
    assert(is_aligned(endidx, num_segment_bits), "not a segment boundary (%zu, %zu", endidx, num_segment_bits);
  }
  static void verify_segment_index_excl(IndexType segidx) {
    assert(segidx < num_segments, "out of bounds (segidx %zu, num_segments %zu)",
      segidx, num_segments);
  }
  static void verify_segment_index_incl(IndexType segidx) {
    assert(segidx <= num_segments, "out of bounds (segidx %zu, num_segments %zu)",
      segidx, num_segments);
  }
  static void verify_segment_index_range_incl(IndexType begsegidx, IndexType endsegidx) {
    assert(begsegidx <= endsegidx, "Sanity");
    verify_segment_index_incl(begsegidx);
    verify_segment_index_incl(endsegidx);
  }
  static void verify_segment_index_range_excl(IndexType begsegidx, IndexType endsegidx) {
    assert(begsegidx <= endsegidx, "Sanity");
    verify_segment_index_incl(begsegidx);
    verify_segment_index_incl(endsegidx);
  }
#endif // ASSERT

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

  // Helper for segment_is_full|empty
  template <bool X>
  bool segment_is_X(IndexType segidx) const {
    const IndexType idx_segstart = index_by_segment_index(segidx);
    const IndexType idx_segend = idx_segstart + num_segment_bits;
    return X ?
      (_bitmap.find_first_clear_bit(idx_segstart, idx_segend) != idx_segstart) :
      (_bitmap.find_first_set_bit(idx_segstart, idx_segend) != idx_segstart);
  }

  // Given a segment index, return true if segment is full in master bitmap
  bool segment_is_full(IndexType segidx) const {
    return segment_is_X<true>(segidx);
  }

  // Given a segment index, return true if segment is empty in master bitmap
  bool segment_is_empty(IndexType segidx) const {
    return segment_is_X<false>(segidx);
  }

  // Given a bit index range [begidx, endidx) aligned to segment boundaries,
  // return number of 1 bits in range
  uintx count_one_bits_segment_range(IndexType begidx, IndexType endidx) const {
    verify_index_range_aligned_to_segment_boundaries(begidx, endidx);
    uintx result = 0;
    const IndexType begsegidx = segment_index_by_index(begidx);
    const IndexType endsegidx = segment_index_by_index(endidx);
    for (IndexType segidx = begsegidx; segidx < endsegidx; segidx++) {
      if (_full_segments_bitmap.at(segidx)) {
        result += num_segment_bits;
      } else {
        if (_nonempty_segments_bitmap.at(segidx)) {
          const IndexType begidx_this_segment = index_by_segment_index(begsegidx);
          const IndexType endidx_this_segment = index_by_segment_index(endsegidx);
          result += _bitmap.count_one_bits(begidx_this_segment, endidx_this_segment);
        }
      }
    }
    return result;
  }

  // Given a bit index range [begidx, endidx) aligned to segment boundaries,
  // returns the bit index for the first 1 or 0 bit
  // X=true: find first 1 bit
  // X=false: find first 0 bit
  template <bool X>
  IndexType find_first_X_bit_segment_range(IndexType begidx, IndexType endidx) const {
    verify_index_range_aligned_to_segment_boundaries(begidx, endidx);
    IndexType result = endidx;
    // search in summary first
    const IndexType begsegidx = segment_index_by_index(begidx);
    const IndexType endsegidx = segment_index_by_index(endidx);
    verify_segment_index_range_incl(begsegidx, endsegidx);
    const IndexType segidx = X ? _nonempty_segments_bitmap.find_first_set_bit(begsegidx, endsegidx) :
                                 _full_segments_bitmap.find_first_clear_bit(begsegidx, endsegidx);
    // refine search inside segment
    if (segidx < endsegidx) {
      const IndexType begidx_this_segment = index_by_segment_index(segidx);
      const IndexType endidx_this_segment = begidx_this_segment + num_segment_bits;
      result = X ? _bitmap.find_first_set_bit(begidx_this_segment, endidx_this_segment) :
                _bitmap.find_first_clear_bit(begidx_this_segment, endidx_this_segment);
      assert(result < endidx_this_segment, "Summary bitmaps out of sync??"); // we should have found a bit
    }
    assert(result <= endidx, "Sanity");
    return result;
  }

  // Helper for find_first_set_bit and find_first_clear_bit
  // X=true: find first 1 bit
  // X=false: find first 0 bit
  template <bool X>
  IndexType find_first_X_bit(IndexType begidx, IndexType endidx) const {
    verify_index_range_incl(begidx, endidx);
    if (begidx == endidx) {
      return endidx;
    }

    IndexType result = 0;
    auto outer_search = [this, &result](IndexType idx1, IndexType idx2) {
      // search not-fully-enclosed segments in the master bitmap directly
      result = X ? _bitmap.find_first_set_bit(idx1, idx2) :
                   _bitmap.find_first_clear_bit(idx1, idx2);
      return result == idx2; // false => found it, stop looking
    };

    auto inner_search = [this, &result](IndexType idx1, IndexType idx2) {
      // The inner search will use the summary bitmaps
      result = find_first_X_bit_segment_range<X>(idx1, idx2);
      return result == idx2; // false => found it, stop looking
    };

    process_index_range_until(begidx, endidx,
              outer_search, inner_search, outer_search);

    return result;
  }

  // Given an arbitrary bit index range, calls three handler lambdas:
  // - a PrologueBitHandler lambda which is given bit indices of the bits in range preceding the first fully enclosed segment
  //   =>  void prologueBitHandler(IndexType begidx, IndexType endidx)
  // - a FullSegmentsHandler lambda which is given the bit indices of the fully enclosed segments
  //   =>  void fullSegmentsHandler(IndexType begidx, IndexType endidx)
  // - a EpilogueBitHandler lambda which is given bit indices of the bits in range following the last fully enclosed segment
  //   =>  void epilogue_bit_handler(IndexType begidx, IndexType endidx)
  // Example: segment size of 64, begidx=18, endidx=544 would call:
  //          1) prologue_bit_handler(18, 63)
  //          2) inner_bit_handler(64, 511)
  //          3) epilogue_bit_handler(512, 544)
  // If any of the prologue/inner/epilogue ranges is empty, the associated handler is not called
  template <typename PrologueBitHandler, typename InnerBitHandler, typename EpilogueBitHandler>
  static void process_index_range(IndexType begidx, IndexType endidx,
                                  PrologueBitHandler prologue_bit_handler,
                                  InnerBitHandler inner_bit_handler,
                                  EpilogueBitHandler epilogue_bit_handler)
  {
    verify_index_range_incl(begidx, endidx);
    const IndexType begidx_aligned = index_align_up_segment_start(begidx);
    const IndexType endidx_aligned = index_align_down_segment_start(endidx);
    if (begidx < begidx_aligned) {
      prologue_bit_handler(begidx, begidx_aligned);
    }
    if (begidx_aligned < endidx_aligned) {
      inner_bit_handler(begidx_aligned, endidx_aligned);
    }
    if (endidx_aligned < endidx) {
      prologue_bit_handler(endidx_aligned, endidx);
    }
  }

  // Same as process_index_range, but the handlers return a boolean; returning false stops the invocation sequence.
  template <typename PrologueBitHandler, typename InnerBitHandler, typename EpilogueBitHandler>
  static void process_index_range_until(IndexType begidx, IndexType endidx,
                                        PrologueBitHandler prologue_bit_handler,
                                        InnerBitHandler inner_bit_handler,
                                        EpilogueBitHandler epilogue_bit_handler)
  {
    verify_index_range_incl(begidx, endidx);
    const IndexType begidx_aligned = index_align_up_segment_start(begidx);
    const IndexType endidx_aligned = index_align_down_segment_start(endidx);
    if (begidx < begidx_aligned && !prologue_bit_handler(begidx, begidx_aligned)) {
      return;
    }
    if (begidx_aligned < endidx_aligned && !inner_bit_handler(begidx_aligned, endidx_aligned)) {
      return;
    }
    if (endidx_aligned < endidx && !epilogue_bit_handler(endidx_aligned, endidx)) {
      return;
    }
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
    if (segment_is_full(segidx)) {
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

    if (_full_segments_bitmap.at(segidx)) {
      _full_segments_bitmap.clear_bit(segidx);
    }

    assert(_nonempty_segments_bitmap.at(segidx), "Cannot have been empty before.");
    if (segment_is_empty(segidx)) {
      _nonempty_segments_bitmap.clear_bit(segidx);
    }
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
      return; // bits already set, nothing changes
    }

    // set in master bitmap
    _bitmap.set_range(begidx, endidx);

    // adjust summary bitmaps
    auto prefix_postfix_handler = [this](IndexType idx1, IndexType idx2) {
      const IndexType segidx = segment_index_by_index(idx1);
      // the non-empty bitmap always has the bit set since we just set 1..n bits in this segment
      _nonempty_segments_bitmap.set_bit(segidx);
      // the full-bitmap has only a 1 bit here if the segment is full. So we have to check the
      // segment.
      const IndexType idx_seg_start = index_by_segment_index(segidx);
      const IndexType idx_seg_end = idx_seg_start + num_segment_bits;
      const bool is_full = (_bitmap.find_first_clear_bit(idx_seg_start, idx_seg_end) == idx_seg_end);
      if (is_full) {
        _full_segments_bitmap.set_bit(segidx);
      }
    };

    auto inner_handler = [this](IndexType beg, IndexType end) {
      _nonempty_segments_bitmap.set_range(beg, end);
      _full_segments_bitmap.set_range(beg, end);
    };

    process_index_range(begidx, endidx,
       prefix_postfix_handler,
                       inner_handler,
                       prefix_postfix_handler);

  }

  // Clears bit range [begidx, endidx)
  void clear_range(IndexType begidx, IndexType endidx) {
    verify_index_range_incl(begidx, endidx);

    const IndexType num = endidx - begidx;
    if (num == 0) {
      return; // nothing changes
    }
    if (_bitmap.find_first_set_bit(begidx, endidx) == endidx) {
      return; // bits already cleared, nothing changes
    }

    // set in master bitmap
    _bitmap.set_range(begidx, endidx);

    // adjust summary bitmaps
    auto prefix_postfix_handler = [this](IndexType idx1, IndexType idx2) {
      const IndexType segidx = segment_index_by_index(idx1);
      // the full bitmap always has the bit cleared since we just cleared 1..n bits in this segment
      _full_segments_bitmap.clear_bit(segidx);
      // the non-empty bitmap has only the bit cleared if the segment is still non-empty. So we have to check the
      // segment.
      const IndexType idx_seg_start = index_by_segment_index(segidx);
      const IndexType idx_seg_end = idx_seg_start + num_segment_bits;
      const bool is_empty = (_bitmap.find_first_set_bit(idx_seg_start, idx_seg_end) == idx_seg_end);
      if (is_empty) {
        _nonempty_segments_bitmap.clear_bit(segidx);
      }
    };

    auto inner_handler = [this](IndexType beg, IndexType end) {
      _nonempty_segments_bitmap.clear_range(beg, end);
      _full_segments_bitmap.clear_range(beg, end);
    };

    process_index_range(begidx, endidx,
       prefix_postfix_handler,
                       inner_handler,
                       prefix_postfix_handler);

  }

  bool at(IndexType idx) const {
    return _bitmap.at(idx);
  }

  uintx count_one_bits(IndexType begidx, IndexType endidx) const {
    verify_index_range_incl(begidx, endidx);
    uintx result = 0;
    auto prefix_postfix_handler = [this, &result](IndexType idx1, IndexType idx2) {
      result += _bitmap.count_one_bits(idx1, idx2);
    };
    auto inner_handler = [this, &result](IndexType beg, IndexType end) {
      result += count_one_bits_segment_range(beg, end);
    };
    process_index_range(begidx, endidx,
        prefix_postfix_handler, inner_handler, prefix_postfix_handler);
    return result;
  }

  uintx count_one_bits() const {
    return count_one_bits_segment_range(0, num_segments);
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
        assert(!segment_is_full(segidx), "Sanity");
        assert(segment_is_empty(segidx), "Sanity");
        assert(!_full_segments_bitmap.at(segidx), "Sanity");
        assert(!_nonempty_segments_bitmap.at(segidx), "Sanity");
      } else if (popcount == num_segment_bits) {
        assert(segment_is_full(segidx), "Sanity");
        assert(!segment_is_empty(segidx), "Sanity");
        assert(_full_segments_bitmap.at(segidx), "Sanity");
        assert(_nonempty_segments_bitmap.at(segidx), "Sanity");
      } else {
        assert(popcount < num_summary_bits, "Sanity");
        assert(!segment_is_full(segidx), "Sanity");
        assert(!segment_is_empty(segidx), "Sanity");
        assert(!_full_segments_bitmap.at(segidx), "Sanity");
        assert(_nonempty_segments_bitmap.at(segidx), "Sanity");
      }
    }
  }
#endif // ASSERT

};

#endif // SHARE_MEMORY_ARENA_LAYERED_BITMAP_HPP
