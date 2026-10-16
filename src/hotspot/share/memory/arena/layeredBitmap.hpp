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
#include "utilities/ostream.hpp"

template <BitMap::idx_t num_summary_bits, BitMap::idx_t num_segment_bits>
class LayeredBitmap {
public:
  typedef BitMap::bm_word_t WordType;
  typedef BitMap::idx_t IndexType;

  static constexpr IndexType num_bits = num_summary_bits * num_segment_bits;
  static constexpr IndexType num_segments = num_summary_bits;
  static constexpr size_t num_words = BitMap::calc_size_in_words(num_bits);
  static constexpr size_t num_summary_words = BitMap::calc_size_in_words(num_summary_bits);
  static constexpr size_t num_segment_words = BitMap::calc_size_in_words(num_segment_bits);

  STATIC_ASSERT(num_summary_bits >= 64);
  STATIC_ASSERT(is_power_of_2(num_summary_bits));
  STATIC_ASSERT(num_segment_bits >= 64);
  STATIC_ASSERT(is_power_of_2(num_segment_bits));

  NONCOPYABLE(LayeredBitmap);

  struct alignas(DEFAULT_CACHE_LINE_SIZE) Storage {
    WordType _full_segments_bitmap_storage[num_summary_words];
    WordType _nonempty_segments_bitmap_storage[num_summary_words];
    WordType _bitmap_storage[num_words];
  };

private:

