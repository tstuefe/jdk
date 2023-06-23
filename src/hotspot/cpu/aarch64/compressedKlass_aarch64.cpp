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

CompressedKlassPointerSettings_PD::CompressedKlassPointerSettings_PD()
  : _base(nullptr), _shift(-1), _mode(Mode::KlassDecodeNone),
    _movk_unshifted_base(false)
{}

//// Zero mode /////////////

// Given the end of the klass range, find the lowest value of shift that works with a zero base.
// If none work, return false.
bool CompressedKlassPointerSettings_PD::attempt_initialize_for_zero(address kr2) {
  const uint64_t high_address_log2 = exact_log2(next_power_of_2(p2i(kr2)));
  if (high_address_log2 <= (NarrowKlassPointerBits + LogKlassAlignmentInBytes)) {
    _mode = Mode::KlassDecodeZero;
    _base = nullptr;
    _shift = (high_address_log2 > NarrowKlassPointerBits) ?
              high_address_log2 - LogKlassAlignmentInBytes : 0;
    return true;
  }
  return false;
}

//// MOVK mode /////////////

// Helper structure for movk mode
class Quads {
  union {
    uint64_t i64;
    struct {
      uint16_t q[4];
    } i16;
  } _v;
public:

  Quads(uint64_t x) {
    _v.i64 = x;
  }

  void mask_lower_bits(int bits) { _v.i64 &= ~right_n_bits(bits); }
  void shl(int bits) { _v.i64 <<= bits; }
  void shr(int bits) { _v.i64 >>= bits; }

  uint64_t v() const { return _v.i64; }
  uint16_t q(int quadrant) const { return _v.i16[quadrant]; }

  int num_quadrants_set() const {
    int r = 0;
    if (q(0) > 0) r++;
    if (q(1) > 0) r++;
    if (q(2) > 0) r++;
    if (q(3) > 0) r++;
    return r;
  }
};

class MovkSetting {
  uint64_t _base;
  int _shift;
  bool _unshifted_base;
public:
  MovkSetting(uint64_t base, int shift, bool unshifted_base) : _q(base), _shift(shift), _unshifted_base(unshifted_base) {}

  bool valid() const {
    if (_base & right_n_bits(NarrowKlassPointerBits + (_unshifted_base ? _shift : 0))) {
      return false;
    }

  }

  int weight() const {
    int rc = Quads(_base).num_quadrants_set();
    rc += (_unshifted_base ? 0 : 1);
    return rc;
  }
};

// returns weight, the lower the better. returns -1 if this is an invalid combination.
static int weight_movk_setting(address kr1, address kr2, address base, int shift, bool unshifted_base) {
  Quads quads(p2i(base));
  address enc_range_end = base + nth_bit(NarrowKlassPointerBits + shift);
  if (enc_range_end >= kr2) {

  }
}


bool CompressedKlassPointerSettings_PD::attempt_initialize_for_movk(address kr1, address kr2) {

  // Given an address range (that may start at a nicely aligned address, but could be
  // located wherever), find the best combination of movk parameters.

  // This is hard because:
  // - the range of valid shift parameters depends on the distance between encoding base and kr2
  // - the encoding base alignment depends on the shift parameter

  // Here, we just keep it simple and try all combinations.
  for (bool unshifted_base =



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
  case Mode::KlassDecodeMovk:
    __ ubfx(dst, src, _shift, NarrowKlassPointerBits);
    break;
  default:
    ShouldNotReachHere();
  }
}

void CompressedKlassPointerSettings_PD::print_on(outputStream* st) const {
  // Don't print base, shift, since those are printed at the caller already
  const char* const modes[] = { "none", "zero", "movk" };
  const int modei = (int)_mode;
  assert(modei >= 0 && modei < 4, "Sanity");
  st->print_cr("Encoding Mode: %s", modes[modei]);
  if (_mode == Mode::KlassDecodeMovk) {
    st->print_cr("Unshifted base: %d", _movk_unshifted_base);
  }
}

