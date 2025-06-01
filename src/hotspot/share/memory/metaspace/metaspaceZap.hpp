/*
 * Copyright (c) 2025 Red Hat, Inc. All rights reserved.
 * Copyright (c) 2025, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_MEMORY_METASPACE_ZAP_HPP
#define SHARE_MEMORY_METASPACE_ZAP_HPP

#include "memory/allStatic.hpp"
#include "memory/metaspace/metablock.hpp"
#include "utilities/align.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"

#if defined(ASSERT) && defined(_LP64)

namespace metaspace {

class Zapper : public AllStatic {
public:
  // canary indicates free metaspace memory
  static constexpr uint64_t metaspace_zap =  0x4D4554414154454DULL; // "METAATEM"

  // canary to use for initializing allocated metadata (to trip over missing member initialization)
  static constexpr uint64_t metaspace_uninitialized =  0x6D6574616174656DULL; // "metaatem"

private:
  // We zap a range of memory with an alternating pattern of the pure zap value and
  // a salted zap value.
  // The salted zap value is computed by xor'ing the pure zap value twice: once with a
  // random salt that is generated on VM start, once with its address location (guarding
  // against accidental misidentification of copied memory; metaspace objects are address
  // stable).
  //
  // We do this to minimize the chance of false positives to an infinitesimally small
  // chance. We can live with false negatives.

  static uint64_t _salt;

  static inline uint64_t salted_value(const uint64_t pure, uint64_t salt) {
    return pure ^ salt;
  }

  static inline uint64_t desalted_value(const uint64_t salted, uint64_t salt) {
    return salted ^ salt;
  }

  static inline bool is_alternate_location(const uint64_t* p) {
    return (uintptr_t)p & (1 << LogBytesPerWord);
  }

  static inline uint64_t location_salt(const uint64_t* p) {
    return (uint64_t)p;
  }

  static inline bool is_salted_location_interleaving(const uint64_t* p) {
    if (is_alternate_location(p)) {
      return (*p) == desalted_value(location_salt(p), metaspace_zap);
    } else {
      return (*p) == metaspace_zap;
    }
  }

  static inline void do_salt_location_interleaving(uint64_t* p) {
    (*p) = is_alternate_location(p) ?
        salted_value(location_salt(p), metaspace_zap) :
        metaspace_zap;
  }

  static inline void assert_range(MetaWord* from, MetaWord* to) {
    assert(is_aligned(from, BytesPerWord) && is_aligned(to, BytesPerWord),
           "range not word aligned? " RANGEFMT, RANGE2FMTARGS(from, to));
    assert(from < to, "Zero or negative range? " RANGEFMT, RANGE2FMTARGS(from, to));
  }

public:

  static void initialize();

  static inline void zap_range(MetaWord* from, MetaWord* to) {
    assert_range(from, to);
    for (MetaWord* p = from; p < to; p++) {
      do_salt_location_interleaving((uint64_t*)p);
    }
  }

  static inline void zap_range(MetaWord* from, size_t word_size) {
    zap_range(from, from + word_size);
  }

  static inline void zap_metablock(MetaBlock blk) {
    zap_range(blk.base(), blk.end());
  }

  static inline bool is_zapped_location(const MetaWord* p) {
    return is_salted_location_interleaving((uint64_t*)p);
  }

  // Given a pointer to a header hdr leading an area of word_size words, returns true if the area following
  // the header is fully zapped.
  template <class HEADER>
  static inline void zap_range_with_header(HEADER* hdr, size_t word_size) {
    STATIC_ASSERT(is_aligned(sizeof(HEADER), BytesPerWord));
    constexpr size_t hdr_word_size = sizeof(HEADER) / BytesPerWord;
    assert(word_size >= hdr_word_size, "Sanity");
    if (word_size > hdr_word_size) {
      assert(is_aligned(hdr, BytesPerWord), "Sanity");
      MetaWord* range_start = (MetaWord*)(hdr + 1);
      const size_t range_size = word_size - hdr_word_size;
      zap_range(range_start, range_size);
    }
  }

  // Given a pointer p and a word size, returns the number of consecutive zapped words
  // found at that location.
  static inline size_t num_zapped_words_at(const MetaWord* p, size_t word_size) {
    size_t result = 0;
    while (result < word_size && is_zapped_location(p + result)) {
      result ++;
    }
    return result;
  }

  // Two pairs of salted/non-salted
  static constexpr size_t min_significance = 4;

  // Given a location p, returns true if it had been zapped with
  // at least min_significance number of words.
  static inline bool location_looks_zapped(const MetaWord* p, size_t word_size) {
    assert(is_aligned(p, BytesPerWord), "Sanity");
    if (word_size < min_significance) {
      return false; // too small to say for sure
    }
    return num_zapped_words_at(p, min_significance) == min_significance;
  }

  // Given a pointer p and a word size, returns true if the full range is zapped.
  // Returns false if not fully zapped, and the position of the first non-zapped
  // word. Used for checking zapped memory for overwriters.
  static inline bool range_is_fully_zapped(const MetaWord* start, size_t word_size, size_t& first_nonzapped) {
    constexpr size_t portion = 4;
    if (word_size <= portion * 2) {
      first_nonzapped = num_zapped_words_at(start, word_size);
      return word_size == first_nonzapped;
    } else {
      // Large range; we just check at certain intervals
      constexpr int interval = 32;
      const MetaWord* const end = start + word_size - portion;
      bool found_nonzapped = false;
      for (const MetaWord* scanpoint = start; scanpoint < end && !found_nonzapped; scanpoint += interval) {
        found_nonzapped = num_zapped_words_at(scanpoint, portion) != portion;
      }
      if (!found_nonzapped) { // Also scan the very end
        found_nonzapped = num_zapped_words_at(end, portion) != portion;
      }
      if (found_nonzapped) { // found something off. Do full scan to find first non-zapped position.
        first_nonzapped = num_zapped_words_at(start, word_size);
        return false;
      }
    }
    return true;
  }

  // Given a pointer to a header hdr leading an area of word_size words, returns true if the area following
  // the header is fully zapped.
  template <class HEADER>
  static inline bool range_with_header_is_fully_zapped(const HEADER* hdr, size_t word_size, size_t& first_nonzapped) {
    STATIC_ASSERT(is_aligned(sizeof(HEADER), BytesPerWord));
    constexpr size_t hdr_word_size = sizeof(HEADER) / BytesPerWord;
    assert(word_size >= hdr_word_size, "Sanity");
    assert(is_aligned(hdr, BytesPerWord), "Sanity");
    const MetaWord* range_start = (const MetaWord*)(hdr + 1);
    const size_t range_size = word_size - hdr_word_size;
    if (!range_is_fully_zapped(range_start, range_size, first_nonzapped)) {
      first_nonzapped += hdr_word_size;
      return false;
    }
    return true;
  }

  // uninitialized marking is different from zapping. Zapping is for marking free memory,
  // uninitialized marking is for initializing memory with a non-zero pattern after allocation
  // to remove the zap pattern and to flush out uses of-uninitialized-members.
  static inline void mark_range_uninitialized(MetaWord* from, MetaWord* to) {
    assert_range(from, to);
    for (MetaWord* p = from; p < to; p++) {
      (*(uint64_t*)p) = metaspace_uninitialized;
    }
  }

  static inline void mark_range_uninitialized(MetaWord* from, size_t word_size) {
    mark_range_uninitialized(from, from + word_size);
  }

  static inline void mark_metablock_uninitialized(MetaBlock blk) {
    mark_range_uninitialized(blk.base(), blk.end());
  }

};

#else

struct Zapper {
  static inline void initialize() {}
  static inline void zap_range(MetaWord* from, MetaWord* to) {}
  static inline void zap_range(MetaWord* from, size_t word_size) {}
  static inline void zap_metablock(MetaBlock blk) {}
  static inline bool is_zapped_location(const MetaWord* p) {}
  static inline size_t num_zapped_words_at(const MetaWord* p, size_t word_size) {}
  static inline bool is_fully_zapped(const MetaWord* start, size_t word_size, size_t& first_nonzapped) {}
  template <class HEADER>
  static inline inline bool is_fully_zapped_with_header(const HEADER* hdr, size_t word_size, size_t& first_nonzapped) {}
  static inline void mark_range_uninitialized(MetaWord* from, MetaWord* to) {}
  static inline void mark_range_uninitialized(MetaWord* from, size_t word_size) {}
  static inline void mark_metablock_uninitialized(MetaBlock blk) {}
  static inline void mark_metablock_uninitialized(MetaBlock blk) {}
};

#endif // defined(ASSERT) && defined(_LP64)

} // namespace metaspace

#endif // SHARE_MEMORY_METASPACE_ZAP_HPP
