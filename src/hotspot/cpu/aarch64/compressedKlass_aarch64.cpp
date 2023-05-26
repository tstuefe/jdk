/*
 * Copyright (c) 2021 SAP SE. All rights reserved.
 * Copyright (c) 2021, Red Hat Inc. All rights reserved.
 * Copyright (c) 2021, Oracle and/or its affiliates. All rights reserved.
 *
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

#include "precompiled.hpp"

#include "immediate_aarch64.hpp"
#include "macroAssembler_aarch64.hpp"
#include "oops/compressedKlass.hpp"
#include "compressedKlass_aarch64.hpp"
#include "utilities/ostream.hpp"
#include "runtime/globals.hpp"


class Quads {
  const uint16_t _imm16_q2;
  const uint16_t _imm16_q3;
public:
  Quads(uint64_t x)
    : _imm16_q2(x >> 32),
      _imm16_q3(x >> 48)
  {}

  uint16_t q2() const { return _imm16_q2; }
  uint16_t q3() const { return _imm16_q3; }

  uint64_t v() const {
    return (((uint64_t)q2()) << 32) +
           (((uint64_t)q3()) << 48);
  }
};


class MovKParameters {

  const uint64_t _kr1;
  const uint64_t _kr2;
  const int _shift;

  const Quads _quads;

public:

  MovKParameters(uint64_t kr1, uint64_t kr2, int shift, int num_base_quadrants)
    : _kr1(kr1), _kr2(kr2), _shift(shift),
      _quads(kr1 >> shift)
  {}

  // Returns true if the encoding covers the whole range
  // between [kr1..kr2)
  bool has_full_coverage() const {

    const uint64_t the_base

  }

};


class MovKBaseSegments {

  const uint16_t _imm16_q2;
  const uint16_t _imm16_q3;

  static uint16_t extract(uint64_t x, int quadrant) {
    return (x >> (quadrant * 16)) & 0xFFFF;
  }

public:

  // Discard the lower 32 bit, store upper 32bit as 16 bit units
  MovKBaseSegments(uint64_t x)
    : _imm16_q2(extract(x, 2)), _imm16_q3(extract(x, 3))
  {}

  uint64_t base() const {
    return (((uint64_t)_imm16_q2) << 32) |
           (((uint64_t)_imm16_q3) << 48);
  }

  int num_movk_operations() const {
    return (_imm16_q2 > 0 ? 1 : 0) +
           (_imm16_q3 > 0 ? 1 : 0);
  }

};


static bool CompressedKlassPointers::attempt_initialize_for_movk(address klass_range_start, size_t klass_range_len) {

  // Given a klass range, find the combination of base + shift that allows us to encode the base
  // with as few operations as possible.

  assert(is_aligned(klass_range_start, LogKlassAlignmentInBytes), "sanity");

  const address KR1 = klass_range_start;
  const address KR2 = align_down(KR1 + klass_range_len, KlassAlignmentInBytes);

  uint64_t candidate_base = 0;
  int candidate_shift = -1;

  for (int shift = 0; shift <= LogKlassAlignmentInBytes; shift ++) {

    const MovKBaseSegments helper(((uint64_t)KR1) >> shift);
    const address base = (address)(helper.base() << shift);

    assert(base <= KR1, "Sanity");

    // Valid base + shift?
    const size_t encoding_range_size = nth_bit(NarrowKlassPointerBits + shift);
    if ((base + encoding_range_size) >= KR2) {

      // Yes, valid (covers the whole klass range)
      // Is this better than the last encoding we found?
      const int rating_this = helper.num_movk_operations() + ((shift > 0) ? 1 : 0);
      const int rating_last = MovKBaseSegments(candidate_base).num_movk_operations() + ((candidate_shift > 0) ? 1 : 0);

      if (rating_this < rating_last) {
        candidate_base = helper.base();

      }

    }



    if (

    const uint64_t base_masks[] = { ~right_n_bits(32), ~right_n_bits(48) };
    const int base_mask_combos = 2;

    for (uint64_t base_mask = )

    const uint64_t highest_nklass_value_left_shifted = (((uint64_t)highest_klass_address) - klass_range_start)
    const uint64_t highest_nklass_value = highest_nklass_value_left_shifted >> shift;

    // Unshifted movk?
    // (decode: left-shift nklass, then movk unshifted base)
    {
      const uint64_t base_mask = movk_base_mask(highest_nklass_value_left_shifted);
      uint64_t candidate_base = ((uint64_t)klass_range_start) & base_mask;

      // Does this cover the whole range?




    // base quadrant combos (which quadrants we allow to be part of the base address)
    // - We leave out q0.
    // - A movk to q1 is theoretically possible for very small class spaces and large shifts. It would give us
    //   64K classes, with e.g. a 9-bit shift this would cover 32MB of class space. Maybe interesting for micro VMs?
    // - movk to q2 is the standard
    // - movk to q3 is possible if we run on a kernel with 52 bit address space, where virtual addresses can have
    //   address bits set beyond bit 47.
    const uint64_t valid_quadrant_combos[] = {
        // needs 1 movk
        (right_n_bits(16) << 32), // q2 only
        (right_n_bits(16) << 48), // q3 only
        (right_n_bits(16) << 16), // q1 only
        // Needs 2 movks
        (right_n_bits(16) << 32) + (right_n_bits(16) << 48), // q2 + q3
        (right_n_bits(16) << 32) + (right_n_bits(16) << 16), // q2 + q1
        (right_n_bits(16) << 48) + (right_n_bits(16) << 16), // q3 + q1
        0
    };

    for (int combo = 0; valid_quadrant_combos[combo] != 0; combo++) {
      const uint64_t mask = valid_quadrant_combos[combo];
      const int log2_offset_part = count_trailing_zeros(mask);
      const size_t value_range_offset =
      if ()
      if (klass_range_start_ui64)

    }

      uint64_t possible_base = klass_range_start_ui64 >> shift;

      const uint64_t mask = 0;
      if (quadrantcombo & 1) {
        mask |= right_n_bits(16) << 16;
      }
      if (quadrantcombo & 1) {
        mask |= right_n_bits(16) << 32;
      }
      if (quadrantcombo & 1) {
        mask |= right_n_bits(16) << 48;
      }
      possible_base &=


      const uint64_t possible_base = ((uint64_t) klass_range_start) & mask;
      if (possible_base < klass_range_start_ui64) { // no bits set in upper quadrants

      }
    }

    for (int )

    // q3 alone?
    uint64_t base_extracted =

    for (quadrant )

    if ()


    // With pre-shifted base?

    const address =

    // With unshifted base?

    const size_t resulting_encoding_range_len = nth_bit(shift + NarrowKlassPointerBits);

    MovKHelper h(((uint64_t)klass_range_start), shift);

    address candidate_base = (address)h.unshifted_base();


  }




}


// Given the future klass range, decide encoding.
void CompressedKlassPointers::initialize_pd(address klass_range_start, size_t klass_range_len) {

  // Get the simple cases out of the way first:
  address klass_range_end = klass_range_start + klass_range_len;

  // Zero base, zero shift?
  if (klass_range_end <= (address)(NarrowKlassPointerValueRange)) {
    initialize_raw(nullptr, 0);
    _pd._mode = CompressedKlassPointerSettings_PD::Mode::KlassDecodeZero;
    return;
  }

  // Zero base, maximum shift?
  if (klass_range_end <= (address)(NarrowKlassPointerValueRange << LogKlassAlignmentInBytes)) {
    initialize_raw(nullptr, LogKlassAlignmentInBytes);
    _pd._mode = CompressedKlassPointerSettings_PD::Mode::KlassDecodeZero;
    return;
  }

  // Calculate the smallest shift that would cover the whole Klass range.
  // If we are lucky, this is 0 and we won't have to shift.
  const int klass_range_len_log2 = exact_log2(next_power_of_2(klass_range_len));
  const int minimal_shift = MAX2(0, klass_range_len_log2 - NarrowKlassPointerBits);

  // Xor mode?

  for (int candidate_shift = minimal_shift; candidate_shift <= LogKlassAlignmentInBytes; candidate_shift++) {

    const size_t encoding_range_len = nth_bit(candidate_shift + NarrowKlassPointerBits);

    // Unshifted XOR mode?
    // decode: XOR *unshifted* base to *left-shifted* nKlass
    {
      const int base_trailing_zero_bits = NarrowKlassPointerBits + candidate_shift; // to be able to xor the unshifted base to the left-shifted nKlass
      const size_t base_alignment = nth_bit(base_trailing_zero_bits);
      address candidate_base = (address)calculate_next_lower_logical_immediate_matching((uint64_t)klass_range_start, base_alignment);
      if (candidate_base != nullptr) {
        // counter-check result
        assert(is_aligned(candidate_base, base_alignment), "Sanity");
        assert(candidate_base <= klass_range_start, "Sanity");
        // Does this base let us cover the whole klass range? If yes, we found a match.
        if ((candidate_base + encoding_range_size) >= klass_range_end) {
          initialize_raw(candidate_base, candidate_shift);
          _pd._mode = CompressedKlassPointerSettings_PD::Mode::KlassDecodeXor;
          _pd._rightshift_base = false;
          return;
        }
      }
    }

    // Shifted XOR mode?
    // decode: XOR *right-shifted* base to *unshifted* nKlass, then left-shift
    {
      const int base_trailing_zero_bits = NarrowKlassPointerBits; // to be able to xor the right-shifted base to the unshifted nKlass
      const size_t base_alignment = nth_bit(base_trailing_zero_bits);

      // We want a base, that, when right shifted, is a valid immediate that can be XORed to the unshifted nKlass:
      const uint64_t candidate_base_rightshifted =
          calculate_next_lower_logical_immediate_matching((uint64_t)klass_range_start >> candidate_shift, base_alignment);
      if (candidate_base_rightshifted != nullptr) {
        assert(is_aligned(candidate_base_rightshifted, base_alignment), "Sanity");
        address candidate_base = (address)(candidate_base_rightshifted << candidate_shift);
        assert(candidate_base <= klass_range_start, "Sanity");
        // Does this base let us cover the whole klass range? If yes, we found a match.
        if ((candidate_base + encoding_range_size) >= klass_range_end) {
          initialize_raw(candidate_base, candidate_shift);
          _pd._mode = CompressedKlassPointerSettings_PD::Mode::KlassDecodeXor;
          _pd._rightshift_base = true;
          return;
        }
      }
    }
  }

  // Still here? Try MovK mode.

  MovKHelper best_so_far;
  uint16_t imm16;

  if (minimal_shift == 0) {

    movk_base_parts


    if (klass_range_start  )

  }

  // try max shift


}

// Given a memory range [addr, addr + len) to be encoded (future Klass location range), propose
// encoding.
bool CompressedKlassPointers::propose_encoding_pd(address addr, size_t len, address* base, int* shift) {

  enum {
    zero,                     // mov,
    movk_unshifted_base_q3,   // decode: shift left, then movk the raw base into the third quadrant
    movk_unshifted_base_q4,   // decode: shift left, then movk the raw base into the fourth quadrant,
    movk_shifted_base_q3,     // decode: movk the pre-right-shifted base into the third quadrant, then shift left
    movk_shifted_base_q4,     // decode: movk the pre-right-shifted base into the fourth quadrant, then shift left
    unknown
  } usemode = unknown;

  address proposed_base = nullptr;
  int proposed_shift = -1;
  bool found = false;

  // Get the simple cases out of the way first:

  // nKlass == Klass*
  if (addr + len <= (address)(NarrowKlassPointerValueRange)) {
    proposed_base = nullptr;
    proposed_shift = 0;
    found = true;
  }

  // base can be zero
  if (!found &&
      addr + len <= (address)(nth_bit(NarrowKlassPointerValueRange << LogKlassAlignmentInBytes))) {
    proposed_base = nullptr;
    proposed_shift = LogKlassAlignmentInBytes;
    found = true;
  }

  /// Movk?

  enum {
    movk_unshifted_base_q3, // decode: shift left, then movk the raw base into the third quadrant
    movk_unshifted_base_q4, // decode: shift left, then movk the raw base into the fourth quadrant,
    movk_shifted_base_q3,   // decode: movk the pre-right-shifted base into the third quadrant, then shift left
    movk_shifted_base_q4,   // decode: movk the pre-right-shifted base into the fourth quadrant, then shift left
    unknown
  } usemode = unknown;

  // Do we have a base that can be used unshifted with movk in the third quadrant?
  if (!found && addr < (address)nth_bit(48)) {

    address candidate_base = align_down(addr, nth_bit(32));
    assert(candidate_base <= addr, "Unexpected");
    assert(((uintptr_t)candidate_base & 0xFFFF0000FFFFFFFFULL) == 0, "Sanity");

    // can we use this base with zero shift?
    if ((candidate_base + NarrowKlassPointerValueRange) >= (addr + len)) {
      proposed_base = candidate_base;
      proposed_shift = 0;
      usemode = movk_unshifted_base_q3;
      found = true;
    }

    // can we use this base with non-zero shift?
    else if ((candidate_base + (NarrowKlassPointerValueRange << LogKlassAlignmentInBytes)) >= (addr + len)) {
      proposed_base = candidate_base;
      proposed_shift = LogKlassAlignmentInBytes;
      usemode = movk_unshifted_base_q3;
      found = true;
    }

  }

  // Do we have a base that can be used unshifted with movk in the forth quadrant?
  else {

    address candidate_base = align_down(addr, nth_bit(48));
    assert(candidate_base <= addr, "Unexpected");
    assert(((uintptr_t)candidate_base & 0x0000FFFFFFFFFFFFULL) == 0, "Sanity");

    // can we use this base with zero shift?
    if ((candidate_base + NarrowKlassPointerValueRange) >= (addr + len)) {
      proposed_base = candidate_base;
      proposed_shift = 0;
      usemode = movk_unshifted_base_q4;
      found = true;
    }

    // can we use this base with non-zero shift?
    else if ((candidate_base + (NarrowKlassPointerValueRange << LogKlassAlignmentInBytes)) >= (addr + len)) {
      proposed_base = candidate_base;
      proposed_shift = LogKlassAlignmentInBytes;
      usemode = movk_unshifted_base_q4;
      found = true;
    }
  }


  // Can we use


  address candidate_base = nullptr;
  for (candidate_shift = 0; candidate_shift < LogKlassAlignmentInBytes; candidate_shift++) {

    // Iterate through all possible shifts that would Find smallest shift that works for the given to-be-encoded range len
    if (nth_bit(NarrowKlassPointerBits + candidate_shift) > len) {

      // For the candidate shift, find a base that works as immediate for movk
      // (either shifted by the prospective shift, or unshifted).
      candidate_base = align_down(addr, nth_bit(32));



    }

  }


  enum mode { movk_unshifted_base, movk_shifted_base, unknown } mode = unknown;

  // Can we find a base that is expressable as 16bit << 32 for movk unshifted_base mode?
  address candidate_base = align_down(addr, nth_bit(32));

  int base_movk_shift = 0; // either 32 or 48 (the latter only for 52-bit address spaces)

  if (candidate_)


  // We may run on a kernel that allows >48bit address space, in which case the uppermost 16 bits
  // of the address could contain non-zero bits. In that case, we cannot use a single movk.
  if ()



  // Can we use Xor mode?


  else if (len < NarrowKlassPointerValueRange ) {
    // shift can be zero
  }


}


// Given an address p, return true if p can be used as an encoding base.
//  (Some platforms have restrictions of what constitutes a valid base address).
bool CompressedKlassPointers::is_valid_base(address p) {
  return MacroAssembler::klass_decode_mode_for_base(p) != MacroAssembler::KlassDecodeNone;
}

void CompressedKlassPointers::print_mode_pd(outputStream* st) {
  st->print_cr("Narrow klass base: " PTR_FORMAT ", Narrow klass shift: %d, "
               "Narrow klass range: " UINT64_FORMAT_X
               ", Encoding mode %s",
               p2i(base()), shift(), KlassEncodingMetaspaceMax,
               MacroAssembler::describe_klass_decode_mode(MacroAssembler::klass_decode_mode()));
}

