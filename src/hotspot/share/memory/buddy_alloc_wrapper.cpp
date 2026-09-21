/*
 * Copyright (c) 2017, 2026, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2019, 2023 SAP SE. All rights reserved.
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

#include "nmt/memTracker.inline.hpp"
#include "runtime/os.hpp"
#include "logging/log.hpp"
#include "buddy_alloc_wrapper.hpp"
#include "utilities/align.hpp"
#include "utilities/powerOfTwo.hpp"
#include "utilities/debug.hpp"
#include "utilities/ostream.hpp"

#define BUDDY_ALLOC_IMPLEMENTATION
#include "buddy_alloc.h"

static address g_memory_arena = nullptr;
static address g_memory_metadata = nullptr;
static size_t g_memory_metadata_size = 0;
static struct buddy* g_buddy = nullptr;

void* BuddyAlloc::allocate_memory(size_t s) {

  assert(g_buddy != nullptr, "not yet inited");

  assert(is_power_of_2(s), "must be pow2");
  assert(s >= os::vm_page_size() && s <= total_size, "bad size");

  void* p = buddy_malloc(g_buddy, s);

  assert(p != nullptr, "alloc fail");

  bool b = os::commit_memory((char*)p, s, false);
  assert(b, "commit fail");

  log_debug(arena)("allocated " PTR_FORMAT ", %zu", p2i(p), s);

  return p;

}


void BuddyAlloc::deallocate_memory(void* p, size_t s) {

  assert(g_buddy != nullptr, "not yet inited");

  assert(is_power_of_2(s), "must be pow2");
  assert(s >= os::vm_page_size() && s <= total_size, "bad size");
  assert(p != nullptr, "bad ptr");

  buddy_free(g_buddy, p);

  os::uncommit_memory((char*)p, s, false);

  log_debug(arena)("deallocated " PTR_FORMAT ", %zu", p2i(p), s);

}


void BuddyAlloc::initialize() {

  assert(g_buddy == nullptr, "already inited");

  g_memory_arena = (address) os::reserve_memory(total_size, mtChunkMmap, false);
  assert(g_memory_arena != nullptr, "sanity");
  assert(is_aligned(g_memory_arena, min_size), "needs to be aligned to smallest byddy size");
  assert(is_aligned(os::vm_page_size(), min_size), "needs to be aligned to page size");

  g_memory_metadata_size = buddy_sizeof_alignment(total_size, min_size);
  g_memory_metadata_size = align_up(g_memory_metadata_size, os::vm_allocation_granularity());
  g_memory_metadata = (address) os::reserve_memory(g_memory_metadata_size, mtChunkMeta, false);
  bool b = os::commit_memory((char*)g_memory_metadata, g_memory_metadata_size, false);
  assert(b, "sanit");

  g_buddy = buddy_init_alignment(g_memory_metadata, g_memory_arena, total_size, min_size);

  assert(g_buddy != nullptr, "sanity");

  log_info(arena)("buddy initialized");


}


void BuddyAlloc::cleanup() {
  assert(g_buddy != nullptr, "sanity");

  os::release_memory((char*)g_memory_arena, total_size);
    os::release_memory((char*)g_memory_metadata, g_memory_metadata_size);
    g_buddy = nullptr;
}
