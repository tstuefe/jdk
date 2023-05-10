/*
 * Copyright (c) 2021 SAP SE. All rights reserved.
 * Copyright (c) 2021, Red Hat Inc. All rights reserved.
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

#include "precompiled.hpp"

#include "macroAssembler_x86.hpp"
#include "oops/compressedKlass.hpp"
#include "utilities/ostream.hpp"
#include "utilities/align.hpp"
#include "utilities/debug.hpp"
#include "runtime/globals.hpp"

#ifdef _LP64

// Given an address p, return true if p can be used as an encoding base.
//  (Some platforms have restrictions of what constitutes a valid base address).
bool CompressedKlassPointers::is_valid_base(address p) {
  if (LogKlassAlignmentInBytes > Address::times_8) {
    // Decoding with larger shifts requires a base that is at least aligned to the shift. Since
    // encoding base address is usually page aligned, this should pose no problem.
    return is_aligned(p, KlassAlignmentInBytes);
  }
  return true; // For shifts <= 3 every base is fine.
}

void CompressedKlassPointers::print_mode_pd(outputStream* st) {}

#endif // _LP64
