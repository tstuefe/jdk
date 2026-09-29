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
#include "utilities/globalDefinitions.hpp"


#define BUDDY_ALLOC_IMPLEMENTATION
PRAGMA_DISABLE_GCC_WARNING("-Wzero-as-null-pointer-constant")
#include "buddy_alloc.h"

BuddyAlloc::BuddyAlloc() {
  _buddy = nullptr;
  initialize();
}

BuddyAlloc::~BuddyAlloc() {
  cleanup();
}

void* BuddyAlloc::allocate_memory(size_t s) {

  assert(_buddy != nullptr, "not yet inited");

  assert(is_power_of_2(s), "must be pow2");
  assert(s >= os::vm_page_size() && s <= total_size, "bad size");

  void* p = buddy_malloc(_buddy, s);

  assert(p != nullptr, "alloc fail");

  bool b = os::commit_memory((char*)p, s, false);
  assert(b, "commit fail");

  log_debug(arena)("allocated " PTR_FORMAT ", %zu", p2i(p), s);

  return p;

}


void BuddyAlloc::deallocate_memory(void* p, size_t s) {

  assert(_buddy != nullptr, "not yet inited");

  assert(is_power_of_2(s), "must be pow2");
  assert(s >= os::vm_page_size() && s <= total_size, "bad size");
  assert(p != nullptr, "bad ptr");

  buddy_free(_buddy, p);

  os::uncommit_memory((char*)p, s, false);

  log_debug(arena)("deallocated " PTR_FORMAT ", %zu", p2i(p), s);

}


void BuddyAlloc::initialize() {

  assert(_buddy == nullptr, "already inited");

  _arena_heap = (address) os::reserve_memory(total_size, mtChunkMmap, false);
  assert(_arena_heap != nullptr, "sanity");
  assert(is_aligned(_arena_heap, min_size), "needs to be aligned to smallest byddy size");
  assert(is_aligned(min_size, os::vm_page_size()), "needs to be aligned to page size");

  _metadata_heap_size = buddy_sizeof_alignment(total_size, min_size);
  _metadata_heap_size = align_up(_metadata_heap_size, os::vm_allocation_granularity());
  _metadata_heap = (address) os::reserve_memory(_metadata_heap_size, mtChunkMeta, false);
  bool b = os::commit_memory((char*)_metadata_heap, _metadata_heap_size, false);
  assert(b, "sanit");

  _buddy = buddy_init_alignment(_metadata_heap, _arena_heap, total_size, min_size);

  assert(_buddy != nullptr, "sanity");

  log_info(arena)("buddy initialized");
}


void BuddyAlloc::cleanup() {
  assert(_buddy != nullptr, "sanity");

  os::release_memory((char*)_metadata_heap, _metadata_heap_size);
  os::release_memory((char*)_arena_heap, total_size);

  _buddy = nullptr;
}
