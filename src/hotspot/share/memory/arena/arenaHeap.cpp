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

#include "memory/arena/arenaBlock.hpp"
#include "memory/arena/arenaHeap.hpp"

#include <sys/mman.h>

#include "runtime/os.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/macros.hpp"
#include "utilities/ostream.hpp"

// Basically a fake implementation, real implementation comes later.
// But it already does the important stuff (reserve, commit, release, madvise for yes no THP)

static constexpr size_t max_heap = 4 * G;
static constexpr size_t thp_page_size = 2 * M;

ArenaHeap::ArenaHeap() {
  _base = (address) os::reserve_memory(max_heap, mtChunk, false);
  assert(_base != nullptr, "OOM");
  _size = max_heap;
}

ArenaHeap::~ArenaHeap() {
  os::release_memory((char*)_base, max_heap);
}

void ArenaHeap::commit_memory(const_blockptr_t from, const_blockptr_t to, bool large_pages){
  const size_t sz = to - from;

  assert(contains(from), "Sanity");
  assert(contains(to), "Sanity");
  assert(sz > 0, "Sanity");
  assert(is_aligned(from, large_pages ? thp_page_size : os::vm_page_size()), "Sanity");
  assert(is_aligned(to, large_pages ? thp_page_size : os::vm_page_size()), "Sanity");

  const bool b = os::commit_memory((char*)from, sz, false);
  assert(b, "OOM?");
#ifdef LINUX
  const int advise = large_pages ? MADV_HUGEPAGE : MADV_NOHUGEPAGE;
  const int rc = madvise((void*)from, sz, advise);
  assert(rc == 0, "madvise failed");
#endif
}

void ArenaHeap::discard_memory(const_blockptr_t from, const_blockptr_t to, bool large_pages) {
  const size_t sz = to - from;

  assert(contains(from), "Sanity");
  assert(contains(to), "Sanity");
  assert(sz > 0, "Sanity");
  assert(is_aligned(from, large_pages ? thp_page_size : os::vm_page_size()), "Sanity");
  assert(is_aligned(to, large_pages ? thp_page_size : os::vm_page_size()), "Sanity");

  const int rc = madvise((void*)from, sz, MADV_DONTNEED);
  assert(rc == 0, "madvise MADV_DONTNEED failed");
}
