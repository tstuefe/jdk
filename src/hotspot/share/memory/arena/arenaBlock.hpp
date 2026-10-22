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

// We have 8 standard size classes: 256, 1K, 32K, 64K, ... 1M
// Anything beyond that is a huge block and var-sized.

struct ArenaSizeClassHelpers : public AllStatic {

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



#endif // SHARE_MEMORY_ARENA_ARENABLOCK
