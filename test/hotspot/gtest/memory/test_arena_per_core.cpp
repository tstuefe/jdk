/*
* Copyright (c) 2026, IBM Corp.
 * Copyright (c) 2026, Oracle and/or its affiliates. All rights reserved.
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


#include "runtime/os.hpp"
#include "memory/arena/arenaBlock.hpp"
#include "memory/arena/arenaBlockCache.hpp"
#include "memory/arena/arenaSlab.hpp"
#include "memory/arena/arenaPerCore.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/macros.hpp"
#include "utilities/ostream.hpp"

#include "unittest.hpp"

#include "test_arena_helpers.hpp"

//#define LOG_PLEASE
#include "testutils.hpp"


struct PerCoreFixture {
  ArenaHeap arena_heap;
  ArenaMetadataHeap metadata_heap;
  ArenaSlabGroup slab_group;
  ArenaPerCoreDataHolder holder;
  PerCoreFixture() :
    slab_group(&arena_heap, &metadata_heap),
    holder(&metadata_heap, &slab_group) {}
};

// Every slot is aligned and padded apart; the holder has one slot per possible CPU id.
TEST_VM(Arena2_PerCore, layout) {
  PerCoreFixture f;
  ASSERT_EQ(f.holder.num_cpus(), (unsigned)os::processor_count());
  STATIC_ASSERT(sizeof(ArenaPerCoreDataHolder::Data) % DEFAULT_PADDING_SIZE == 0);
  STATIC_ASSERT(alignof(ArenaPerCoreDataHolder::Data) == DEFAULT_PADDING_SIZE);
  const ArenaPerCoreDataHolder::Data* const d = f.holder.get_current_core_data();
  ASSERT_TRUE(is_aligned(d, DEFAULT_PADDING_SIZE));
}
