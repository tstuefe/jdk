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
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/macros.hpp"

#include "unittest.hpp"

#include "test_arena_helpers.hpp"


TEST_VM(Arena2_CacheGroup, basic) {
  ArenaHeap arena_heap;
  ArenaMetadataHeap metadata_heap;
  ArenaSlabGroup slab_group(&arena_heap, &metadata_heap);
  ArenaCacheGroup cache_group(&metadata_heap, &slab_group);

  unsigned num_checked = 0;
  auto checker = [&cache_group, &num_checked](block_size_class_t szclass) {
    const size_t block_size = ArenaBlockHelpers::size_class_to_block_size(szclass);
    ArenaBlockCache* const cache = cache_group.cache_for_size_class(szclass);
    ASSERT_NE(cache, nullptr);
    const const_blockptr_t p1 = cache->allocate_block();
    const const_blockptr_t p2 = cache->allocate_block();
    ASSERT_NE(p1, nullptr);
    ASSERT_EQ(p2, p1 + block_size);
    num_checked++;
  };
  ArenaBlockHelpers::iterate_standard_size_classes_forward(checker);
  ASSERT_EQ(num_checked, ArenaBlockHelpers::num_valid_standard_size_classes);
}

// Helper function; Given a cache group whose caches are empty, triggers
// prefetch on all of them
static void prefetch_all(ArenaCacheGroup& cache_group) {
  auto cache_filler = [&cache_group](block_size_class_t szclass) {
    ArenaBlockCache* const cache = cache_group.cache_for_size_class(szclass);
    ASSERT_EQ(cache->count(), 0);                       // Cache should be empty
    const const_blockptr_t p = cache->allocate_block(); // Prefetch should partly fill the cache
    ASSERT_NE(p, nullptr);
    ASSERT_GT(cache->count(), 0);
  };
  ArenaBlockHelpers::iterate_standard_size_classes_forward(cache_filler);
}

TEST_VM(Arena2_CacheGroup, flush_all) {
  ArenaHeap arena_heap;
  ArenaMetadataHeap metadata_heap;
  ArenaSlabGroup slab_group(&arena_heap, &metadata_heap);
  ArenaCacheGroup cache_group(&metadata_heap, &slab_group);

  prefetch_all(cache_group);

  cache_group.flush();

  auto check = [&cache_group](block_size_class_t szclass) {
    const ArenaBlockCache* const cache = cache_group.cache_for_size_class(szclass);
    ASSERT_EQ(cache->count(), 0); // should have flushed
  };
  ArenaBlockHelpers::iterate_standard_size_classes_forward(check);
}

TEST_VM(Arena2_CacheGroup, tick_all) {
  ArenaHeap arena_heap;
  ArenaMetadataHeap metadata_heap;
  ArenaSlabGroup slab_group(&arena_heap, &metadata_heap);
  ArenaCacheGroup cache_group(&metadata_heap, &slab_group);

  prefetch_all(cache_group);

  // how big are caches before tick
  unsigned num[ArenaBlockHelpers::highest_size_class + 1];
  auto take_counts = [&cache_group, &num](block_size_class_t szclass) {
    num[szclass] = cache_group.cache_for_size_class(szclass)->count();
  };
  ArenaBlockHelpers::iterate_standard_size_classes_forward(take_counts);

  // tick
  cache_group.tick();

  // Every cache should have shed at least one item
  auto check = [&cache_group, &num](block_size_class_t szclass) {
    const ArenaBlockCache* const cache = cache_group.cache_for_size_class(szclass);
    ASSERT_LT((unsigned)cache->count(), num[szclass]); // should have flushed
  };
  ArenaBlockHelpers::iterate_standard_size_classes_forward(check);
}