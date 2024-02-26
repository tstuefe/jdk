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
#include "nmt/memTracker.hpp"
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

static void do_test_speed(const bool new_impl) {
  // prepare:
  // We create X reserved regions with Y committed regions in them.
  constexpr int num_reserved = 100;
  constexpr int num_committed = 10000;
  constexpr int num_regions = num_reserved * num_committed;

  constexpr size_t region_size = 4 * K;
  constexpr size_t step_size = region_size * 2;

  constexpr size_t reserved_size = num_committed * step_size;

  constexpr uintptr_t base_i = 0xFFFF000000000000ULL;
  const address base = (address)base_i;

  double d1 = os::elapsedTime();

  // Now, establish regions
  for (int i = 0; i < num_reserved; i++) {
    const address addr = base + (i * reserved_size);
    const MEMFLAGS f = ((i % 2) == 0) ? mtNMT : mtInternal;
    if (new_impl) {
      VMADictionary::register_create_mapping(addr, addr + reserved_size, f, VMAState::reserved);
    } else {
      MemTracker::record_virtual_memory_reserve(addr, reserved_size, CALLER_PC, f);
    }

    // Establish committed regions
    for (int i2 = 0; i2 < num_committed; i2++) {
      const address addr2 = addr + (i2 * step_size);
      if (new_impl) {
        VMADictionary::register_create_mapping(addr2, addr2 + region_size, f, VMAState::committed);
      } else {
        MemTracker::record_virtual_memory_commit(addr2, region_size, CALLER_PC);
      }
    }
  }

  double d2 = os::elapsedTime();
  tty->print_cr("Setup: %f seconds", d2 - d1);

  // Now: randomly commit and uncommit regions.
  const double d3 = d2 + 5.0f;
  uintx num_recommits = 0;
  int r = os::random();
  while (os::elapsedTime() < d3) {
    r = os::next_random(r);
    const int res_i = r % num_reserved;
    r = os::next_random(r);
    const int com_i = r % num_committed;
    const MEMFLAGS f = ((res_i % 2) == 0) ? mtNMT : mtInternal;
    const address addr = base + (res_i * reserved_size) + (com_i * step_size);
    if (new_impl) {
      VMADictionary::register_create_mapping(addr, addr + region_size, f, VMAState::reserved); // uncommit
      VMADictionary::register_create_mapping(addr, addr + region_size, f, VMAState::committed); // re-commit
    } else {
      Tracker(Tracker::uncommit).record(addr, region_size); // uncommit
      MemTracker::record_virtual_memory_commit(addr, region_size, CALLER_PC); // re-commit
    }
    num_recommits ++;
  }

  tty->print_cr("Result: " UINTX_FORMAT, num_recommits);

  {
    double d = os::elapsedTime();
    if (new_impl) {
      VMADictionary::report_summary(tty);
      double d2 = os::elapsedTime();
      tty->print_cr("Summary took %f seconds.", d2 - d);
    }
  }
}

TEST_OTHER_VM(NMTVMADict, test_speed_old_1)  {  do_test_speed(false); }
TEST_OTHER_VM(NMTVMADict, test_speed_new_1)  {  do_test_speed(true); }

