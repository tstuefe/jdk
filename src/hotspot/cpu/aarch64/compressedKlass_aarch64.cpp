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

#include "macroAssembler_aarch64.hpp"
#include "register_aarch64.hpp"

#include "logging/log.hpp"

#include "oops/compressedKlass.hpp"
#include "compressedKlass_aarch64.hpp"
#include "utilities/ostream.hpp"
#include "runtime/globals.hpp"
#include "runtime/os.hpp"

CompressedKlassPointerSettings_PD::CompressedKlassPointerSettings_PD()
  : _base(nullptr), _shift(-1), _mode(Mode::KlassDecodeNone),
    _movk_modeA(false)
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
    return
      ((q(0) > 0) ? 1 : 0) +
      ((q(1) > 0) ? 1 : 0) +
      ((q(2) > 0) ? 1 : 0) +
      ((q(3) > 0) ? 1 : 0);
  }
};

struct movk_mode_t {
  uint64_t _base;
  int _shift;
  bool _modeA;

  bool is_valid_for_movk() {
    // mode A:   unshifted   base must be restricted to q2 q3
    // mode B: right-shifted base must be restricted to q2 q3
    if (_modeA) {
      return (_base & right_n_bits(32)) == 0;
    } else {
      return (_base & right_n_bits(32 + _shift)) == 0;
    }
  }

  // less is better
  int quality() const {
    int num_ops = Quads(_base).num_quadrants_set();
    if (_shift > 0) {
      num_ops ++;
    }
    if (!_modeA) {
      num_ops ++;
    }
    return num_ops;
  }

  bool covers_range(uint64_t kr1, uint64_t kr2) const {
    bool rc = false;
    if (kr1 >= _base) {
      const uint64_t needed_coverage = kr2 - _base;
      rc = (needed_coverage <= nth_bit(NarrowKlassPointerBits + _shift));
    }
    return rc;
  }
};

NOT_DEBUG(static)
DEBUG_ONLY(extern) // for testing in gtests
bool find_best_movk_mode(address kr1, address kr2, movk_mode_t* out) {

  //   |---------------------( e  n  c  o  d  i  n  g     r  a  n  g  e )--------->|
  //   |                                                                           |
  //   |--------------(minimal needed coverage)------------>|                      |
  //   |                                                    |                      |
  //   |------------------------|XXXXXXXXXXXXXXXXXXXXXXXXXXX|--------------------->|
  //   |                        |      (klass range)        |                      |
  //  base                     kr1                         kr2                enc range end

  // Have: kr1 and kr2 (e.g. class space or class space + cds)
  // Want: base and shift and movkmode

  // base is just a number, does not have to correlate with anything physical.
  // Distance between base and kr2 is the coverage we need to provide via
  // NarrowKlassPointerBits >> shift.
  // Ideally, that distance is just the size of the klass range. In that case, base==kr1,
  // which is the case when we managed to attach to a preferred address location in
  // CompressedKlassPointerSettings_PD::reserve_klass_range.

  // In MacroAssembler, we have two modes:
  // In movk mode A, we apply the *unshifted* base to the *left-shifted* nK
  // In movk mode B, we apply the *pre-right-shifted* base to the *original* nK, then left-shift
  //
  // movk mode A needs one operation less when src != dst (because we need an additional movw in mode B)
  //
  // Here, given an arbitrary Klass range [kr1, kr2), we calculate the best base+shift+mode for it.
  // This is complex, because of circularities:
  //
  // Base:
  // - must be ORable with the original (mode B) or left-shifted (mode A) nK.
  // - must be MOVK-able, so:
  //    - in mode A, it must only have bits in the upper 32 bits (ideally restricted to bits 32+shift..47+shift)
  //    - in mode B, it must only have bits in the upper 32+shift bits (ideally restricted to bits 32+shit..47+shift)
  //
  // So, Base depends on shift. But shift depends on the needed coverage range. And the coverage range depends on
  //  the distance between base and kr2. Which depends on base.

  // We solve this by just calculating all options, then taking the best variant.

  bool found = false;
  movk_mode_t found_mode;

  for (int candidate_shift = 0; candidate_shift <= LogKlassAlignmentInBytes; candidate_shift ++) {
    for (int mode = 0; mode < 3; mode ++) {
      const bool modeA = (mode == 0);
      uint64_t candidate_base = p2i(kr1);
      if (modeA) {
        candidate_base &= right_n_bits(32);
      } else {
        candidate_base &= right_n_bits(32 + candidate_shift);
      }
      movk_mode_t mode;
      mode._base = candidate_base;
      mode._shift = candidate_shift;
      mode._modeA = modeA;
      assert(mode.is_valid_for_movk(), "Must be valid"); // must be, since we sheared off q1 q2 above.
      if (mode.covers_range((uint64_t)kr1, (uint64_t)kr2)) {
        if (found == false || mode.quality() < found_mode.quality()) {
          found_mode = mode;
        }
      }
    }
  }

  if (found) {
    (*out) = found_mode;
  }

  return found;
}

