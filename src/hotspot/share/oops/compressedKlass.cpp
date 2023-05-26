/*
 * Copyright (c) 2021 SAP SE. All rights reserved.
 * Copyright (c) 2023 Red Hat, Inc. All rights reserved.
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
#include "logging/log.hpp"
#include "oops/compressedKlass.inline.hpp"
#include "runtime/java.hpp"
#include "runtime/globals.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/ostream.hpp"

CompressedKlassPointerSettings CompressedKlassPointers::_settings;
address CompressedKlassPointers::_base_copy = nullptr;
int CompressedKlassPointers::_shift_copy = 0;

#ifdef _LP64
int NarrowKlassPointerBits = -1;
int LogKlassAlignmentInBytes = -1;
int KlassAlignmentInBytes = -1;
int KlassAlignmentInWords = -1;
uint64_t NarrowKlassPointerValueRange = 0;
size_t KlassEncodingMetaspaceMax = 0;
#endif

static void CompressedKlassPointers::initialize_raw(address base, int shift) {
  _settings.set_encoding_base(base);
  _settings.set_encoding_shift(shift);
  _settings.set_use_compact_headers(UseCompactObjectHeaders);
  _settings.set_use_compressed_class_pointers(UseCompressedClassPointers);
  _base_copy = base;
  _shift_copy = shift;
}

// Given:
// - a memory range [addr, addr + len) to be encoded (future Klass location range)
// - a desired encoding base and shift
// if the desired encoding base and shift are suitable to encode the desired memory range, use them
// and return true. Otherwise return false.
// Used to initialize compressed class pointer encoding for the CDS runtime case, where we prefer to use
// the same encoding base and shift as we used at archive dump time. But since the CDS archive may be located
// at a different base address or class space may be larger and hence to-be-encoded range may be larger than at
// dump time, the desired base/shift are not a guaranteed fit.
bool CompressedKlassPointers::attempt_initialize_for_encoding(address addr, size_t len, address desired_base, int desired_shift) {
#ifdef _LP64
  assert(UseCompressedClassPointers, "Only for +UseCompressedClassPointers");
  assert(UseSharedSpaces, "Only at CDS runtime");
  assert(len <= KlassEncodingMetaspaceMax,
         "to be encoded range is too large to be covered by Klass encoding");
  assert(NarrowKlassPointerBits + desired_shift < BitsPerLong, "Invalid proposed shift (%d)", desired_shift);s
  assert(desired_shift <= LogKlassAlignmentInBytes,
         "Archive narrow Klass shift (%d) > LogKlassAlignmentInBytes (%d) - are we using the wrong archive?",
         desired_shift, LogKlassAlignmentInBytes);

  // Does the proposed encoding scheme cover all of the to-be-encoded range?
  const size_t encoding_range_size = calc_encoding_range_size(NarrowKlassPointerBits, desired_shift);
  if (desired_base <= addr && (desired_base + encoding_range_size) >= (addr + len)) {
    // Yes, we can use this.
    initialize_raw(desired_base, desired_shift);
    return true;
  }

#else
  ShouldNotReachHere();
#endif // _LP64

  return false;
}

// Given a memory range [addr, addr + len) to be encoded (future Klass location range),
// choose base and shift.
void CompressedKlassPointers::initialize(address addr, size_t len) {
#ifdef _LP64
  assert(UseCompressedClassPointers, "Only for CCS");
  assert(len <= KlassEncodingMetaspaceMax,
         "to be encoded range is too large to be covered by Klass encoding");

  initialize_pd(addr, len);

  // Double check base/shift proposed by the platfom
  assert(NarrowKlassPointerBits + shift() < BitsPerLong, "Invalid proposed shift (%d)", shift());
  assert(shift() <= LogKlassAlignmentInBytes, "Invalid proposed shift (%d), larger than LogKlassAlignmentInBytes (%d)",
         shift(), LogKlassAlignmentInBytes);
  const size_t encoding_range_size = nth_bit(NarrowKlassPointerBits + shift());
  assert(encoding_range_size >= len, "Invalid proposed shift (%d), not large enough to encode Klass range "
         "[" PTR_FORMAT "-" PTR_FORMAT ")", shift(), p2i(addr), p2i(addr + len));
  assert(base() <= addr && (base() + encoding_range_size) >= (addr + len),
         "Invalid proposed base (" PTR_FORMAT ") and shift (%d), does not cover Klass range "
         "[" PTR_FORMAT "-" PTR_FORMAT ")", p2i(base()), shift(), p2i(addr), p2i(addr + len));

#else
  ShouldNotReachHere(); // 64-bit only
#endif

}

void CompressedKlassPointers::print_mode(outputStream* st) {
  st->print_cr("UseCompactObjectHeaders: %d", UseCompactObjectHeaders);
  st->print_cr("UseCompressedClassPointers: %d", UseCompressedClassPointers);
  st->print_cr("LogKlassAlignmentInBytes: %d", LogKlassAlignmentInBytes);
  st->print_cr("KlassAlignmentInBytes: %d", KlassAlignmentInBytes);
  st->print_cr("MaxNarrowKlassPointerBits: %d", MaxNarrowKlassPointerBits);
  st->print_cr("NarrowKlassPointerValueRange: " UINT64_FORMAT, NarrowKlassPointerValueRange);
#ifdef _LP64
  st->print_cr("KlassEncodingMetaspaceMax: " UINT64_FORMAT " (" UINT64_FORMAT_X ")", KlassEncodingMetaspaceMax, KlassEncodingMetaspaceMax);
  st->print_cr("Narrow klass base: " PTR_FORMAT ", Narrow klass shift: %d, "
               "Narrow klass range: " SIZE_FORMAT_X "( " PROPERFMT ")",
               p2i(base()), shift(), KlassEncodingMetaspaceMax, PROPERFMTARGS(KlassEncodingMetaspaceMax));
  print_mode_pd(st);
#endif
}

// 64-bit platforms define these functions on a per-platform base. They are not needed for
//  32-bit (in fact, the whole setup is not needed and could be excluded from compilation,
//  but that is a question for another RFE).
#ifndef _LP64
// Given an address p, return true if p can be used as an encoding base.
//  (Some platforms have restrictions of what constitutes a valid base address).
bool CompressedKlassPointers::is_valid_base(address p) {
  ShouldNotReachHere(); // 64-bit only
  return false;
}
#endif