// attempt to reserve a memory range well suited to compressed class encoding
address CompressedKlassPointerSettings_PD::reserve_klass_range(size_t len) {
  assert(is_aligned(len, os::vm_allocation_granularity()), "Sanity");

  address result = nullptr;

  // We reserve memory with MOVK mode in mind.
  //
  // Note that this is a "best effort" scenario: here, we attempt to reserve
  //  memory best suitable for MOVK encoding. That may or may not work due to
  //  ASLR etc.
  //
  // Later, we will setup up encoding depending on the klass range we reserve here.
  //  See attempt_initialize().
  //  That setup attempts to work well with any arbitrary klass range we give
  //  it, but will benefit from "good" klass ranges we manage to reserve here.

  // We have two MOVK modes (assuming non-zero shift and base):
  // A) unshifted base: where we first left-shift nKlass, then movk the unshifted base onto it
  // B) pre-shifted base: where we first movk the right-shifted base into the unshifted nK,
  //     then left-shift nK
  // We prefer the former since it may save one move when src and dst registers are different.

  // Minimal shift that would be needed to cover the klass range iff encoding base==klass range start
  // (which we aim for here):
  const int minimal_shift_needed = exact_log2(next_power_of_2(len));
  assert(minimal_shift_needed <= (NarrowKlassPointerBits + LogKlassAlignmentInBytes), "range too large");
  assert(minimal_shift_needed <= 12, "Lets have a reasonable Limit here.");

  // Depending on whether we need more than 32bits to cover the whole of klass range len,
  // calculate the amount by which we will need to shift the base.
  const int base_shift_amount = (minimal_shift_needed > 32) ?
                                32 - minimal_shift_needed : // Mode B
                                0;                          // Mode A

  const int log_klass_range_start_address_alignment = 32 + base_shift_amount;

  // Assume that 64k system page translates to 52-bit address space vs 48-bit with 4k pages.
  // Depends on whether we support ARMv8.2-LVA, but its easier just to check the page size.
  const uint64_t highest_possible_address = (os::vm_page_size() == 64 * K) ? nth_bit(52) : nth_bit(48);


  // Now attempt to reserve a "good" range. That is any range that:
  // a) starts at an address aligned to log_klass_range_start_address
  // b) when being right-shifted (mode B) or unshifted (mode A) only contains bits in either the
  //    third or (assuming a 52-bit address space) the fourth quadrant.
  // For simplicity we ignore the fourth quadrant. We don't have that many tries anyway.

  // Example: for 48-bit addresses, for base_shift_amount = 0, test:
  //   0x0000_0001_0000_0000 ... 0x0001_0000_0000_0000
  //          for 52-bit addresses, for base_shift_amount = 2, test:
  //   0x0000_0004_0000_0000 ... 0x0004_0000_0000_0000
  const uint64_t min = nth_bit(log_klass_range_start_address_alignment);
  const uint64_t max = MIN2(highest_possible_address, (uint64_t)nth_bit(log_klass_range_start_address_alignment + 16));

  // Lets do 32 tries
  const size_t stepsize = CONST64(0x800) << log_klass_range_start_address_alignment;

  const uint64_t klass_range_start_address_alignment = nth_bit(log_klass_range_start_address_alignment);

  for (uint64_t probe_point = min;
      probe_point < max && result == nullptr;
      probe_point += stepsize) {
    assert(is_aligned(probe_point, klass_range_start_address_alignment), "Sanity");

    result = (address)os::attempt_reserve_memory_at((char*)probe_point, len, false);
    assert(result == nullptr || result == (address)probe_point, "Sanity");
  }

  // Fallback case:
  //
  // Any address with lower 32 bits all zero can be used as a base for rshift MOVK mode.
  // So try for that. Note that this may fail too, since reserve_memory_aligned over-allocates
  // alignment. We may run into os limits with the following mmap call.
  if (result == nullptr) {
    assert(NarrowKlassPointerBits <= 32, "Sanity");
    result = (address)os::reserve_memory_aligned(len, nth_bit(32), false);
    assert(result == nullptr || is_aligned(result, nth_bit(32)), "Sanity");
  }

  // Fallback fallback: result anywhere and lets hope encoding setup can deal with the address
  if (result == nullptr) {
    result = (address)os::reserve_memory(len, false);
  }

  return result;
}
