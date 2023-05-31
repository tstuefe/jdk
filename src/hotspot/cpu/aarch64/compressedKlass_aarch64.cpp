/*
 * Copyright (c) 2023, Red Hat Inc. All rights reserved.
 * Copyright (c) 2023, Oracle and/or its affiliates. All rights reserved.
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
#include "register_aarch64.hpp"

#include "oops/compressedKlass.hpp"
#include "compressedKlass_aarch64.hpp"
#include "utilities/ostream.hpp"
#include "runtime/globals.hpp"
#include "runtime/os.hpp"

static bool encoding_covers_range(address base, int shift, address kr1, address kr2) {
  return base <= kr1 &&
         (base + (nth_bit(NarrowKlassPointerBits + shift))) > kr2; // Note: kr2 included.
}

CompressedKlassPointerSettings_PD::CompressedKlassPointerSettings_PD()
  : _base(nullptr), _shift(-1), _mode(Mode::KlassDecodeNone),
    _do_rshift_base(false)
{}

//// Zero mode /////////////

bool CompressedKlassPointerSettings_PD::attempt_initialize_for_zero(address kr2) {
  for (int shift = 0; shift <= LogKlassAlignmentInBytes; shift++) {
    if (kr2 < (address)nth_bit(NarrowKlassPointerBits + shift)) {
      _mode = Mode::KlassDecodeZero;
      _base = nullptr;
      _shift = shift;
      return true;
    }
  }
  return false;
}

//// XOR mode /////////////

bool CompressedKlassPointerSettings_PD::attempt_initialize_for_xor(address kr1, address kr2) {

  // Find an immediate that gives us a valid encoding; start with minimal shift in the hope that its 0.
  // Since the form of the immediates - and their distance to kr1 and hence their encoding range -
  // are difficult to predict, just try all valid shift values.

  for (int candidate_shift = 0; candidate_shift <= LogKlassAlignmentInBytes; candidate_shift++) {
    const size_t encoding_range_len = nth_bit(candidate_shift + NarrowKlassPointerBits);

    // Ignore shift values that are obviously too small
    if (encoding_range_len < (size_t)(kr2 - kr1)) {
      continue;
    }

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
      if (candidate_base_rightshifted != 0) {
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

// Helper structure for movk mode
class Quads {
  const uint16_t _imm16_q1;
  const uint16_t _imm16_q2;
  const uint16_t _imm16_q3;
public:

  Quads(uint64_t x)
    : _imm16_q1(x >> 16),
      _imm16_q2(x >> 32),
      _imm16_q3(x >> 48)
  {}

  uint16_t q1() const { return _imm16_q1; }
  uint16_t q2() const { return _imm16_q2; }
  uint16_t q3() const { return _imm16_q3; }

  uint64_t v() const {
    return (((uint64_t)q2()) << 16) +
           (((uint64_t)q2()) << 32) +
           (((uint64_t)q3()) << 48);
  }

  int num_quadrants_set() const {
    int r = 0;
    if (q1() > 0) r++;
    if (q2() > 0) r++;
    if (q3() > 0) r++;
    return r;
  }
};

class MovKParameters : public CHeapObj<mtMetaspace> {

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

  MovKParameters()
    : _shift(0), _do_rshift_base(false), _quads(0) {}

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
    return encoding_covers_range(base_unshifted(), shift(), kr1, kr2);
  }

  // Returns number of ops necessary
  int num_ops() const {
    int r = quads().num_quadrants_set(); // one movk per quadrant, hopefully just one
    if (needs_shift()) {
      // for decode right-shifted base, if src != dst, we need a movw first
      r += _do_rshift_base ? 2 : 1;
    }
    return r;
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

  MovKParameters* best_so_far = nullptr;

  for (int shift = 0; shift <= LogKlassAlignmentInBytes; shift ++) {
    for (int num_base_quadrants = 1; num_base_quadrants <= 3; num_base_quadrants ++) {
      for (int do_rshift = 0; do_rshift < 2; do_rshift++) {
        MovKParameters* here = new MovKParameters(kr1, shift, num_base_quadrants, do_rshift == 1);
        MovKParameters* deletethis = here;
        if (here->covers_klass_range(kr1, kr2)) {
          if (best_so_far == nullptr || (here->num_ops() < best_so_far->num_ops())) {
            deletethis = best_so_far;
            best_so_far = here;
          }
        }
        delete deletethis;
      }
    }
  }

  if (best_so_far != nullptr) {
    assert(best_so_far->covers_klass_range(kr1, kr2), "Sanity");
    _mode = Mode::KlassDecodeMovk;
    _base = best_so_far->base_unshifted();
    _shift = best_so_far->shift();
    _do_rshift_base = best_so_far->do_rshift_base();
    delete best_so_far;
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

  if (!encoding_covers_range(base, shift, kr1, kr2)) {
    return false;
  }

  if (base == nullptr) {

    _mode = Mode::KlassDecodeZero;
    _base = base;
    _shift = shift;

  } else if (Assembler::operand_valid_for_logical_immediate(false, (uint64_t)base)) {

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

    // MOVK mode?

    const uint64_t base_unshifted = (uint64_t) base;
    const bool base_unshifted_xorable = ((base_unshifted & right_n_bits(NarrowKlassPointerBits + shift)) == 0);

    const uint64_t base_rshifted = ((uint64_t) base) >> shift;
    const bool base_rshifted_xorable = ((base_rshifted & right_n_bits(NarrowKlassPointerBits)) == 0);

    if (base_unshifted_xorable || base_rshifted_xorable) {
      // we can use movk. Now figure out whether rshifted or unshifted is better.
      _mode = Mode::KlassDecodeMovk;
      _shift = shift;
      if (!base_rshifted_xorable) {
        _do_rshift_base = false;
      } else if (!base_unshifted_xorable) {
        _do_rshift_base = true;
      } else {
        // Both work, chose the one with the fewer ops
        _do_rshift_base = Quads(base_unshifted).num_quadrants_set() > Quads(base_rshifted).num_quadrants_set();
      }
      _base = (address)(_do_rshift_base ? base_rshifted : base_unshifted);
    }
  }

  return _mode != Mode::KlassDecodeNone ? true : false;
}

///// Code generation /////////////

#define __ masm->

static void copy_nKlass_if_needed(MacroAssembler* masm, Register dst, Register src) {
  if (dst != src) {
    assert(NarrowKlassPointerBits <= 32, "Sanity");
    __ movw(dst, src);
  }
}

static void generate_movk_ops(MacroAssembler* masm, uint64_t base, Register dst) {
  Quads quads(base);
  if (quads.q1() > 0) {
    __ movk(dst, quads.q1(), 16);
  }
  if (quads.q2() > 0) {
    __ movk(dst, quads.q2(), 32);
  }
  if (quads.q3() > 0) {
    __ movk(dst, quads.q3(), 48);
  }
}

void CompressedKlassPointerSettings_PD::decode_klass_not_null_for_zero(MacroAssembler* masm, Register dst, Register src) const {
  assert(_base == nullptr, "Sanity");
  if (_shift == 0) {
    copy_nKlass_if_needed(masm, dst, src);
  } else {
    __ lsl(dst, src, _shift);
  }
}

void CompressedKlassPointerSettings_PD::decode_klass_not_null_for_xor(MacroAssembler* masm, Register dst, Register src) const {
  assert(_base != nullptr, "Sanity");
  const uint64_t base_ui64 = (uint64_t)_base;
  const uint64_t base_ui64_rshifted = ((uint64_t)_base) >> _shift;

  if (_shift == 0) {
    __ eor(dst, src, base_ui64);
  } else {
    if (_do_rshift_base) {
      __ eor(dst, src, base_ui64_rshifted);
      __ lsl(dst, dst, _shift);
    } else {
      __ lsl(dst, src, _shift);
      __ eor(dst, dst, base_ui64);
    }
  }
}

void CompressedKlassPointerSettings_PD::decode_klass_not_null_for_movk(MacroAssembler* masm, Register dst, Register src) const {
  assert(_base != nullptr, "Sanity");
  const uint64_t base_ui64 = (uint64_t)_base;
  const uint64_t base_ui64_rshifted = ((uint64_t)_base) >> _shift;

  if (_shift == 0) {
    copy_nKlass_if_needed(masm, dst, src);
    generate_movk_ops(masm, base_ui64, dst);
  } else {
    if (_do_rshift_base) {
      copy_nKlass_if_needed(masm, dst, src);
      generate_movk_ops(masm, base_ui64_rshifted, dst);
      __ lsl(dst, dst, _shift);
    } else {
      __ lsl(dst, src, _shift);
      generate_movk_ops(masm, base_ui64, dst);
    }
  }
}

void CompressedKlassPointerSettings_PD::decode_klass_not_null(MacroAssembler* masm, Register dst, Register src) const {
  assert(UseCompressedClassPointers, "should only be used for compressed headers");
  switch (_mode) {
  case Mode::KlassDecodeZero:
    decode_klass_not_null_for_zero(masm, dst, src);
    break;
  case Mode::KlassDecodeXor:
    decode_klass_not_null_for_xor(masm, dst, src);
    break;
  case Mode::KlassDecodeMovk:
    decode_klass_not_null_for_movk(masm, dst, src);
    break;
  default:
    ShouldNotReachHere();
  }
}

void CompressedKlassPointerSettings_PD::encode_klass_not_null(MacroAssembler* masm, Register dst, Register src) const {
  assert(UseCompressedClassPointers, "should only be used for compressed headers");
  switch (_mode) {
  case Mode::KlassDecodeZero:
    if (_shift == 0) {
      // nKlass == Klass*
      copy_nKlass_if_needed(masm, dst, src);
    } else {
      __ lsr(dst, src, _shift);
    }
    break;
  case Mode::KlassDecodeXor:
  case Mode::KlassDecodeMovk:
    __ ubfx(dst, src, _shift, NarrowKlassPointerBits);
    break;
  default:
    ShouldNotReachHere();
  }
}

void CompressedKlassPointerSettings_PD::print_on(outputStream* st) const {
  // Don't print base, shift, since those are printed at the caller already
  const char* const modes[] = { "none", "zero", "xor", "movk" };
  const int modei = (int)_mode;
  assert(modei >= 0 && modei < 4, "Sanity");
  st->print_cr("Encoding Mode: %s", modes[modei]);
  if (_mode == Mode::KlassDecodeMovk || _mode == Mode::KlassDecodeXor) {
    st->print_cr("Rshifted base: %d", _do_rshift_base);
  }
}

// attempt to reserve a memory range well suited to compressed class encoding
address CompressedKlassPointerSettings_PD::reserve_klass_range(size_t len) {
  assert(is_aligned(len, os::vm_allocation_granularity()), "Sanity");

  address result = nullptr;

  // Fallback case on aarch64:
  //
  // Any address with lower 32 bits all zero can be used as a base for rshift MOVK mode
  // (as long as NarrowKlassPointerBits <= 32)
  assert(NarrowKlassPointerBits <= 32, "Sanity");
  result = (address)os::reserve_memory_aligned(len, nth_bit(32), false);
  assert(p2i(result) & right_n_bits(32) == 0, "Sanity");

  return result;
}
