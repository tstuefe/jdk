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

#include "utilities/globalDefinitions.hpp"

class outputStream;
class MacroAssembler;
class Register;

class CompressedKlassPointerSettings_PD {

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

  bool attempt_initialize_for_zero(address kr2);
  bool attempt_initialize_for_xor(address kr1, address kr2);
  bool attempt_initialize_for_movk(address kr1, address kr2);

  void decode_klass_not_null_for_zero(MacroAssembler* masm, Register dst, Register src) const;
  void decode_klass_not_null_for_xor(MacroAssembler* masm, Register dst, Register src) const;
  void decode_klass_not_null_for_movk(MacroAssembler* masm, Register dst, Register src) const;

public:

  CompressedKlassPointerSettings_PD();

  // Given a klass range, initialize to use the best encoding (if it exists)
  bool attempt_initialize(address kr1, address kr2);

  // "reverse-initialize" from a given base and shift, for a given klass range (called for the CDS runtime path)
  bool attempt_initialize_for_fixed_base_and_shift(address base, int shift, address kr1, address kr2);

  // attempt to reserve a memory range well suited to compressed class encoding
  static address reserve_klass_range(size_t len);

  address base() const  { return _base; }
  int shift() const     { return _shift; }

  void decode_klass_not_null(MacroAssembler* masm, Register dst, Register src) const;
  void encode_klass_not_null(MacroAssembler* masm, Register dst, Register src) const;

  void print_on(outputStream* st) const;

  DEBUG_ONLY(void verify() const;)
};

#endif // CPU_AARCH64_COMPRESSEDKLASS_AARCH64_HPP
