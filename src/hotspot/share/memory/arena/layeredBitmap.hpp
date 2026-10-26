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
    assert(is_aligned(begidx, num_segment_bits), "not a segment boundary (%zu, %zu)", begidx, num_segment_bits);
    assert(is_aligned(endidx, num_segment_bits), "not a segment boundary (%zu, %zu)", endidx, num_segment_bits);
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
    const Range range_segidx = index_range_aligned_to_segment_index_range(begidx, endidx);
    const uintx from_full = _full_segments_bitmap.count_one_bits(range_segidx.beg, range_segidx.end) * num_segment_bits;
    // count the ones in not-full not-empty segments
    WordType diff[num_summary_words];
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

  // Helper function, calls find_X_X_bit on a bitmap
  template <bool forward, bool set>
  IndexType find_X_X_bit_in_master_bitmap(IndexType begidx, IndexType endidx) const {
    if (forward) {
      if (set) {
        return _bitmap.find_first_set_bit(begidx, endidx);
      } else {
        return _bitmap.find_first_clear_bit(begidx, endidx);
      }
    } else {
      if (set) {
        return _bitmap.find_last_set_bit(begidx, endidx);
      } else {
        return _bitmap.find_last_clear_bit(begidx, endidx);
      }
    }
  }

  // Given a bit index range [begidx, endidx) aligned to segment boundaries,
  // returns the bit index for the first|last 1|0 bit
  template <bool forward, bool set>
  IndexType find_X_X_bit_in_aligned_range(IndexType begidx, IndexType endidx) const {
    verify_index_range_aligned_to_segment_boundaries(begidx, endidx);
    IndexType result = endidx;
    // search in summary first
    const Range range_segidx = index_range_aligned_to_segment_index_range(begidx, endidx);
    // Search the segment index for the first|last 1|0 bit
    IndexType segidx = 0;
    if (forward) {
      if (set) {
        segidx = _nonempty_segments_bitmap.find_first_set_bit(range_segidx.beg, range_segidx.end);
      } else {
        segidx = _full_segments_bitmap.find_first_clear_bit(range_segidx.beg, range_segidx.end);
      }
    } else {
      if (set) {
        segidx = _nonempty_segments_bitmap.find_last_set_bit(range_segidx.beg, range_segidx.end);
      } else {
        segidx = _full_segments_bitmap.find_last_clear_bit(range_segidx.beg, range_segidx.end);
      }
    }
    if (segidx < range_segidx.end) {
      // Now search inside the segment
      const Range range_idx_aligned = segment_index_to_index_range(segidx);
      result = find_X_X_bit_in_master_bitmap<forward, set>(range_idx_aligned.beg, range_idx_aligned.end);
      // We should have found something, since the summary bitmaps say so
      assert(result < range_idx_aligned.end, "Summary bitmaps out of sync??");
    }
    assert(result <= endidx, "Sanity");
    return result;
  }

  template <bool forward, bool set>
  IndexType find_X_X_bit(IndexType begidx, IndexType endidx) const {
    verify_index_range_incl(begidx, endidx);
    if (begidx == endidx) {
      return endidx;
    }
    IndexType result = endidx;
    auto outer_search = [this, &result](IndexType idx1, IndexType idx2) {
      // search outer bits in the master bitmap directly
      const IndexType idx = find_X_X_bit_in_master_bitmap<forward, set>(idx1, idx2);
      if (idx < idx2) {
        result = idx;
        return false; // stop looking
      }
      return true;
    };
    auto inner_search = [this, &result](IndexType idx1, IndexType idx2) {
      const IndexType idx = find_X_X_bit_in_aligned_range<forward, set>(idx1, idx2);
      if (idx < idx2) {
        result = idx;
        return false; // stop looking
      }
      return true;
    };

    if (forward) {
      process_index_range_forward(begidx, endidx, outer_search, inner_search);
    } else {
      process_index_range_backward(begidx, endidx, outer_search, inner_search);
    }

    return result;
  }

  // Given an arbitrary bit index range, and two BitsHandler lambdas (outer_bits_handler, inner_bits_handler),
  // calls them up to three times.
  //
  // The lambdas have the format:
  // bool foo(idx_t beg, idx_t end);
  //
  // If forward=true, the function calls:
  // 1) the outer_bits_handler for bit range preceding the first segment-aligned bit index
  // 2) the inner_bits_handler for bits from the first to the last segment-aligned bit indexes
  // 3) the outer_bits_handler again for bits following the last segment-aligned bit
  //
  // If forward=false, the order is reversed.
  //
  // The handlers are not called for empty ranges.
  //
  // A handler returns true to continue with the next handler, false to abort the sequence.
  //
  // Example: segment size of 64, begidx=18, endidx=544, forward=true would call:
  //          1) outer_bits_handler(18, 64)
  //          2) inner_bits_handler(64, 512)
  //          3) outer_bits_handler(512, 544)
  //
  // Example: segment size of 64, begidx=18, endidx=20, forward=false would call:
  //          3) outer_bits_handler(18, 20)
  template <typename OuterBitsHandler, typename InnerBitsHandler, bool forward>
  static void process_index_range(IndexType begidx, IndexType endidx,
                                  OuterBitsHandler outer_bits_handler,
                                  InnerBitsHandler inner_bits_handler)
  {
    verify_index_range_incl(begidx, endidx);
    if (begidx == endidx) {
      return;
    }
    const IndexType begidx_aligned = index_align_up_segment_start(begidx);
    const IndexType endidx_aligned = index_align_down_segment_start(endidx);
    if (begidx_aligned > endidx_aligned) { // same segment
      outer_bits_handler(begidx, endidx);
    } else {
      const bool have_prologue_bits = begidx < begidx_aligned;
      const bool have_inner_bits = begidx_aligned < endidx_aligned;
      const bool have_epilogue_bits = endidx_aligned < endidx;
      if (forward && have_prologue_bits) {
        if (!outer_bits_handler(begidx, begidx_aligned)) return;
      }
      if (!forward && have_epilogue_bits) {
        if (!outer_bits_handler(endidx_aligned, endidx)) return;
      }
      if (have_inner_bits) {
        if (!inner_bits_handler(begidx_aligned, endidx_aligned)) return;
      }
      if (forward && have_epilogue_bits) {
        if (!outer_bits_handler(endidx_aligned, endidx)) return;
      }
      if (!forward && have_prologue_bits) {
        if (!outer_bits_handler(begidx, begidx_aligned)) return;
      }
    }
  }

  template <typename OuterBitsHandler, typename InnerBitsHandler>
  static void process_index_range_forward(IndexType begidx, IndexType endidx,
                                          OuterBitsHandler outer_bits_handler,
                                          InnerBitsHandler inner_bits_handler) {
    process_index_range<OuterBitsHandler, InnerBitsHandler, true>(begidx, endidx, outer_bits_handler, inner_bits_handler);
  }

  template <typename OuterBitsHandler, typename InnerBitsHandler>
  static void process_index_range_backward(IndexType begidx, IndexType endidx,
                                           OuterBitsHandler outer_bits_handler,
                                           InnerBitsHandler inner_bits_handler) {
    process_index_range<OuterBitsHandler, InnerBitsHandler, false>(begidx, endidx, outer_bits_handler, inner_bits_handler);
  }

  // called when the bitmap was set from outer values and
  // we need to recalculate the summaries.
  void recalculate_summary_bitmaps() {
    for (IndexType segidx = 0; segidx < num_segments; segidx++) {
      _full_segments_bitmap.at_put(segidx, segment_is_full(segidx));
      _nonempty_segments_bitmap.at_put(segidx, !segment_is_empty(segidx));
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
    return find_X_X_bit<true, true>(beg, end);
  }

  IndexType find_first_clear_bit(IndexType beg, IndexType end) const {
    return find_X_X_bit<true, false>(beg, end);
  }

  IndexType find_last_set_bit(IndexType beg, IndexType end) const {
    return find_X_X_bit<false, true>(beg, end);
  }

  IndexType find_last_clear_bit(IndexType beg, IndexType end) const {
    return find_X_X_bit<false, false>(beg, end);
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

    // set in summary bitmaps
    auto outer_bits_handler = [this](IndexType idx1, IndexType) {
      const IndexType segidx = index_to_segment_index(idx1);
      _nonempty_segments_bitmap.set_bit(segidx);
      const bool is_full = segment_is_full(segidx);
      if (is_full) {
        _full_segments_bitmap.set_bit(segidx);
      }
      return true;
    };
    auto inner_bits_handler = [this](IndexType idx1, IndexType idx2) {
      const Range range_segidx = index_range_aligned_to_segment_index_range(idx1, idx2);
      _nonempty_segments_bitmap.set_range(range_segidx.beg, range_segidx.end);
      _full_segments_bitmap.set_range(range_segidx.beg, range_segidx.end);
      return true;
    };

    process_index_range_forward(begidx, endidx, outer_bits_handler, inner_bits_handler);
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

    // clear in master bitmap
    _bitmap.clear_range(begidx, endidx);

    // clear in summary bitmaps
    auto outer_bits_handler = [this](IndexType idx1, IndexType) {
      const IndexType segidx = index_to_segment_index(idx1);
      _full_segments_bitmap.clear_bit(segidx);
      const bool is_empty = segment_is_empty(segidx);
      if (is_empty) {
        _nonempty_segments_bitmap.clear_bit(segidx);
      }
      return true;
    };
    auto inner_bits_handler = [this](IndexType idx1, IndexType idx2) {
      const Range range_segidx = index_range_aligned_to_segment_index_range(idx1, idx2);
      _nonempty_segments_bitmap.clear_range(range_segidx.beg, range_segidx.end);
      _full_segments_bitmap.clear_range(range_segidx.beg, range_segidx.end);
      return true;
    };

    process_index_range_forward(begidx, endidx, outer_bits_handler, inner_bits_handler);
  }

  bool at(IndexType idx) const {
    return _bitmap.at(idx);
  }

  uintx count_one_bits(IndexType begidx, IndexType endidx) const {
    verify_index_range_incl(begidx, endidx);
    uintx result = 0;
    auto outer_bits_handler = [this, &result](IndexType idx1, IndexType idx2) {
      result += _bitmap.count_one_bits(idx1, idx2);
      return true;
    };
    auto inner_bits_handler = [this, &result](IndexType idx1, IndexType idx2) {
      result += count_one_bits_in_aligned_range(idx1, idx2);
      return true;
    };
    process_index_range_forward(begidx, endidx, outer_bits_handler, inner_bits_handler);
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

  // Set operations.
  void set_union(const BitMap& bits) {
    (void) set_union_with_result(bits);
  }

  void set_difference(const BitMap& bits) {
    (void) set_difference_with_result(bits);
  }

  void set_intersection(const BitMap& bits) {
    (void) set_intersection_with_result(bits);
  }

  // Returns true iff "this" is a superset of "bits".
  bool contains(const BitMap& bits) const {
    return _bitmap.contains(bits);
  }

  // Returns true iff "this and "bits" have a non-empty intersection.
  bool intersects(const BitMap& bits) const {
    return _bitmap.intersects(bits);
  }

  // Returns result of whether this map changed
  // during the operation
  bool set_union_with_result(const BitMap& bits) {
    const bool changed = _bitmap.set_union_with_result(bits);
    if (changed) {
      recalculate_summary_bitmaps();
    }
    return changed;
  }

  bool set_difference_with_result(const BitMap& bits) {
    const bool changed = _bitmap.set_difference_with_result(bits);
    if (changed) {
      recalculate_summary_bitmaps();
    }
    return changed;
  }

  bool set_intersection_with_result(const BitMap& bits) {
    const bool changed = _bitmap.set_intersection_with_result(bits);
    if (changed) {
      recalculate_summary_bitmaps();
    }
    return changed;
  }

  void set_from(const BitMap& bits) {
    _bitmap.set_from(bits);
    recalculate_summary_bitmaps();
  }

  bool is_same(const BitMap& bits) const {
    return _bitmap.is_same(bits);
  }

  void clear() {
    memset(_storage, 0, sizeof(Storage));
  }

  // This is mostly for tests. Fills the bitmap with a pattern.
  void propagate_bit_pattern(WordType bit_pattern) {
    for (IndexType idx = 0; idx < num_words; idx++) {
      _storage->_bitmap_storage[idx] = bit_pattern;
    }
    constexpr WordType fullword = right_n_bits(BitsPerWord);
    const WordType summary_pattern_full = bit_pattern == fullword ? fullword : 0;
    const WordType summary_pattern_nonempty = bit_pattern > 0 ? fullword : 0;
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

  // Convert this bitmap to a flat bitmap.
  void convert_to_flat_bitmap(BitMap& out) const {
    out.set_from(_bitmap);
  }


};

#endif // SHARE_MEMORY_ARENA_LAYERED_BITMAP_HPP
