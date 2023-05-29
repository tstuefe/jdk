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

CompressedKlassPointerSettings_PD::CompressedKlassPointerSettings_PD()
  : _base(nullptr), _shift(-1), _mode(Mode::KlassDecodeNone),
    _do_rshift_base(false),
    _movk_imm16_q1(0), _movk_imm16_q2(0), _movk_imm16_q3(0)
{}

//// Zero mode /////////////

bool CompressedKlassPointerSettings_PD::attempt_initialize_for_zero(address kr2) {
  if (kr2 < nth_bit(NarrowKlassPointerBits + LogKlassAlignmentInBytes)) {
    _mode = Mode::KlassDecodeZero;
    _base = nullptr;
    _shift = (kr2 < nth_bit(NarrowKlassPointerBits)) ? 0 : LogKlassAlignmentInBytes;
    return true;
  }
  return false;
}

//// XOR mode /////////////

bool CompressedKlassPointerSettings_PD::attempt_initialize_for_xor(address kr1, address kr2) {

  // Calculate the smallest shift that we'd need to cover the whole Klass range, if base were kr1.
  const int klass_range_len_log2 = exact_log2(next_power_of_2(kr2 - kr1));
  const int minimal_shift = MAX2(0, klass_range_len_log2 - NarrowKlassPointerBits);


  // Find an immediate that gives us a valid encoding; start with minimal shift in the hope that its 0.
  // Since the form of the immediates - and their distance to kr1 and hence their encoding range -
  // are difficult to predict, just try all valid shift values.

  for (int candidate_shift = minimal_shift; candidate_shift <= LogKlassAlignmentInBytes; candidate_shift++) {

    const size_t encoding_range_len = nth_bit(candidate_shift + NarrowKlassPointerBits);

    // Unshifted XOR mode?
    //  (XOR *unshifted* base to *left-shifted* nKlass)
    {
      const int bits_offset = NarrowKlassPointerBits + candidate_shift; // left-shifted nKlass
      const size_t base_alignment = nth_bit(bits_offset);
      address candidate_base = (address)calculate_next_lower_logical_immediate_matching((uint64_t)kr1, base_alignment);
      if (candidate_base != nullptr) {
        assert(is_aligned(candidate_base, base_alignment), "Sanity");
        assert(candidate_base <= kr1, "Sanity");
        if ((candidate_base + encoding_range_len) >= kr2) {
          _mode = Mode::KlassDecodeXor;
          _do_rshift_base = false;
          _base = candidate_base;
          _shift = candidate_shift;
          return true;
        }
      }
    }

    // Shifted XOR mode?
    //  (decode: XOR *right-shifted* base to *unshifted* nKlass, then left-shift)
    {
      const int bits_offset = NarrowKlassPointerBits; // unshifted nKlass
      const size_t base_alignment = nth_bit(bits_offset);
      const uint64_t candidate_base_rightshifted =
          calculate_next_lower_logical_immediate_matching((uint64_t)kr1 >> candidate_shift, base_alignment);
      if (candidate_base_rightshifted != nullptr) {
        assert(is_aligned(candidate_base_rightshifted, base_alignment), "Sanity");
        address candidate_base = (address)(candidate_base_rightshifted << candidate_shift);
        assert(candidate_base <= kr1, "Sanity");
        if ((candidate_base + encoding_range_len) >= kr2) {
          _mode = Mode::KlassDecodeXor;
          _do_rshift_base = true;
          _base = candidate_base;
          _shift = candidate_shift;
          return true;
        }
      }
    }
  }
  return false;
}

//// MOVK mode /////////////

class MovKParameters {

  // Shift to use
  const int _shift;

  // Whether to apply the right-shifted base to the nKlass
  // or the unshifted base to the left-shifted nKlass
  const bool _do_rshift_base;

  // Base
  const Quads _quads;

  static uint64_t calc_clipped_base(address kr1, int shift, int num_base_quadrants, bool do_rshift_base) {
    uint64_t b = (uint64_t)kr1;
    if (do_rshift_base) {
      b >>= shift;
    }
    switch (num_base_quadrants) {
      case 1: b &= (right_n_bits(16) << 48); break;
      case 2: b &= (right_n_bits(32) << 32); break;
      case 3: b &= (right_n_bits(48) << 16); break;
      default: ShouldNotReachHere();
    }
    return b;
  }

public:

  MovKParameters() : _shift(0), _do_rshift_base(false), _quads(0) {}

  MovKParameters(address kr1, int shift, int num_base_quadrants, bool do_rshift_base)
    : _shift(shift), _do_rshift_base(do_rshift_base),
      _quads(calc_clipped_base(kr1, shift, num_base_quadrants, do_rshift_base))
  {}

  const Quads& quads() const { return _quads; }

  int shift() const               { return _shift; }
  bool needs_shift() const        { return shift() > 0; }

  bool do_rshift_base() const      { return _do_rshift_base; }

