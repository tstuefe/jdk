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

#ifndef SHARE_MEMORY_ARENA_ARENABLOCK_HPP
#define SHARE_MEMORY_ARENA_ARENABLOCK_HPP

#include "memory/allocation.hpp"
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

// A "block class" identifies a valid block size. Its really an enum, but uint8 is more handy.
typedef uint8_t block_size_class_t;

struct ArenaBlockHelpers : public AllStatic {

#define ARENA_VALID_BLOCK_SIZES_DO(X) \
  X(0, 256u, 256) \
  X(1, 1024u, 1K) \
  X(2, 32768u, 32K) \
  X(3, 65536u, 64K) \
  X(4, 131072u, 128K) \
  X(5, 262144u, 256K) \
  X(6, 524288u, 512K) \
  X(7, 1048576u, 1M) \
  X(8, 2097152u, 2M)

#define X(szclass, blocksize, name) \
  static constexpr block_size_class_t szclass_ ## name = szclass; \
  static constexpr size_t block_size_ ## name = blocksize;
  ARENA_VALID_BLOCK_SIZES_DO(X)
#undef X

  static constexpr block_size_class_t highest_standard_size_class = szclass_1M;
  static constexpr block_size_class_t huge_size_class = szclass_2M;
  static constexpr block_size_class_t highest_size_class = huge_size_class;

  static constexpr unsigned log2_max_standard_block_size = 20; // 1MB
  static constexpr size_t max_standard_block_size = nth_bit(log2_max_standard_block_size);
  static constexpr unsigned log2_huge_block_granularity = log2_max_standard_block_size + 1;
  static constexpr size_t huge_block_granularity = nth_bit(log2_huge_block_granularity); // 2MB
  static constexpr size_t huge_block_max_size = 2 * G;

  // A translation table translates from size class to block size
  static constexpr size_t size_class_to_block_size_table[] = {
#define X(szclass, blocksize, name) blocksize,
    ARENA_VALID_BLOCK_SIZES_DO(X)
#undef X
  };

  static constexpr unsigned num_size_classes = sizeof(size_class_to_block_size_table)/sizeof(size_class_to_block_size_table[0]);
  static constexpr unsigned num_standard_size_classes = num_size_classes - 1;

  // Given a size class, return block size (for huge_size_class, returns huge_block_granularity)
  static constexpr size_t size_class_to_block_size(block_size_class_t szclass) {
    assert(szclass < num_size_classes, "Invalid size class (%u)", szclass);
    return size_class_to_block_size_table[szclass];
  }

  // A translation table translates from log2 block size to size class
  static constexpr block_size_class_t log2_block_size_to_size_class_table[log2_max_standard_block_size + 1] = {
    // [1, 256] => 256
    szclass_256,
    szclass_256,
    szclass_256,
    szclass_256,
    szclass_256,
    szclass_256,
    szclass_256,
    szclass_256,
    szclass_256,
    // [512, 1K] => 1K
    szclass_1K,
    szclass_1K,
    // [2K, 32K] => 32K
    szclass_32K,
    szclass_32K,
    szclass_32K,
    szclass_32K,
    szclass_32K,
    // From now on its 1:1
    szclass_64K,
    szclass_128K,
    szclass_256K,
    szclass_512K,
    szclass_1M
  };

  // Returns the size class for a given valid standard block size (one of 256, K, 32K, 64K ...).
  static constexpr block_size_class_t valid_standard_block_size_to_size_class(size_t block_size) {
    DEBUG_ONLY(verify_valid_standard_block_size(block_size);)
    const unsigned log2_blocksize = constexpr_exact_log2(block_size);
    return log2_block_size_to_size_class_table[log2_blocksize];
  }

  // Returns the size class for a given pow2-aligned block size that can be represented by a standard
  // block size. If no size class for this exact size exist, the next larger size class is returned.
  // E.g. 8K => szclass_32K.
  static constexpr block_size_class_t pow2_block_size_to_standard_size_class(size_t block_size) {
    assert(is_power_of_2(block_size), "Block size must be a power of 2 (%zu)", block_size);
    assert(block_size <= max_standard_block_size, "Block size too large (%zu)", block_size);
    const unsigned log2_blocksize = constexpr_exact_log2(block_size);
    return log2_block_size_to_size_class_table[log2_blocksize];
  }

  // Returns the size class for a given block size that can be represented by a standard
  // block size. The block size does not have to be aligned.
  // If no size class for this exact size exist, the next larger size class is returned.
  // E.g. 9K => szclass_32K.
  static block_size_class_t unaligned_block_size_to_standard_size_class(size_t block_size) {
    assert(block_size <= max_standard_block_size, "Block size too large (%zu)", block_size);
    const size_t pow2 = round_up_power_of_2(block_size);
    return pow2_block_size_to_standard_size_class(pow2);
  }

  static constexpr bool is_valid_standard_size_class(block_size_class_t szclass) {
    return szclass <= highest_standard_size_class;
  }

  static constexpr bool is_valid_huge_size_class(block_size_class_t szclass) {
    return szclass == highest_size_class;
  }

  static constexpr bool is_valid_size_class(block_size_class_t szclass) {
    return szclass <= highest_size_class;
  }

  // Given a block size, returns true if its one of our valid standard block sizes
  // or the huge block granularity
  static constexpr bool is_valid_block_size(size_t blocksize) {
#define X(ignored, size, ignored2) if (blocksize == size) return true;
    ARENA_VALID_BLOCK_SIZES_DO(X)
#undef X
    return false;
  }

