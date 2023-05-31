/*
 * Copyright (c) 2021 SAP SE. All rights reserved.
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

#ifndef SHARE_OOPS_COMPRESSEDKLASS_HPP
#define SHARE_OOPS_COMPRESSEDKLASS_HPP

#include "memory/allStatic.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/macros.hpp"

#include CPU_HEADER(compressedKlass)

class outputStream;
class Klass;

#ifdef _LP64

// Narrow Klass pointer (nKlass) geometry. These values are *not* the same as
// CompressedKlassPointers::shift() etc, though they are closely related.

// Size, in bits, an nKlass occupies. Legacy: 32 bits; COH-mode: 2x bits.
extern int NarrowKlassPointerBits;

// The alignment of Klass structures in memory.
// Or, the size of the alignment shadow of a valid Klass* pointer.
// Or, the interval at which Klass structures can be located.
extern int LogKlassAlignmentInBytes;
extern size_t KlassAlignmentInBytes;
extern int KlassAlignmentInWords;

// How many valid values can be expressed with an nKlass (aka 1<<NarrowKlassPointerBits)
extern uint64_t NarrowKlassPointerValueRange;

// The maximum size of the range that can be encoded with the current nKlass geometry
//extern size_t KlassEncodingMetaspaceMax;

#else
// Most of the compressed class pointer encoding gets compiled for 32-bit too, even though
// it never gets called. We may fix that in the future, but for now we need these constants
// to prevent build errors.
const int LogKlassAlignmentInBytes = 3; // traditional 64-bit alignment
const int KlassAlignmentInBytes    = 1 << LogKlassAlignmentInBytes;
const int KlassAlignmentInWords = KlassAlignmentInBytes / BytesPerWord;
const int NarrowKlassPointerBits = 32;
const uint64_t NarrowKlassPointerValueRange = ((uint64_t)1) << NarrowKlassPointerBits;
const uint64_t KlassEncodingMetaspaceMax = (uint64_t(max_juint) + 1) << LogKlassAlignmentInBytes;
#endif

typedef uint32_t narrowKlass;

class CompressedKlassPointers : public AllStatic {

  // Encoding base and shift
  static address _base;
  static int _shift;

  // Optional platform-specific details
  static CompressedKlassPointerSettings_PD _pd;

#ifdef ASSERT
  // First and last valid Klass location
  static address _kr1;
  static address _kr2;
#endif

public:

  // The decode/encode versions taking an explicit base are for the sole use of CDS during dump time
  // (see ArchiveBuilder).
  static inline Klass* decode_raw(narrowKlass v, address base);
  static inline Klass* decode_not_null(narrowKlass v, address base);
  static inline narrowKlass encode_not_null(Klass* v, address base);
  DEBUG_ONLY(static inline void verify_klass_pointer(const Klass* v, address base));

  static const CompressedKlassPointerSettings_PD& pd() { return _pd; }

  // Given a memory range to be encoded (future Klass range), chose a suitable encoding scheme and initialize encoding.
  // Return false if there is no encoding that would work with the given klass range.
  static bool attempt_initialize(address klass_range_start, size_t klass_range_length);

  // Given a memory range to be encoded, test if that range can be encoded. Only used at
  // CDS dumptime to check if a given (overridden via command line) SharedBaseAddress is feasible.
  static bool can_encode_klass_range(address klass_range_start, size_t klass_range_length);

  // Given:
  // - a memory range to be encoded (future Klass range)
  // - a preferred encoding base and shift
  // If the desired encoding base and shift can be used for encoding, use that and return true; return false otherwise.
  // This is used for the CDS runtime case, where the archive we load pre-determines a base and shift value, but which may or may
  // not fit the range we actually managed to reserve.
  static bool attempt_initialize_for_encoding(address klass_range_start, size_t klass_range_length,
                                       address desired_base, int desired_shift);

  // attempt to reserve a memory range well suited to compressed class encoding
  static address reserve_klass_range(size_t len);

  static void print_on(outputStream* st);

  // The encoding base and shift. Note that this shift is not necessarily the same as
  // LogKlassAlignmentInBytes - a platform could avoid the shift if the reduced encoding
  // range would still be large enough to encode all possible Klass* values.
  static inline address base()                       { return _base; }
  static inline int shift()                          { return _shift; }

  static bool is_null(Klass* v)      { return v == nullptr; }
  static bool is_null(narrowKlass v) { return v == 0; }

  static inline Klass* decode_raw(narrowKlass v);
  static inline Klass* decode_not_null(narrowKlass v);
  static inline Klass* decode(narrowKlass v);
  static inline narrowKlass encode_not_null(Klass* v);
  static inline narrowKlass encode(Klass* v);

  DEBUG_ONLY(static inline void verify_klass_pointer(const Klass* v));
  DEBUG_ONLY(static inline void verify_narrow_klass_pointer(narrowKlass v);)
  DEBUG_ONLY(static void verify());

};

#endif // SHARE_OOPS_COMPRESSEDKLASS_HPP
