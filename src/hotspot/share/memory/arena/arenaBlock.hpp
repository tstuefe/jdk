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

// (need a constexpr version of exact_log2)
template <typename T>
constexpr int constexpr_exact_log2(T n) {
  static_assert(std::is_integral<T>::value && std::is_unsigned<T>::value,
                "exact_log2 requires an unsigned integer type.");
  int result = 0;
  while (n > 1) {
    n >>= 1;
    ++result;
  }
  return result;
}
static_assert(constexpr_exact_log2(16u) == 4, "Passed");

// We have 8 standard size classes: 256, 1K, 32K, 64K, ... 1M and another huge size class 2M.
// Block sizes > 1M are huge and var-sized and consist of several adjacent 2MB blocks.

// A "block class" identifies a valid block size. In practice, its log2 of the block size, normalized to 0.
typedef uint8_t block_size_class_t;

struct ArenaBlockHelpers : public AllStatic {

  static constexpr unsigned log2_min_block_size = 8; // 256
  static constexpr size_t min_block_size = nth_bit(log2_min_block_size);
  static constexpr unsigned log2_max_standard_block_size = 20;
  static constexpr size_t max_standard_block_size = nth_bit(log2_min_block_size); // 1MB
  static constexpr unsigned log2_huge_block_granularity = log2_max_standard_block_size + 1;
  static constexpr size_t huge_block_granularity = nth_bit(log2_huge_block_granularity); // 2MB

  static constexpr size_t slab_alignment = huge_block_granularity; // To be able to back it with 2MB pages

  static constexpr bool is_valid_standard_block_size(size_t blocksize) {
    return blocksize == 256 || blocksize == K || blocksize == K * 32 || blocksize == K * 64 ||
           blocksize == K * 128 || blocksize == K * 256 || blocksize == K * 512 || blocksize == M;
  }

  static constexpr bool is_valid_huge_size(size_t blocksize) {
    return blocksize > max_standard_block_size &&
           is_aligned(blocksize, huge_block_granularity);
  }

  static constexpr bool is_valid_block_size(size_t blocksize) {
    return is_valid_standard_block_size(blocksize) || blocksize == huge_block_granularity;
  }

  static constexpr bool is_valid_standard_block(size_t blocksize, const_blockptr_t b) {
    return is_aligned(b, blocksize);
  }

  static constexpr bool is_valid_huge_block(const_blockptr_t b) {
    return is_aligned(b, huge_block_granularity);
  }

  // block_size_class_t handling

  static constexpr block_size_class_t szclass_256 = constexpr_exact_log2(256);
  static constexpr block_size_class_t szclass_1K  = constexpr_exact_log2(K);
  static constexpr block_size_class_t szclass_32K  = constexpr_exact_log2(32*K);
  static constexpr block_size_class_t szclass_64K  = constexpr_exact_log2(64*K);
  static constexpr block_size_class_t szclass_128K  = constexpr_exact_log2(128*K);
  static constexpr block_size_class_t szclass_256K  = constexpr_exact_log2(256*K);
  static constexpr block_size_class_t szclass_512K  = constexpr_exact_log2(512*K);
  static constexpr block_size_class_t szclass_1M  = constexpr_exact_log2(M);
  static constexpr block_size_class_t szclass_2M  = constexpr_exact_log2(2*M);

  // We mostly use the size class for looking up slabs or caches for a given block size.
  // These lookup tables contain slots for all size classes, including invalid ones (which
  // makes lookup very easy).
  static constexpr unsigned highest_size_class = szclass_2M;
  static constexpr unsigned num_size_classes = szclass_2M + 1;
  static constexpr unsigned num_standard_size_classes = szclass_1M + 1;



  static constexpr bool is_valid_size_class(block_size_class_t szclass) {
    return szclass == szclass_256 || szclass == szclass_1K ||
           szclass >= szclass_32K && szclass <= szclass_2M) {
      return true;
    }
  }

  static constexpr size_t size_class_to_block_size(block_size_class_t szclass) {
    assert(is_valid_size_class(szclass), "invalid block size");
    return nth_bit(szclass);
  }

  static constexpr block_size_class_t block_size_to_size_class(size_t blocksize) {
    assert(is_valid_block_size(blocksize), "invalid block size");
    return constexpr_exact_log2(blocksize);
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

  // Creates an empty, unusable object
  BlockRange() :
    _base(nullptr),
    _block_size(0),
    _num_blocks(0) {
  }

  BlockRange(const_blockptr_t base, IndexType block_size, IndexType num_blocks)
    : _base(base), _block_size(block_size), _num_blocks(num_blocks) {
    assert(num_blocks > 0, "num_blocks must be positive");
    assert(is_aligned(_base, block_size), "Base pointer must be aligned to size class: " PTR_FORMAT, p2i(_base));
  }

  const_blockptr_t base() const { return _base; }
  size_t size_in_bytes() const { return (size_t)_block_size * _num_blocks; }

  const_blockptr_t end() const { return _base + size_in_bytes(); }
  IndexType num_blocks() const { return _num_blocks; }

  // True if b inside [base, end)
  bool contains(const_blockptr_t b) const {
    return b >= _base && b < end();
  }

  // True if b inside [base, end]
  bool contains_incl(const_blockptr_t b) const {
    return b >= _base && b <= end();
  }

  // True if block_index inside [baseindex, endindex)
  bool is_valid_block_index(IndexType block_index) const {
    return block_index < _num_blocks;
  }

  // True if block_index inside [baseindex, endindex]
  bool is_valid_block_index_incl(IndexType num_blocks) const {
    return num_blocks <= _num_blocks;
  }

  bool is_valid_block(const_blockptr_t b) const {
    return contains(b) && is_aligned(b, _block_size);
  }

  bool is_valid_block_incl(const_blockptr_t b) const {
    return contains_incl(b) && is_aligned(b, _block_size);
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
    return _base + ((size_t)block_index * _block_size);
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