  static constexpr bool is_valid_standard_block_size(size_t blocksize) {
    return blocksize < block_size_2M && is_valid_block_size(blocksize);
  }

  static constexpr bool is_valid_huge_size(size_t blocksize) {
    return blocksize > 0u && blocksize < ArenaBlockHelpers::huge_block_max_size &&
           is_aligned(blocksize, huge_block_granularity);
  }

#ifdef ASSERT
  static constexpr void verify_valid_standard_block_size(size_t blocksize) {
    assert(is_valid_standard_block_size(blocksize), "Not a valid standard block size (%zu)", blocksize);
  }
  static constexpr void verify_valid_huge_size(size_t blocksize) {
    assert(is_valid_huge_size(blocksize), "Not a valid huge size (%zu)", blocksize);
  }
  static constexpr void verify_valid_block_size(size_t blocksize) {
    assert(is_valid_block_size(blocksize), "Not a valid block size (%zu)", blocksize);
  }
#endif // ASSERT

  // Todo: we can probably scrap that, we removed most usages
  template <typename function, bool forward, bool including_huge>
  static void iterate_size_classes_X_X(function f) {
    constexpr unsigned limit = including_huge ? num_size_classes : num_standard_size_classes;
    if (forward) {
      for (unsigned i = 0; i < limit; i++) {
        f(i);
      }
    } else {
      for (unsigned i = limit - 1; i < limit; i--) {
        f(i);
      }
    }
  }
  template <typename function>
  static void iterate_size_classes_forward(function f) { iterate_size_classes_X_X<function, true, true>(f); }
  template <typename function>
  static void iterate_size_classes_backward(function f) { iterate_size_classes_X_X<function, false, true>(f); }
  template <typename function>
  static void iterate_standard_size_classes_forward(function f) { iterate_size_classes_X_X<function, true, false>(f); }
  template <typename function>
  static void iterate_standard_size_classes_backward(function f) { iterate_size_classes_X_X<function, false, false>(f); }

#ifdef ASSERT
  static void verify_standard_size_class(block_size_class_t szclass) {
    assert(is_valid_standard_size_class(szclass), "Not a valid standard size class (%u)", szclass);
  }
  static void verify_huge_size_class(block_size_class_t szclass) {
    assert(is_valid_huge_size_class(szclass), "Not a valid huge size class (%u)", szclass);
  }
  static void verify_size_class(block_size_class_t szclass) {
    assert(is_valid_size_class(szclass), "Not a valid size class (%u)", szclass);
  }
#endif // ASSERT

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
    DEBUG_ONLY(verify_contains(p));
    return block_to_index(start_of_containing_block(p));
  }

  // Given a pointer, not necessarily aligned, that points into this range,
  // return the start address of the containing block
  const_blockptr_t start_of_containing_block(const_address p) const {
    DEBUG_ONLY(verify_contains(p));
    return align_down(p, _block_size);
  }

  IndexType block_to_index(const_blockptr_t b) const {
    DEBUG_ONLY(verify_is_valid_block(b));
    return block_to_index_without_assert(b);
  }

  IndexType block_to_index_incl(const_blockptr_t b) const {
    DEBUG_ONLY(verify_is_valid_block_incl(b));
    return block_to_index_without_assert(b);
  }

  IndexType block_to_index_without_assert(const_blockptr_t b) const {
    return checked_cast<IndexType>((p2u(b) - p2u(_base)) / _block_size);
  }

#ifdef ASSERT
  void verify_contains(const_address p) const {
    assert(contains(p), "Pointer out of range: " PTR_FORMAT, p2i(p));
  }
  void verify_contains_incl(const_address p) const {
    assert(contains_incl(p), "Pointer out of range: " PTR_FORMAT, p2i(p));
  }
  void verify_is_valid_block(const_blockptr_t b) const {
    assert(is_valid_block(b), "Block pointer invalid: " PTR_FORMAT, p2i(b));
  }
  void verify_is_valid_block_incl(const_blockptr_t b) const {
    assert(is_valid_block_incl(b), "Block pointer invalid: " PTR_FORMAT, p2i(b));
  }
  void verify_is_valid_block_index(IndexType block_index) const {
    assert(is_valid_block_index(block_index), "Block index invalid: %zu", checked_cast<uintptr_t>(block_index));
  }
  void verify_is_valid_block_index_incl(IndexType num) const {
    assert(is_valid_block_index_incl(num), "Invalid number of blocks: %zu", checked_cast<uintptr_t>(num));
  }
#endif
};

// Little helper; allocate a buffer, preferably inline,
// otherwise on C-heap
template <typename T, size_t max_inline_size>
class LittleBuffer {
  T _inline_buffer[max_inline_size];
  const size_t _len;
  T* const _start;
public:
  explicit LittleBuffer(size_t len) :
    _len(len),
    _start(len > max_inline_size ? NEW_C_HEAP_ARRAY(T, len, mtInternal) : _inline_buffer)
  {}
  ~LittleBuffer() {
    if (_start != _inline_buffer) {
      FREE_C_HEAP_ARRAY(_start);
    }
  }
  T* buffer() const { return _start; }
};

#endif // SHARE_MEMORY_ARENA_ARENABLOCK_HPP

