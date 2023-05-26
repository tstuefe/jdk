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
extern int KlassAlignmentInBytes;
extern int KlassAlignmentInWords;

// How many valid values can be expressed with an nKlass (aka 1<<NarrowKlassPointerBits)
extern uint64_t NarrowKlassPointerValueRange;

// The maximum size of the range that can be encoded with the current nKlass geometry
extern size_t KlassEncodingMetaspaceMax;

#else
// Most of the compressed class pointer encoding gets compiled for 32-bit too, even though
// it never gets called. We may fix that in the future, but for now we need these constants
// to prevent build errors.
const int LogKlassAlignmentInBytes = 3; // traditional 64-bit alignment
const int KlassAlignmentInBytes    = 1 << LogKlassAlignmentInBytes;
const int KlassAlignmentInWords = KlassAlignmentInBytes / BytesPerWord;
const int MaxNarrowKlassPointerBits = 32;
const uint64_t NarrowKlassPointerValueRange = ((uint64_t)1) << MaxNarrowKlassPointerBits;
const uint64_t KlassEncodingMetaspaceMax = (uint64_t(max_juint) + 1) << LogKlassAlignmentInBytes;
#endif

typedef uint32_t narrowKlass;

class CompressedKlassPointerSettings {

  // A dense representation of values that are frequently used together, in order to fold
  // them into a single load:
  // - UseCompactObjectHeaders and UseCompressedClassPointers flags
  // - encoding base and encoding shift
  // All of them we can encode in a 64-bit word. Since the encoding base will be page-aligned,
  // we have a 12-bit alignment shadow to store the rest of the data.

  uintptr_t _config;
  constexpr int useCompactObjectHeadersShift = 0;
  constexpr int useCompressedClassPointersShift = 1;
  constexpr int encodingShiftShift = 2;
  constexpr int encodingShiftWidth = 5;
  constexpr int baseAddressMask = ~right_n_bits(12);

public:

  CompressedKlassPointerSettings() : _config(0) {}

  void set_encoding_base(address base);
  void set_encoding_shift(int shift);
  void set_use_compact_headers(bool b);
  void set_use_compressed_class_pointers(bool b);

  address base() const  { return (address)(_config & baseAddressMask); }
  int shift() const     { return (_config >> encodingShiftShift) & right_n_bits(encodingShiftWidth); }
  bool use_compact_object_headers() const     { return (_config >> useCompactObjectHeadersShift) & 1; }
  bool use_compressed_class_pointers() const  { return (_config >> useCompressedClassPointersShift) & 1; }

  void print_mode_pd(outputStream* st) const;

};

class CompressedKlassPointers : public AllStatic {

  static CompressedKlassPointerSettings _settings;

  // optional platform-specific details
  static CompressedKlassPointerSettings_PD _pd;

  // These members hold copies of encoding base and shift and only exist for SA (see vmStructs.cpp and
  // sun/jvm/hotspot/oops/CompressedKlassPointers.java)
  static address _base_copy;
  static int _shift_copy;

  static void initialize_raw(address base, int shift);

  // Platform specific hooks:

  // Given a memory range [addr, addr + len) to be encoded (future Klass location range), set encoding.
  static void initialize_pd(address addr, size_t len);

public:

  // Given a shift, calculate the max. memory range encoding would have given the current nKlass width
  static inline size_t calc_encoding_range_size(int num_narrow_klass_bits, int shift);
  static inline size_t calc_encoding_range_size(int shift);

  static void set_encoding_base(address base)           { _settings.set_encoding_base(base); }
  static void set_encoding_shift(int shift)             { _settings.set_encoding_shift(shift); }
  static void set_use_compact_headers(bool b)           { _settings.set_use_compact_headers(b); }
  static void set_use_compressed_class_pointers(bool b) { _settings.set_use_compressed_class_pointers(b); }

  // The decode/encode versions taking an explicit base are for the sole use of CDS during dump time
  // (see ArchiveBuilder).
  static inline Klass* decode_raw(narrowKlass v, address base);
  static inline Klass* decode_not_null(narrowKlass v, address base);
  static inline narrowKlass encode_not_null(Klass* v, address base);
  DEBUG_ONLY(static inline void verify_klass_pointer(const Klass* v, address base));

  static void print_mode_pd(outputStream* st)           { _settings.print_mode_pd(st); }

  // Given an address p, return true if p can be used as an encoding base.
  //  (Some platforms have restrictions of what constitutes a valid base
  //   address).
  static bool is_valid_base(address p);

  // Given:
  // - a memory range [addr, addr + len) to be encoded (future Klass location range)
  // - a desired encoding base and shift
  // if the desired encoding base and shift are suitable to encode the desired memory range, initialize
  // CompressedClassPointers with the desired base/shift, and return true. Otherwise return false.
  // Used to initialize compressed class pointer encoding for the CDS runtime case, where we would really
  // prefer the encoding base and shift used when the archive was generated, but where due to runtime condition
  // the CDS may have been mapped at a different base, or where the to-be-encoded range is larger than at dump time
  // (e.g. larger CompressedClassSPaceSize).
  bool attempt_initialize_for_encoding(address addr, size_t len, address desired_base, int desired_shift);

  // Given a memory range [addr, addr + len) to be encoded (future Klass location range),
  // choose base and shift.
  void initialize(address addr, size_t len);

  static void print_mode(outputStream* st);

  // The encoding base and shift. Note that this shift is not necessarily the same as
  // LogKlassAlignmentInBytes - a platform could avoid the shift if the reduced encoding
  // range would still be large enough to encode all possible Klass* values.
  static inline address base()                       { return _settings.base(); }
  static inline int shift()                          { return _settings.shift(); }
  static inline bool use_compact_object_headers()    { return _settings.use_compact_object_headers(); }
  static inline bool use_compressed_class_pointers() { return _settings.use_compressed_class_pointers(); }

  // Encoding range size
  static inline size_t encoding_range_size();

  // End of encoding range
  static inline address end();


  static bool is_null(Klass* v)      { return v == nullptr; }
  static bool is_null(narrowKlass v) { return v == 0; }

  static inline Klass* decode_raw(narrowKlass v);
  static inline Klass* decode_not_null(narrowKlass v);
  static inline Klass* decode(narrowKlass v);
  static inline narrowKlass encode_not_null(Klass* v);
  static inline narrowKlass encode(Klass* v);

  DEBUG_ONLY(static inline void verify_klass_pointer(const Klass* v));
  DEBUG_ONLY(static inline void verify_narrow_klass_pointer(narrowKlass v);)

  const CompressedKlass_PD& pd() { return _pd; }

};

#endif // SHARE_OOPS_COMPRESSEDOOPS_HPP
