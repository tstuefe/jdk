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


#ifndef SHARE_MEMORY_ARENA_ARENABLOCK
#define SHARE_MEMORY_ARENA_ARENABLOCK

#include "memory/allStatic.hpp"
#include "utilities/align.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"

// an opaque block pointer.
typedef uint8_t* blockptr_t;
typedef const uint8_t* const_blockptr_t;

static_assert(std::is_pointer_v<blockptr_t>, "Type must be a pointer");
static_assert(sizeof(std::remove_pointer_t<blockptr_t>) == 1, "Pointer arithmetic must be in byte-sized increments");

// We have 8 standard size classes: 256, 1K, 32K, 64K, ... 1M
// Anything beyond that is a huge block and var-sized.

struct ArenaBlockHelpers : public AllStatic {

  static constexpr size_t min_block_size = 256;
  static constexpr size_t max_standard_block_size = M;
  static constexpr size_t max_huge_block_size = G;
  static constexpr size_t huge_block_granularity = 2 * M;

  static constexpr bool is_valid_standard_size_class(size_t szclass) {
    return szclass == 256 || szclass == K || szclass == K * 32 || szclass == K * 64 ||
           szclass == K * 128 || szclass == K * 256 || szclass == K * 512 || szclass == M;
  }

  static constexpr bool is_valid_huge_size_class(size_t szclass) {
    return szclass > max_standard_block_size &&
           szclass <= max_huge_block_size &&
           is_aligned(szclass, huge_block_granularity);
  }

  static constexpr int size_class_to_id_exact(size_t szclass) {
    switch (szclass) {
      case 256: return 0;
      case K: return 1;
      case K * 32: return 3;
      case K * 64: return 4;
      case K * 128: return 5;
      case K * 256: return 6;
      case K * 512: return 7;
      case M: return 8;
      default: ShouldNotReachHere();
    }
  }

  static constexpr int normalize_size_class(size_t szclass) {
    if (szclass <= 256) { return 256; }
    if (szclass <= K) { return K; }
    if (szclass <= K * 32) { return K * 32; }
    if (szclass <= K * 64) { return K * 64; }
    if (szclass <= K * 128) { return K * 128; }
    if (szclass <= K * 256) { return K * 256; }
    if (szclass <= K * 512) { return K * 512; }
    if (szclass <= M) { return M; }
    return align_up(szclass, huge_block_granularity);
  }

  static constexpr bool is_valid_standard_block(size_t szclass, const_blockptr_t b) {
    return is_aligned(b, szclass);
  }

  static constexpr bool is_valid_huge_block(const_blockptr_t b) {
    return is_aligned(b, huge_block_granularity);
  }

};

// Helper class for ranges of uniformly-sized blocks
template <typename IndexType>
class BlockRange {
  const_blockptr_t const _base;
  const unsigned _block_size;
  const unsigned _num_blocks;

  static_assert(std::is_integral_v<IndexType>, "invalid block index type");
  static_assert(std::is_unsigned_v<IndexType>, "invalid block index type");

public:

  BlockRange(const_blockptr_t base, IndexType block_size, IndexType num_blocks)
    : _base(base), _block_size(block_size), _num_blocks(num_blocks) {
    assert(num_blocks > 0, "num_blocks must be positive");
    assert(is_aligned(_base, block_size), "Base pointer must be aligned to size class: " PTR_FORMAT, p2i(_base));
  }

  const_blockptr_t base() const { return _base; }
  size_t size_in_bytes() const { return _block_size * _num_blocks; }
  const_blockptr_t end() const { return _base + size_in_bytes(); }
  IndexType num_blocks() const { return _num_blocks; }

  // True if b inside [base, end)
  bool contains(const_blockptr_t b) const {
    return b >= _base && b < end();
  }

  // True if b inside [base, end]
  bool contains_incl(const_blockptr_t b) const {
    return b >= _base && b < end();
  }

  // True if block_index inside [baseindex, endindex)
  bool is_valid_block_index(IndexType block_index) const {
    return block_index >= 0 && block_index < _num_blocks;
  }

  // True if block_index inside [baseindex, endindex]
  bool is_valid_block_index_incl(IndexType num_blocks) const {
    return num_blocks >= 0 && num_blocks <= _num_blocks;
  }

  bool is_valid_block(const_blockptr_t b) const {
    return contains(b) && is_valid_block_index(block_to_index(b));
  }

  bool is_valid_block_incl(const_blockptr_t b) const {
    return contains_incl(b) && is_valid_block_index(block_to_index(b));
  }

  const_blockptr_t index_to_block(IndexType block_index) const {
    assert(is_valid_block_index(block_index), "invalid block index");
    return index_to_block_without_asserts(block_index);
  }

  const_blockptr_t index_to_block_incl(IndexType block_index) const {
    assert(is_valid_block_index_incl(block_index), "invalid block index");
    return index_to_block_without_asserts(block_index);
  }

  const_blockptr_t index_to_block_without_asserts(IndexType block_index) const {
    return _base + (block_index * _block_size);
  }

  // Given a pointer, not necessarily aligned, that points into this range,
  // return the index of the containing block
  IndexType index_of_containing_block(const_address p) const {
    DEBUG_ONLY(check_contains(p));
    return block_to_index(start_of_containing_block(p));
  }

  // Given a pointer, not necessarily aligned, that points into this range,
  // return the start address of the containing block
  const_blockptr_t start_of_containing_block(const_address p) const {
    DEBUG_ONLY(check_contains(p));
    return align_down(p, _block_size);
  }

  IndexType block_to_index(const_blockptr_t b) const {
    DEBUG_ONLY(check_is_valid_block(b));
    return block_to_index_without_assert(b);
  }

  IndexType block_to_index_incl(const_blockptr_t b) const {
    DEBUG_ONLY(check_is_valid_block_incl(b));
    return block_to_index_without_assert(b);
  }

  IndexType block_to_index_without_assert(const_blockptr_t b) const {
    return checked_cast<IndexType>((p2u(b) - p2u(_base)) / _block_size);
  }

#ifdef ASSERT
  void check_contains(const_address p) const {
    assert(contains(p), "Pointer out of range: " PTR_FORMAT, p2i(p));
  }
  void check_contains_incl(const_address p) const {
    assert(contains_incl(p), "Pointer out of range: " PTR_FORMAT, p2i(p));
  }
  void check_is_valid_block(const_blockptr_t b) const {
    assert(is_valid_block(b), "Block pointer invalid: " PTR_FORMAT, p2i(b));
  }
  void check_is_valid_block_incl(const_blockptr_t b) const {
    assert(is_valid_block_incl(b), "Block pointer invalid: " PTR_FORMAT, p2i(b));
  }
  void check_is_valid_block_index(IndexType block_index) const {
    assert(is_valid_block_index(block_index), "Block index invalid: %zu", checked_cast<uintptr_t>(block_index));
  }
  void check_is_valid_block_index_incl(IndexType num) const {
    assert(is_valid_block_index_incl(num), "Invalid number of blocks: %zu", checked_cast<uintptr_t>(num));
  }
#endif
};

#endif // SHARE_MEMORY_ARENA_ARENABLOCK