  address base_rshifted() const {
    assert(do_rshift_base(), "Only use for rshift-base-case");
    return (address)(quads().v());
  }

  address base_unshifted() const {
    return (address)(quads().v() << (do_rshift_base() ? shift() : 0));
  }

  // Returns true if the encoding covers the whole range between [kr1..kr2]
  bool covers_klass_range(address kr1, address kr2) const  {
    const uint64_t encoding_coverage = nth_bit(NarrowKlassPointerBits + shift());
    return (base_unshifted() <= kr1 &&
           (base_unshifted() + encoding_coverage) > kr2);
  }

  // Returns number of ops necessary
  int num_ops() const {
    return quads().num_quadrants_set() + (needs_shift() ? 1 : 0);
  }

};

bool CompressedKlassPointerSettings_PD::attempt_initialize_for_movk(address kr1, address kr2) {

  // Given a Klass range, find the combination of base + shift that allows us to encode the base
  // with as few operations as possible. Valid solutions are all that give us a base that is
  // encodable in either or both of q3 (bits 48-63) and q2 (bits 32-47).
  //
  // Notes:
  // - we test for q4 too since we may enconter klass range addresses that have bits set in the
  //   upper quadrant, if we run on a kernel that allows 52-bit addresses
  // - we test for q2 too since that allows us to work with nKlass bit sizes that are very small,
  //   e.g. 16.

  bool found = false;
  MovKParameters best_so_far;

  for (int shift = 0; shift <= LogKlassAlignmentInBytes; shift ++) {
    for (int num_base_quadrants = 1; num_base_quadrants <= 3; num_base_quadrants ++) {
      for (int do_rshift = 0; do_rshift < 2; do_rshift++) {
        MovKParameters here(kr1, shift, num_base_quadrants, do_rshift == 1);
        if (here.covers_klass_range(kr1, kr2)) {
          if (!found || (here.num_ops() < best_so_far.num_ops())) {
            best_so_far = here;
            found = true;
          }
        }
      }
    }
  }

  if (found) {
    assert(best_so_far.covers_klass_range(kr1, kr2), "Sanity");
    _mode = Mode::KlassDecodeMovk;
    _base = best_so_far.base_unshifted();
    _shift = best_so_far.shift();
    _do_rshift_base = best_so_far.do_rshift_base();
    _movk_imm16_quads = best_so_far.quads();
    return true;
  }
  return false;

}

bool CompressedKlassPointerSettings_PD::attempt_initialize(address kr1, address kr2) {
  // We prefer zero over xor over movk
  return
      attempt_initialize_for_zero(kr2) ||
      attempt_initialize_for_xor(kr1, kr2) ||
      attempt_initialize_for_movk(kr1, kr2);
}

// "reverse-initialize" from a given base and shift, for a given klass range (called for the CDS runtime path)
bool CompressedKlassPointerSettings_PD::attempt_initialize_for_fixed_base_and_shift(address base, int shift, address kr1, address kr2) {

  _mode = Mode::KlassDecodeNone;

  if (base == nullptr) {
    _mode = Mode::KlassDecodeZero;
    _base = base;
    _shift = shift;
  } else {
    if (Assembler::operand_valid_for_logical_immediate(false, base)) {
      _mode = Mode::KlassDecodeXor;
      _base = base;
      _shift = shift;
      _do_rshift_base = false;
    } else if (Assembler::operand_valid_for_logical_immediate(false, ((uint64_t)base) >> shift)) {
      _mode = Mode::KlassDecodeXor;
      _base = (address)(((uint64_t)base) >> shift);
      _shift = shift;
      _do_rshift_base = true;
    } else {

      bool unshifted_is_valid =



      // deduce number of base quadrants

      // For a given base and shift, search all possible combos for the best one that covers the klass range
      for (int num_base_quadrants = 1; num_base_quadrants <= 3; num_base_quadrants ++) {
        for (int do_rshift = 0; do_rshift < 2; do_rshift++) {
          MovKParameters here(base, shift, num_base_quadrants, do_rshift == 1);
          if (here.is_valid() && here.covers_klass_range(kr1, kr2)) {
            if (!found || best_so_far.num_ops() > here.num_ops()) {
              found = true;
              best_so_far = here;
            }
          }
        }
      }

      if (found) {
        assert(best_so_far.is_valid() && best_so_far.covers_klass_range(kr1, kr2), "Sanity");
        assert()
        _mode = Mode::KlassDecodeMovk;
        _base = best_so_far.base_unshifted();
        _shift = best_so_far.shift();
        _do_rshift_base = best_so_far.do_rshift_base();
        _movk_imm16_q1 = best_so_far.q1();
        _movk_imm16_q2 = best_so_far.q2();
        _movk_imm16_q3 = best_so_far.q3();
      }
    }

  }

  return _mode != Mode::KlassDecodeNone ? true : false;
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

