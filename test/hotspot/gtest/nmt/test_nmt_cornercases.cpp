/*
 * Copyright (c) 2022 SAP SE. All rights reserved.
 * Copyright (c) 2022, Oracle and/or its affiliates. All rights reserved.
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
#include "runtime/os.hpp"
#include "services/mallocHeader.inline.hpp"
#include "services/mallocTracker.hpp"
#include "services/memTracker.hpp"
#include "testutils.hpp"
#include "unittest.hpp"

static void check_expected_malloc_header(const void* payload, MEMFLAGS type, size_t size) {
  char msg[64];
  address dummy;
  const MallocHeader* hdr = MallocHeader::header_for(payload);
  bool seems_ok = hdr->check_block_integrity(msg, sizeof(msg), &dummy);
  EXPECT_TRUE(seems_ok);
  EXPECT_EQ(hdr->size(), size);
  EXPECT_EQ(hdr->flags(), type);
}

// Check that a malloc with an overflowing size is rejected.
TEST_VM(NMT, malloc_failure1) {
  void* p = os::malloc(SIZE_MAX, mtTest);
  EXPECT_NULL(p);
}

// Check that gigantic mallocs are rejected, even if no size overflow happens.
TEST_VM(NMT, malloc_failure2) {
  void* p = os::malloc(SIZE_MAX - M, mtTest);
  EXPECT_NULL(p);
}

static void check_failing_realloc(size_t failing_request_size) {

  // We test this with both NMT enabled and disabled.
  bool nmt_enabled = MemTracker::enabled();
  const size_t first_size = 0x100;

  void* p = os::malloc(first_size, mtTest);
  EXPECT_NOT_NULL(p);
  if (nmt_enabled) {
    check_expected_malloc_header(p, mtTest, first_size);
  }
  GtestUtils::mark_range(p, first_size);

  // should fail
  void* p2 = os::realloc(p, failing_request_size, mtTest);
  EXPECT_NULL(p2);

  // original allocation should still be intact
  GtestUtils::check_range(p, first_size);
  if (nmt_enabled) {
    check_expected_malloc_header(p, mtTest, first_size);
  }

  os::free(p);
}

// Check that a reallocation that would overflow is correctly rejected.
TEST_VM(NMT, realloc_failure1) {
  check_failing_realloc(SIZE_MAX);
  check_failing_realloc(SIZE_MAX - MemTracker::overhead_per_malloc());
}

// Check that a reallocation that fails because if too large size is rejected and
// that the original NMT accounting is kept intact (since a failing realloc(3) will leave
// the original block untouched).
TEST_VM(NMT, realloc_failure2) {
  check_failing_realloc(SIZE_MAX - M);
}

// Check a simple sequence of mallocs and reallocs. We expect the
// newly allocated memory to be zapped (in debug)
// while the old section should be left intact.
TEST_VM(NMT, malloc_realloc) {
  bool nmt_enabled = MemTracker::enabled();

  void* p = os::malloc(1024, mtTest);
  ASSERT_NOT_NULL(p);
  if (nmt_enabled) {
    check_expected_malloc_header(p, mtTest, 1024);
  }

#ifdef ASSERT
  GtestUtils::check_range(p, 1024, uninitBlockPad);
#endif
  GtestUtils::mark_range_with(p, 1024, '-');

  // Enlarging realloc
  void* p2 = os::realloc(p, 4096, mtTest);
  ASSERT_NOT_NULL(p2);
  if (nmt_enabled) {
    check_expected_malloc_header(p2, mtTest, 4096);
  }
  GtestUtils::check_range(p2, 1024, '-');
#ifdef ASSERT
  GtestUtils::check_range((char*)p2 + 1024, 4096 - 1024, uninitBlockPad);
#endif

  GtestUtils::mark_range_with(p2, 4096, '+');

  // Shrinking realloc
  void* p3 = os::realloc(p, 256, mtTest);
  ASSERT_NOT_NULL(p3);
  if (nmt_enabled) {
    check_expected_malloc_header(p3, mtTest, 256);
  }
#ifdef ASSERT
  GtestUtils::check_range(p3, 256, '+');
#endif

  os::free(p3);
}