  Storage* const _storage;
  BitMapView _full_segments_bitmap;
  BitMapView _nonempty_segments_bitmap;
  BitMapView _bitmap;

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
    verify_segment_index_excl(begsegidx);
    verify_segment_index_excl(endsegidx);
  }

  // Given a bit index, return the index of enclosing segment
  // Passing num_bits is allowed and returns num_summary_bits
  static IndexType index_to_segment_index(IndexType idx) {
    verify_index_incl(idx);
    const IndexType segidx = idx / num_segment_bits;
    verify_segment_index_incl(segidx);
    return segidx;
  }

  // Given a segment index, return idx of first bit.
  // Passing num_segments is allowed and returns num_bits
  static IndexType segment_index_to_index(IndexType segidx) {
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

  struct Range { IndexType beg; IndexType end; };

  // Given an index range that is aligned to segment boundaries, return the segment index range
  static Range index_range_aligned_to_segment_index_range(IndexType begidx, IndexType endidx) {
    verify_index_range_aligned_to_segment_boundaries(begidx, endidx);
    return Range { index_to_segment_index(begidx), index_to_segment_index(endidx) };
  }

  // Given a segment index range, return the index range covering all segments
  static Range segment_index_range_to_index_range(IndexType segbegidx, IndexType segendidx) {
    verify_segment_index_range_incl(segbegidx, segendidx);
    return Range { segment_index_to_index(segbegidx), segment_index_to_index(segendidx) };
  }

  // Given a single segment index, return the index range covering this segment
  static Range segment_index_to_index_range(IndexType segidx) {
    verify_segment_index_excl(segidx);
    return Range { segment_index_to_index(segidx), segment_index_to_index(segidx + 1) };
  }

  // Helper for segment_is_full|empty
  template <bool X>
  bool segment_is_X(IndexType segidx) const {
    const Range range_idx = segment_index_to_index_range(segidx);
    return X ?
      (_bitmap.find_first_clear_bit(range_idx.beg, range_idx.end) == range_idx.end) :
      (_bitmap.find_first_set_bit(range_idx.beg, range_idx.end) == range_idx.end);
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
  uintx count_one_bits_in_aligned_range(IndexType begidx, IndexType endidx) const {
    verify_index_range_aligned_to_segment_boundaries(begidx, endidx);
    const Range range_segidx = index_range_aligned_to_segment_index_range(begidx, endidx);
    const uintx from_full = _full_segments_bitmap.count_one_bits(range_segidx.beg, range_segidx.end) * num_segment_bits;
    // count the ones in not-full not-empty segments
    WordType diff[num_segments];
    BitMapView tmpbm(diff, num_segments);
    tmpbm.set_from(_nonempty_segments_bitmap); // all segments that are not empty...
    tmpbm.set_difference(_full_segments_bitmap); // but not full.
    uintx from_half_full = 0;
    IndexType segidx = range_segidx.beg;
    while (segidx < range_segidx.end) {
      segidx = tmpbm.find_first_set_bit(segidx, range_segidx.end);
      if (segidx < range_segidx.end) {
        const IndexType segidx_end = tmpbm.find_first_clear_bit(segidx, range_segidx.end);
        const Range range_idx = segment_index_range_to_index_range(segidx, segidx_end);
        from_half_full += _bitmap.count_one_bits(range_idx.beg, range_idx.end);
        segidx = segidx_end;
      }
    }
    return from_half_full + from_full;
  }

  // Given a bit index range [begidx, endidx) aligned to segment boundaries,
  // returns the bit index for the first 1 or 0 bit
  // X=true: find first 1 bit
  // X=false: find first 0 bit
  template <bool X>
  IndexType find_first_X_bit_in_aligned_range(IndexType begidx, IndexType endidx) const {
    verify_index_range_aligned_to_segment_boundaries(begidx, endidx);
    IndexType result = endidx;
    // search in summary first
    const Range range_segidx = index_range_aligned_to_segment_index_range(begidx, endidx);
    const IndexType segidx = X ? _nonempty_segments_bitmap.find_first_set_bit(range_segidx.beg, range_segidx.end) :
                                 _full_segments_bitmap.find_first_clear_bit(range_segidx.beg, range_segidx.end);
    // refine search inside segment
    if (segidx < range_segidx.end) {
      const Range range_idx = segment_index_to_index_range(segidx);
      result = X ? _bitmap.find_first_set_bit(range_idx.beg, range_idx.end) :
                _bitmap.find_first_clear_bit(range_idx.beg, range_idx.end);
      assert(result < range_idx.end, "Summary bitmaps out of sync??"); // we should have found a bit
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
      result = find_first_X_bit_in_aligned_range<X>(idx1, idx2);
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
    if (begidx == endidx) {
      return;
    }
    const IndexType begidx_aligned = index_align_up_segment_start(begidx);
    const IndexType endidx_aligned = index_align_down_segment_start(endidx);
    if (begidx_aligned > endidx_aligned) { // same segment
      prologue_bit_handler(begidx, endidx);
    } else {
      if (begidx < begidx_aligned) {
        prologue_bit_handler(begidx, begidx_aligned);
      }
      if (begidx_aligned < endidx_aligned) {
        inner_bit_handler(begidx_aligned, endidx_aligned);
      }
      if (endidx_aligned < endidx) {
        epilogue_bit_handler(endidx_aligned, endidx);
      }
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
    if (begidx == endidx) {
      return;
    }
    const IndexType begidx_aligned = index_align_up_segment_start(begidx);
    const IndexType endidx_aligned = index_align_down_segment_start(endidx);
    if (begidx_aligned > endidx_aligned) { // same segment
      if (!prologue_bit_handler(begidx, endidx)) return;
    } else {
      if (begidx < begidx_aligned) {
        if (!prologue_bit_handler(begidx, begidx_aligned)) return;
      }
      if (begidx_aligned < endidx_aligned) {
        if (!inner_bit_handler(begidx_aligned, endidx_aligned)) return;
      }
      if (endidx_aligned < endidx) {
        if (!epilogue_bit_handler(endidx_aligned, endidx)) return;
      }
    }
  }

public:

  LayeredBitmap(Storage* storage, bool do_clear)
  : _storage(storage),
    _full_segments_bitmap(_storage->_full_segments_bitmap_storage, num_segments),
    _nonempty_segments_bitmap(_storage->_nonempty_segments_bitmap_storage, num_segments),
    _bitmap(_storage->_bitmap_storage, num_bits)
  {
    if (do_clear) {
      clear();
    }
  }

  void set_bit(IndexType idx) {
    verify_index_excl(idx);

    if (_bitmap.at(idx)) {
      return; // nothing changes
    }
    _bitmap.set_bit(idx);

    // Update summary bitmaps
    const IndexType segidx = index_to_segment_index(idx);

    if (!_nonempty_segments_bitmap.at(segidx)) { // avoid unnecessary writes
      _nonempty_segments_bitmap.set_bit(segidx);
    }

    assert(!_full_segments_bitmap.at(segidx), "Cannot have been full before.");
    if (segment_is_full(segidx)) {
      _full_segments_bitmap.set_bit(segidx);
    }
  }

  void clear_bit(IndexType idx) {
    verify_index_excl(idx);

    if (!_bitmap.at(idx)) {
      return; // nothing changes
    }
    _bitmap.clear_bit(idx);

    // Update summary bitmaps
    const IndexType segidx = index_to_segment_index(idx);

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
    if (find_first_clear_bit(begidx, endidx) == endidx) {
      return; // bits already set, nothing changes
    }

    // set in master bitmap
    _bitmap.set_range(begidx, endidx);

    // adjust summary bitmaps

    auto prologue_epilogue_handler = [this](IndexType idx1, IndexType idx2) {
#ifdef ASSERT
      const IndexType num_prologue_bits = idx2 - idx1;
      assert(num_prologue_bits < num_segment_bits && num_prologue_bits > 0, "Sanity");
#endif
      const IndexType segidx = index_to_segment_index(idx1);
      _nonempty_segments_bitmap.set_bit(segidx);
      const bool is_full = segment_is_full(segidx);
      if (is_full) {
        _full_segments_bitmap.set_bit(segidx);
      }
    };

    auto inner_handler = [this](IndexType begidx, IndexType endidx) {
      const Range range_segidx = index_range_aligned_to_segment_index_range(begidx, endidx);
      _nonempty_segments_bitmap.set_range(range_segidx.beg, range_segidx.end);
      _full_segments_bitmap.set_range(range_segidx.beg, range_segidx.end);
    };

    process_index_range(begidx, endidx, prologue_epilogue_handler, inner_handler, prologue_epilogue_handler);
  }

  // Clears bit range [begidx, endidx)
  void clear_range(IndexType begidx, IndexType endidx) {
    verify_index_range_incl(begidx, endidx);

    const IndexType num = endidx - begidx;
    if (num == 0) {
      return; // nothing changes
    }
    if (find_first_set_bit(begidx, endidx) == endidx) {
      return; // bits already cleared, nothing changes
    }

    // set in master bitmap
    _bitmap.clear_range(begidx, endidx);

    // adjust summary bitmaps
    auto prefix_postfix_handler = [this](IndexType idx1, IndexType) {
      const IndexType segidx = index_to_segment_index(idx1);
      _full_segments_bitmap.clear_bit(segidx);
      const bool is_empty = segment_is_empty(segidx);
      if (is_empty) {
        _nonempty_segments_bitmap.clear_bit(segidx);
      }
    };

    auto inner_handler = [this](IndexType begidx, IndexType endidx) {
      const Range range_segidx = index_range_aligned_to_segment_index_range(begidx, endidx);
      _nonempty_segments_bitmap.clear_range(range_segidx.beg, range_segidx.end);
      _full_segments_bitmap.clear_range(range_segidx.beg, range_segidx.end);
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
    auto inner_handler = [this, &result](IndexType idx1, IndexType idx2) {
      result += count_one_bits_in_aligned_range(idx1, idx2);
    };
    process_index_range(begidx, endidx,
        prefix_postfix_handler, inner_handler, prefix_postfix_handler);
    return result;
  }

  uintx count_one_bits() const {
    return count_one_bits_in_aligned_range(0, num_bits);
  }

  bool is_empty() const {
    return _nonempty_segments_bitmap.is_empty();
  }

  bool is_full() const {
    return _full_segments_bitmap.is_full();
  }

  void clear() {
    memset(_storage, 0, sizeof(Storage));
  }

  // This is mostly for tests. Fills the bitmap with a pattern.
  void propagate_bit_pattern(WordType bit_pattern) {
    for (IndexType idx = 0; idx < num_words; idx++) {
      _storage->_bitmap_storage[idx] = bit_pattern;
    }
    const WordType summary_pattern_full = (bit_pattern == SIZE_MAX) ? SIZE_MAX : 0;
    const WordType summary_pattern_nonempty = (bit_pattern > 0) ? SIZE_MAX : 0;
    for (IndexType idx = 0; idx < num_summary_words; idx++) {
      _storage->_full_segments_bitmap_storage[idx] = summary_pattern_full;
      _storage->_nonempty_segments_bitmap_storage[idx] = summary_pattern_nonempty;
    }
  }

#ifdef ASSERT
  void verify() const {
    for (IndexType segidx = 0; segidx < num_segments; segidx++) {
      const Range range_idx = segment_index_to_index_range(segidx);
      const IndexType popcount = _bitmap.count_one_bits(range_idx.beg, range_idx.end);
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
        assert(popcount < num_segment_bits, "Sanity");
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
