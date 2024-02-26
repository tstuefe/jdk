/*
 * Copyright (c) 2021, 2022 SAP SE. All rights reserved.
 * Copyright (c) 2021, 2024, Oracle and/or its affiliates. All rights reserved.
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
 */

#include "precompiled.hpp"
#include "memory/allocation.hpp"
#include "nmt/vmaTree.hpp"
#include "runtime/os.hpp"
#include "utilities/ostream.hpp"
#include "unittest.hpp"

// convenience log. switch on if debugging tests. Don't use tty, plain stdio only.
//#define LOG(...) { printf(__VA_ARGS__); printf("\n"); fflush(stdout); }
#define LOG(...)

static void loghere() {
  tty->print("--\n");
  VMADictionary::print_tree_raw(tty);
  DEBUG_ONLY(VMADictionary::verify();)
}

TEST_VM(NMTVMADict, basics) {

  const address A = (address)G;
  const address B = (address)G + M;
  const address C = (address)G + (M * 2);
  const address D = (address)G + (M * 3);
  constexpr VMAState res = VMAState::reserved;
  constexpr VMAState com = VMAState::committed;

  loghere();

  VMADictionary::register_create_mapping(A, B, mtNMT, res);
  loghere();

  VMADictionary::register_create_mapping(A, B, mtNMT, res);
  loghere();

  VMADictionary::register_create_mapping(B, C, mtNMT, res);
  loghere();

  VMADictionary::register_create_mapping(C, D, mtClass, res);
  loghere();

  VMADictionary::register_create_mapping(B, C, mtNMT, com);
  loghere();

  VMADictionary::report_summary(tty);

  VMADictionary::register_release_mapping(A, C);
  loghere();

}

TEST_VM(NMTVMADict, random) {
  constexpr int max_cycles = 100000;
  constexpr int address_variance = 40;
  int r = os::random();

  for (int n = 0; n < max_cycles; n++) {

    r = os::next_random(r);
    int n1 = r % address_variance;
    r = os::next_random(r);
    int n2 = r % address_variance;

    if (n1 > n2) {
      int tmp = n1;
      n1 = n2;
      n2 = tmp;
    }
    if (n1 == n2) {
      if (n1 == 0) {
        n2 ++;
      } else {
        n1 --;
      }
    }

    const address from = (address)(G * (1 + n1));
    const address to = (address)(G * (1 + n2));

    r = os::next_random(r);
    const bool unmap      = (r % 4) == 0;
    const bool committed  = (r % 1) == 0;

    r = os::next_random(r);
    const MEMFLAGS f = (MEMFLAGS)(r % (int)(MEMFLAGS::mt_number_of_types));

    if (unmap) {
      VMADictionary::register_release_mapping(from, to);
    } else {
      VMADictionary::register_create_mapping(from, to, f, committed ? VMAState::committed : VMAState::reserved);
    }
  }

  VMADictionary::report_summary(tty);

  // Delete all nodes
  VMADictionary::register_release_mapping((address)(4 * K), (address)SIZE_MAX);
  VMADictionary::report_summary(tty);

}

