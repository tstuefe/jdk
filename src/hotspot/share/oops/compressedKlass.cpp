/*
 * Copyright (c) 2023 Red Hat, Inc. All rights reserved.
 * Copyright (c) 2023, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2021 SAP SE. All rights reserved.
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

#ifdef _LP64

int NarrowKlassPointerBits = -1;
int LogKlassAlignmentInBytes = -1;
size_t KlassAlignmentInBytes = -1;
int KlassAlignmentInWords = -1;
uint64_t NarrowKlassPointerValueRange = 0;

address CompressedKlassPointers::_base;
int CompressedKlassPointers::_shift;
CompressedKlassPointerSettings_PD CompressedKlassPointers::_pd;
#ifdef ASSERT
address CompressedKlassPointers::_kr1 = nullptr;
address CompressedKlassPointers::_kr2 = nullptr;
#endif

void CompressedKlassPointers::set_kr12(address klass_range_start, size_t klass_range_length) {
  _kr1 = align_up(klass_range_start, LogKlassAlignmentInBytes);
  _kr2 = align_down(klass_range_start + klass_range_length - 1, LogKlassAlignmentInBytes);
  assert(_kr2 > _kr1, "Sanity");
}

// Given a memory range to be encoded (future Klass range), chose a suitable encoding scheme
void CompressedKlassPointers::initialize(address klass_range_start, size_t klass_range_length) {
  set_kr12(klass_range_start, klass_range_length);
  if (_pd.attempt_initialize(_kr1, _kr2)) {
    _base = pd().base();
    _shift = pd().shift();
  } else {
    fatal("Failed to initialize CCS encoding"); // Todo
  }
}

// Given:
// - a memory range to be encoded (future Klass range)
// - a preferred encoding base and shift
// If the desired encoding base and shift can be used for encoding, use that and return true; return false otherwise.
// This is used for the CDS runtime case, where the archive we load pre-determines a base and shift value, but which may or may
// not fit the range we actually managed to reserve.
bool CompressedKlassPointers::attempt_initialize_for_encoding(address klass_range_start, size_t klass_range_length, address desired_base, int desired_shift) {
  set_kr12(klass_range_start, klass_range_length);
  if (_pd.attempt_initialize_for_fixed_base_and_shift(desired_base, desired_shift, _kr1, _kr2)) {
    _base = pd().base();
    _shift = pd().shift();
    return true;
  }
}

// Attempt to reserve a memory range well suited to compressed class encoding
address CompressedKlassPointers::reserve_klass_range(size_t len) {

  len = align_up(len, os::vm_allocation_granularity());
  address result = nullptr;

  // Attempt to allocate for zero-based encoding:
  // This is useful for all platforms, so do this here.
  for (int shift = 0; result == nullptr && shift <= LogKlassAlignmentInBytes; shift++) {
    const size_t encoding_size = nth_bit(shift + NarrowKlassPointerBits);
    if (encoding_size >= len) {
      const address base = (address)(encoding_size - len);
      address p = (address)os::attempt_reserve_memory_at((char*)base, len, false); // Todo tag??
    }
  }

  // Otherwise ask platform
  if (result == nullptr) {
    result = pd().reserve_klass_range(len);
  }

  // Failing that (or, if the platform does not care), reserve anywhere and hope for the best
  result = os::reserve_memory(len, false, mtMetaspace);

  // Todo: NMT tag memory

  return result;
}

void CompressedKlassPointers::print_on(outputStream* st) {

  st->print_cr("UseCompactObjectHeaders: %d", UseCompactObjectHeaders);
  st->print_cr("UseCompressedClassPointers: %d", UseCompressedClassPointers);

  st->print_cr("UseSharedSpaces: %d", UseSharedSpaces);
  st->print_cr("DumpSharedSpaces: %d", DumpSharedSpaces);

  st->print_cr("NarrowKlassPointerBits: %d", NarrowKlassPointerBits);
  st->print_cr("LogKlassAlignmentInBytes: %d", LogKlassAlignmentInBytes);
  st->print_cr("KlassAlignmentInBytes: " SIZE_FORMAT, KlassAlignmentInBytes);
  st->print_cr("NarrowKlassPointerValueRange: " UINT64_FORMAT, NarrowKlassPointerValueRange);

#ifdef ASSERT
  st->print_cr("Klass range: " PTR_FORMAT "-" PTR_FORMAT ", (" SIZE_FORMAT " bytes)",
      p2i(_kr1), p2i(_kr2), (size_t)(_kr2 - _kr1));
#endif

  st->print_cr("Encoding base: " PTR_FORMAT, p2i(base()));
  st->print_cr("Encoding shift: %d", _shift);
  const size_t encoding_range = nth_bit(NarrowKlassPointerBits + shift());
  st->print_cr("Theoretical encoding range: " PTR_FORMAT "-" PTR_FORMAT ", (" SIZE_FORMAT " bytes)",
      p2i(base()), p2i(base() + encoding_range), encoding_range);

  // Print platform specifics
  pd().print_on(st);
}

#ifdef ASSERT
void CompressedKlassPointers::verify() const {
  assert(base() == pd().base() && shift() == pd().shift(), "Sanity");
  const size_t encoding_range = nth_bit(NarrowKlassPointerBits + shift());
  assert(base() <= _kr1 && (base() + encoding_range) > _kr2, "Encoding not large enough");
  pd().verify();
}
#endif

#endif // _LP64
