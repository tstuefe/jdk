/*
 * Copyright (c) 2023 SAP SE. All rights reserved.
 * Copyright (c) 2023, Oracle and/or its affiliates. All rights reserved.
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
#include "memory/metaspace/metaspaceArena.hpp"
#include "memory/metaspace/metaspaceSettings.hpp"
#include "memory/metaspace/testHelpers.hpp"

//#define LOG_PLEASE
#include "metaspaceGtestCommon.hpp"
#include "metaspaceGtestContexts.hpp"

//#include "testutils.hpp"
#include "unittest.hpp"

using namespace metaspace::chunklevel;
using metaspace::MetaspaceTestArena;
using metaspace::Settings;

static const size_t sizes[] = {
  MAX_CHUNK_WORD_SIZE - 1,
  MAX_CHUNK_WORD_SIZE,
  MAX_CHUNK_WORD_SIZE + 1,
  MAX_CHUNK_WORD_SIZE * 4,
  MAX_CHUNK_WORD_SIZE * 4 + MAX_CHUNK_WORD_SIZE / 5
};
static const int num_sizes = sizeof(sizes) / sizeof(size_t);

static void assert_usage_numbers(MetaspaceTestArena* arena, size_t reserved, size_t committed, size_t used) {
  size_t u = 0, c = 0, r = 0;
  arena->arena()->usage_numbers(&u, &c, &r);
  EXPECT_EQ(u, used);
  EXPECT_EQ(c, committed);
  EXPECT_EQ(r, reserved);
}

TEST_VM(metaspace, HumongousAllocateDeallocate) {

  // Allocate and (prematurely) deallocate a big block repeatedly. The first time
  // it should be carved from virtual space, all subsequent times it should come
  // from the free block list belonging to the arena. Since free block list memory
  // counts still as used, usage numbers for the arena should not change after the
  // initial allocation.

  MetaspaceGtestContext context;

  for (int i = 0; i < num_sizes; i++) {
    const size_t s = sizes[i];

    MetaspaceTestArena* arena = context.create_arena(Metaspace::StandardMetaspaceType);
    assert_usage_numbers(arena, 0, 0, 0);

    const size_t expected_reserved = align_up(s, MAX_CHUNK_WORD_SIZE);
    const size_t expected_committed = align_up(s, Settings::commit_granule_words());
    const size_t expected_used = s;

    for (int repeat = 0; repeat < 10; repeat ++) {
      MetaWord* p = arena->allocate(sizes[i]);
      ASSERT_NOT_NULL(p);
      assert_usage_numbers(arena, expected_reserved, expected_committed, expected_used);
      arena->deallocate(p, s);
      assert_usage_numbers(arena, expected_reserved, expected_committed, expected_used);
    }
  }
}

TEST_VM(metaspace, HumongousAllocateRelease) {

  // Allocate a big block repeatedly, then let arena die. The first time
  // block should be carved from virtual space, arena death should retire the chunks to
  // the chunk manager inside the MetaspaceGtestContext; all subsequent allocations
  // we should see the allocation coming from the chunk manager.

  MetaspaceGtestContext context;

  for (int i = 0; i < num_sizes; i++) {
    const size_t s = sizes[i];

    for (int repeat = 0; repeat < 10; repeat ++) {

      MetaspaceTestArena* arena = context.create_arena(Metaspace::StandardMetaspaceType);
      assert_usage_numbers(arena, 0, 0, 0);

      MetaWord* p = arena->allocate(sizes[i]);
      ASSERT_NOT_NULL(p);
      assert_usage_numbers(arena, expected_reserved, expected_committed, expected_used);
      arena->deallocate(p, s);
      assert_usage_numbers(arena, expected_reserved, expected_committed, expected_used);

      const size_t expected_reserved = align_up(s, MAX_CHUNK_WORD_SIZE);
      const size_t expected_committed = align_up(s, Settings::commit_granule_words());
      const size_t expected_used = s;


    }
  }
}
