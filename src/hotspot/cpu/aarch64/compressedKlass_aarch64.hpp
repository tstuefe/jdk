/*
 * Copyright (c) 2023, Red Hat Inc. All rights reserved.
 * Copyright (c) 2023, Oracle and/or its affiliates. All rights reserved.
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

#ifndef CPU_AARCH64_COMPRESSEDKLASS_AARCH64_HPP
#define CPU_AARCH64_COMPRESSEDKLASS_AARCH64_HPP


class outputStream;

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

// Structure to hold aarch64 specific settings for compressed klass en/decoding

struct CompressedKlassPointerSettings_PD {

  // Base, shift
  address _base;
  int _shift;

  enum class Mode {
    KlassDecodeNone,
    KlassDecodeZero,
    KlassDecodeXor,
    KlassDecodeMovk
  };

  Mode _mode;

  // for XOR and MOVK, decode:
  // Whether to xor the *right-shifted* base to the *unshifted* nKlass,
  // or the *unshifted* base to the *left-shifted* nKlass
  // Does not matter for encode, both use ubfx (or movw/movz if possible)
  bool _do_rshift_base;

  // for MOVK mode: imm16 for second, third and fourth quadrant (typically just one is used)
  Quads _movk_imm16_quads;

  bool attempt_initialize_for_zero(address kr2);
  bool attempt_initialize_for_xor(address kr1, address kr2);
  bool attempt_initialize_for_movk(address kr1, address kr2);

public:

  CompressedKlassPointerSettings_PD();

  bool attempt_initialize(address kr1, address kr2);

  bool attempt_initialize_for_fixed_base_and_shift(address base, int shift, address kr1, address kr2);

  address base() const  { return _base; }
  int shift() const     { return _shift; }

  void decode_klass_not_null(MacroAssembler* masm, Register dst, Register src) const;
  void encode_klass_not_null(MacroAssembler* masm, Register dst, Register src) const;

  void print_on(outputStream* st) const;

};


#endif // CPU_AARCH64_COMPRESSEDKLASS_AARCH64_HPP
