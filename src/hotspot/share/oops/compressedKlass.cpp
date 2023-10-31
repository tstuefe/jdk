/*
 * Copyright (c) 2019, 2023, Oracle and/or its affiliates. All rights reserved.
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
#include "oops/compressedKlass.hpp"
#include "runtime/globals.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/ostream.hpp"

address CompressedKlassPointers::_base = nullptr;
int CompressedKlassPointers::_shift = 0;
size_t CompressedKlassPointers::_range = 0;

#ifdef _LP64

void CompressedKlassPointers::verify_encoding_for_range(address addr, size_t len, address base, int shift) {
  guarantee(addr >= base, "base does not preceed range start");
  const size_t encoding_range_size = nth_bit(shift);
  guarantee((addr + len) <= (base + encoding_range_size), "range not fully covered by encoding scheme");
}

// Given a klass range [addr, addr+len) and a given encoding scheme, assert that this scheme covers the range, then
// set this encoding scheme. Used by CDS at runtime to re-instate the scheme used to pre-compute klass ids for
// archived heap objects.
void CompressedKlassPointers::initialize_for_given_encoding(address addr, size_t len, address requested_base, int requested_shift) {
  assert(is_valid_base(requested_base), "Address must be a valid encoding base");

  verify_encoding_for_range(addr, len, requested_base, requested_shift);

  _klass_range_start = addr;
  _klass_range_end = addr + len;

  // This function is called from CDS only, and for CDS at runtime the requested base has to equal
  // the range start.
  assert(requested_base == addr, "Invalid requested base");

  _base = requested_base;
  _shift = requested_shift;

  assert(klass_min() < klass_max(), "klass range too small");

  _initialized = true;
}

// Given an address range [addr, addr+len) which the encoding is supposed to
//  cover, choose base, shift and range.
//  The address range is the expected range of uncompressed Klass pointers we
//  will encounter (and the implicit promise that there will be no Klass
//  structures outside this range).
void CompressedKlassPointers::initialize(address addr, size_t len) {
  assert(UseCompressedClassPointers, "no compressed klass ptrs?");

  _klass_range_start = addr;
  _klass_range_end = addr + len;

  // Attempt to run with encoding base == zero
  _base = (_klass_range_end <= (address)max_encoding_range()) ? nullptr : addr;

  // Highest offset a Klass* can ever have in relation to base.
  const size_t highest_klass_offset = _klass_range_end - _base;

  // Set shift. We may not even need a shift if the range fits into 32bit.
  constexpr size_t unscaled_max = nth_bit(_max_narrow_klass_id_bits);
  _shift = (highest_klass_offset <= unscaled_max) ? 0 : _max_shift;

  verify_encoding_for_range(addr, len, _base, _shift);
  assert(klass_min() < klass_max(), "klass range too small");
  assert(is_valid_base(_base), "Address must be a valid encoding base");

  _initialized = true;
}

// Given an address p, return true if p can be used as an encoding base.
//  (Some platforms have restrictions of what constitutes a valid base address).
bool CompressedKlassPointers::is_valid_base(address p) {
#ifdef AARCH64
  // Below 32G, base must be aligned to 4G.
  // Above that point, base must be aligned to 32G
  if (p < (address)(32 * G)) {
    return is_aligned(p, 4 * G);
  }
  return is_aligned(p, (4 << LogKlassAlignmentInBytes) * G);
#else
  return true;
#endif
}

void CompressedKlassPointers::print_mode(outputStream* st) {
  st->print_cr("Narrow klass base: " PTR_FORMAT ", shift: %d, "
               "Range [" PTR_FORMAT ", " PTR_FORMAT "), "
               "min/max nKlass: %u/%u, num classes: %u",
               p2i(base()), shift(), p2i(_klass_range_start), p2i(_klass_range_end),
               narrow_klass_min(), narrow_klass_max(), num_possible_classes());
}

#endif // _LP64
