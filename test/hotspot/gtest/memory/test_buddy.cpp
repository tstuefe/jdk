/*
 * Copyright (c) 2021, 2025, Oracle and/or its affiliates. All rights reserved.
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
 */

#include "memory/buddy_alloc_wrapper.hpp"
#include "runtime/os.hpp"

#include "utilities/globalDefinitions.hpp"
#include "unittest.hpp"
#include "testutils.hpp"


TEST_VM(Buddy, simple) {

  BuddyAlloc::initialize();

  for (size_t s = 4 * K; s <= 4 * G; s *= 2) {
    address p = (address)BuddyAlloc::allocate_memory(s);
    assert(p != nullptr, "sanity");
    p[0] = 41;
    p[s - 1] = 41;
    BuddyAlloc::deallocate_memory(p, s);
  }

  BuddyAlloc::cleanup();
}

TEST_VM(Buddy, random) {

  BuddyAlloc::initialize();

  struct { address p; size_t s; } pointers[1024];
  memset(pointers, 0, sizeof(pointers));

  int pos = 0;
  for (int cycle = 0; cycle < 10000; cycle++) {
    int oldest_pos = pos % 1024;
    if (pointers[oldest_pos].p != nullptr) {
      BuddyAlloc::deallocate_memory(pointers[oldest_pos].p, pointers[oldest_pos].s);
    }
    int pow_spread_from = exact_log2(os::vm_page_size());
    int pow_spread_to = exact_log2(32 * M);
    int log_2newsize = pow_spread_from + (os::random() % (pow_spread_to - pow_spread_from));
    size_t newsize = (size_t)1 << log_2newsize;
    address p = (address)BuddyAlloc::allocate_memory(newsize);
    assert(p != nullptr, "sanity");
    p[0] = 41;
    p[newsize - 1] = 42;
    pointers[oldest_pos].p = p;
    pointers[oldest_pos].s = newsize;
    oldest_pos ++;
  }

  BuddyAlloc::cleanup();
}
