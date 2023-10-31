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

#ifndef SHARE_OOPS_COMPRESSEDKLASS_HPP
#define SHARE_OOPS_COMPRESSEDKLASS_HPP

#include "memory/allStatic.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"

class outputStream;
class Klass;

// If compressed klass pointers then use narrowKlass.
typedef juint  narrowKlass;

const int LogKlassAlignmentInBytes = 3;
const int KlassAlignmentInBytes    = 1 << LogKlassAlignmentInBytes;

// For UseCompressedClassPointers.
class CompressedKlassPointers : public AllStatic {

  static bool _initialized;

  // The maximum number of bits in a narrow Klass ID
  // (can be less at runtime if we don't fully use the theoretically possible encoding range)
  static constexpr int _max_narrow_klass_id_bits = 32;

  // The maximum value we can shift
  // (can be less at runtime: we set it to zero if possible).
  static constexpr int _max_shift = 3;

  friend class VMStructs;

  // Encoding scheme: Klass* = (nKlass << _shift) + _base
  static address _base;
  static int _shift;

  // The to-be-encoded range: we only expect Klass to exist within this address range
  static address _klass_range_start;
  static address _klass_range_end;

  // Given a klass range [addr, addr+len) and an encoding scheme (base, shift), verify that this scheme
  // covers the whole range
  DEBUG_ONLY(static void verify_encoding_for_range(address addr, size_t len, address requested_base, int requested_shift);)

  static void assert_inited() { assert(_initialized, "Not yet initialized"); }

public:

  // Largest theoretically possible encoding range size
  static inline size_t max_encoding_range();

  // Largest theoretically possible Klass alignment
  static inline size_t max_klass_alignment() { return MAX2((size_t)BytesPerWord, (size_t)nth_bit(_max_shift)); }

  // Given an address p, return true if p can be used as an encoding base.
  //  (Some platforms have restrictions of what constitutes a valid base
  //   address).
  static bool is_valid_base(address p);

  // Given a klass range [addr, addr+len) and an encoding scheme (base, shift), assert that this scheme covers
  // the range, then set this encoding scheme. Used by CDS at runtime to re-instate the scheme used to pre-compute
  // klass ids for archived heap objects.
  static void initialize_for_given_encoding(address addr, size_t len, address requested_base, int requested_shift);

  // Given an address range [addr, addr+len) which the encoding is supposed to
  //  cover, choose base, shift and range.
  //  The address range is the expected range of uncompressed Klass pointers we
  //  will encounter (and the implicit promise that there will be no Klass
  //  structures outside this range).
  static void initialize(address addr, size_t len);

  static void print_mode(outputStream* st);

  static bool is_null(const Klass* v)  { return v == nullptr; }
  static bool is_null(narrowKlass v)   { return v == 0; }

  static inline Klass* decode_raw(narrowKlass v, address base, int shift);
  static inline Klass* decode_raw(narrowKlass v);
  static inline Klass* decode_not_null(narrowKlass v);
  static inline Klass* decode_not_null(narrowKlass v, address base, int shift);
  static inline Klass* decode(narrowKlass v);
  static inline narrowKlass encode_not_null(const Klass* v);
  static inline narrowKlass encode_not_null(const Klass* v, address base, int shift);
  static inline narrowKlass encode(const Klass* v);


  // The following values are runtime-values, determined after the klass range location
  // is known. Using them before initialization will assert.

  // Encoding base
  static address  base()               { assert_inited(); return _base; }

  // Encoding shift (runtime value; may be less than max. possible shift)
  static int      shift()              { assert_inited(); return _shift; }

  // Returns Klass* alignment dictated by *runtime* shift AND natural alignment
  static size_t   klass_alignment()    { return MAX2((size_t)BytesPerWord, (size_t)nth_bit(_shift)); }

  // Lowest valid Klass*
  static inline const Klass* klass_min();

  // Highest valid Klass*
  static inline const Klass* klass_max();

  // Lowest valid narrow Klass ID larger than 0
  static inline narrowKlass narrow_klass_min();

  // Highest valid narrow Klass ID
  static inline narrowKlass narrow_klass_max();

  // Number of bits needed to represent a narrow Klass ID
  static inline int narrow_klass_id_bits();

  // Number of classes we can represent
  static inline unsigned num_possible_classes();

#ifdef ASSERT
  // Asserts if k is properly aligned and inside allowed Klass range. Must not be null.
  static inline void check_klass_not_null(const Klass* k);
  // Asserts if k is inside allowed narrow Klass id range. Must not be 0.
  static inline void check_narrow_klass_id_not_null(narrowKlass k);
  // Asserts if k is properly aligned and inside allowed Klass range. Can be null.
  static inline void check_klass(const Klass* k);
  // Asserts if k is inside allowed narrow Klass id range. Can be 0.
  static inline void check_narrow_klass_id(narrowKlass k);
#endif

};

#endif // SHARE_OOPS_COMPRESSEDKLASS_HPP
