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

#ifndef CPU_X86_COMPRESSEDKLASS_X86_HPP
#define CPU_X86_COMPRESSEDKLASS_X86_HPP

// Structure to hold x86 specific settings for compressed klass en/decoding

class CompressedKlassPointerSettings_PD {

  // Base, shift
  address _base;
  int _shift;

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

  void print_on(outputStream* st) const;

  DEBUG_ONLY(void verify() const;)
};

#endif // CPU_X86_COMPRESSEDKLASS_X86_HPP