bool CompressedKlassPointerSettings_PD::attempt_initialize_for_movk(address kr1, address kr2) {

  movk_mode_t mode;
  if (find_best_movk_mode(kr1, kr2, &mode)) {
    _base = mode._base;
    _shift = mode._shift;
    _movk_modeA = mode._modeA;
    _mode = Mode::KlassDecodeMovk;
    return true;
  }
  return false;
}

bool CompressedKlassPointerSettings_PD::attempt_initialize(address kr1, address kr2) {
  // We prefer zero over xor over movk
  return
      attempt_initialize_for_zero(kr2) ||
      attempt_initialize_for_movk(kr1, kr2);
}

// "reverse-initialize" from a given base and shift, for a given klass range (called for the CDS runtime path)
bool CompressedKlassPointerSettings_PD::attempt_initialize_for_fixed_base_and_shift(address base, int shift, address kr1, address kr2) {

  _mode = Mode::KlassDecodeNone;

  // is the coverage ok?
  const size_t coverage = nth_bit(NarrowKlassPointerBits + shift);

  if (_base + coverage < kr2) {
    log_warning(cds) ("Provided base " PTR_FORMAT ", shift %d, does not cover "
                      "klass range [" PTR_FORMAT ".." PTR_FORMAT ")",
                      p2i(base), shift, p2i(kr1), p2i(kr2));
    return false;
  }

  // given base and shift - which is all CDS stores in archive - deduce mode.
  if (base == nullptr) {
    _mode = Mode::KlassDecodeZero;
    _base = base;
    _shift = shift;
    return true;
  }

  // MOVK mode:
  movk_mode_t mode;
  mode._base = base;
  mode._shift = shift;

  // Prefer modeA if possible modeB if not
  mode._modeA = true;
  if (!mode.is_valid_for_movk()) {
    mode._modeA = false;
    if (!mode.is_valid_for_movk()) {
      // Nope. Out of ideas.
      return false;
    }
  }

  assert(mode.covers_range((uint64_t)kr1, (uint64_t)kr2), "Must be"); // already asserted at start of function

  _mode = Mode::KlassDecodeMovk;
  _base = mode._base;
  _shift = mode._shift;
  _movk_modeA = mode._modeA;

  return true;
}

// Called at VM init time to reserve a klass range (either class space or class space+cds)
// - called for -Xshare:off or -Xshare:dump
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
  assert(quads.q(0) == 0 && quads.q(1) == 0, "invalid base");
  if (quads.q(2) > 0) {
    __ movk(dst, quads.q(2), 32);
  }
  if (quads.q(3) > 0) {
    __ movk(dst, quads.q(3), 48);
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

void CompressedKlassPointerSettings_PD::decode_klass_not_null_for_movk(MacroAssembler* masm, Register dst, Register src) const {
  assert(_base != nullptr, "Sanity");
  const uint64_t base_ui64 = (uint64_t)_base;
  const uint64_t base_ui64_rshifted = ((uint64_t)_base) >> _shift;
  if (_shift == 0) {
    copy_nKlass_if_needed(masm, dst, src);
    generate_movk_ops(masm, base_ui64, dst);
  } else {
    if (_movk_modeA) {
      // unshifted base to left-shifted nK
      __ lsl(dst, src, _shift);
      generate_movk_ops(masm, base_ui64, dst);
    } else {
      // right-shifted base to orginal nK, then leftshift
      copy_nKlass_if_needed(masm, dst, src);
      generate_movk_ops(masm, base_ui64_rshifted, dst);
      __ lsl(dst, dst, _shift);
    }
  }
}

void CompressedKlassPointerSettings_PD::decode_klass_not_null(MacroAssembler* masm, Register dst, Register src) const {
  assert(UseCompressedClassPointers, "should only be used for compressed headers");
  switch (_mode) {
  case Mode::KlassDecodeZero: decode_klass_not_null_for_zero(masm, dst, src);
    break;
  case Mode::KlassDecodeMovk: decode_klass_not_null_for_movk(masm, dst, src);
    break;
  default: ShouldNotReachHere();
  }
}

void CompressedKlassPointerSettings_PD::encode_klass_not_null(MacroAssembler* masm, Register dst, Register src) const {
  assert(UseCompressedClassPointers, "should only be used for compressed headers");
  switch (_mode) {
  case Mode::KlassDecodeZero:
    if (_shift == 0) { // nKlass is Klass*
      copy_nKlass_if_needed(masm, dst, src);
    } else {
      __ lsr(dst, src, _shift);
    }
    break;
  case Mode::KlassDecodeMovk:
    __ ubfx(dst, src, _shift, NarrowKlassPointerBits);
    break;
  default: ShouldNotReachHere();
  }
}

void CompressedKlassPointerSettings_PD::print_on(outputStream* st) const {
  // Don't print base, shift, since those are printed at the caller already
  switch (_mode) {
  case Mode::KlassDecodeZero: st->print("encoding mode: zero");
    break;
  case Mode::KlassDecodeMovk: st->print("encoding mode: movk (movk mode %c)", _movk_modeA ? 'A' : 'B');
    break;
  default: ShouldNotReachHere();
  }
}

