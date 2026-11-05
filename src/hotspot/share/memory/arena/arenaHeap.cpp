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
#include "utilities/ostream.hpp"

// Basically a fake implementation, real implementation comes later.
// But it already does the important stuff (reserve, commit, release, madvise for yes no THP)

static constexpr size_t max_heap = 4 * G;

ArenaHeapBase::ArenaHeapBase() :
  _base([]() {
    const address p = (address) os::reserve_memory_aligned(max_heap, ArenaHeap::large_page_size, mtChunk, false);
    assert(p != nullptr, "OOM");
    return p;
  } ()),
  _end(_base + max_heap)
{}

ArenaHeapBase::~ArenaHeapBase() {
  os::release_memory((char*)_base, max_heap);
}

void ArenaHeapBase::commit_memory(const_blockptr_t from, const_blockptr_t to){
  const size_t sz = to - from;
  assert(to > from, "Sanity");
  assert(reserved_contains(from), "Sanity");
  assert(reserved_contains(to - 1), "Sanity");
  assert(is_aligned(from, os::vm_page_size()), "Sanity");
  assert(is_aligned(to, os::vm_page_size()), "Sanity");
  const bool b = os::commit_memory((char*)from, sz, false);
  assert(b, "OOM?");
}

// Given a memory range, enforce or inhibit huge pages (THPs).
void ArenaHeapBase::advise_large_page_use(const_blockptr_t from, const_blockptr_t to, bool use_large_pages) {
#ifdef LINUX
  const int advise = use_large_pages ? MADV_HUGEPAGE : MADV_NOHUGEPAGE;
  const size_t alignment = use_large_pages ? ArenaHeap::large_page_size : os::vm_page_size();
  assert(is_aligned(from, alignment), "Sanity");
  assert(is_aligned(to, alignment), "Sanity");
  const int rc = madvise((void*)from, to - from, advise);
  assert(rc == 0, "madvise failed");
#else
  (void)from; (void)to; (void)use_large_pages;
#endif
}

void ArenaHeapBase::discard_memory(const_blockptr_t from, const_blockptr_t to) {
  const size_t sz = to - from;
  assert(to > from, "Sanity");
  assert(reserved_contains(from), "Sanity");
  assert(reserved_contains(to - 1), "Sanity");
  assert(is_aligned(from, os::vm_page_size()), "Sanity");
  assert(is_aligned(to, os::vm_page_size()), "Sanity");

  // TODO
#ifdef LINUX
  const int rc = madvise((void*)from, sz, MADV_DONTNEED);
  assert(rc == 0, "madvise failed");

#elif defined(__APPLE__)
  // Use madvise(MADV_FREE) for lazy return
  // Use mmap to replace the pages for immediate return
  void* const p = mmap((void*)from, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
  assert(p == from, "Sanity");

#else
  ShouldNotReachHere();
#endif
}

/////////////

ArenaHeap::ArenaHeap() : ArenaHeapBase(), _hwm(base()) {}

// Reserves memory for a slab. Slab start will be aligned to granule size.
const_blockptr_t ArenaHeap::reserve_slab_memory(size_t block_size, size_t granule_size, unsigned num_blocks) {

  DEBUG_ONLY(ArenaBlockHelpers::verify_valid_block_size(block_size));
  assert(block_size <= granule_size, "Sanity");
  assert(granule_size >= os::vm_page_size(), "Sanity");
  assert(is_power_of_2(granule_size), "Sanity");

  // Slabs are always aligned to large page size, independent from granule size.
  // That makes dealing with commit- and release- boundaries a lot easier.
  constexpr size_t alignment = large_page_size;
  _hwm = align_up(_hwm, alignment);

  const size_t needed = block_size * num_blocks;

  const const_blockptr_t p = _hwm;
  _hwm += needed;
  assert(_hwm <= end(), "Sanity");

  return p;

}


//////////////

ArenaMetadataHeap::ArenaMetadataHeap() : ArenaHeapBase(),
  _hwm(base())
{
  // Make it a multiple of THP size - we want to have to option to back this with
  // large pages on finalize() if it makes sense.
  assert(is_aligned(reserved_size(), large_page_size), "Sanity");
  commit_memory(base(), end());
}

// Align high-water-mark up to match alignment. Next allocation will be aligned such.
void ArenaMetadataHeap::align_hwm_up(size_t alignment) {
  _hwm = align_up(_hwm, alignment);
  assert(_hwm <= end(), "Sanity");
}

void ArenaMetadataHeap::align_hwm_to_large_page_size() {
  align_hwm_up(ArenaHeapBase::large_page_size);
}

void ArenaMetadataHeap::align_hwm_to_page_size() {
  align_hwm_up(os::vm_page_size());
}

void ArenaMetadataHeap::align_hwm_to_padded_size() {
  align_hwm_up(DEFAULT_PADDING_SIZE);
}

// Reserves memory at the hwm.
address ArenaMetadataHeap::allocate(size_t size) {
  const address p = _hwm;
  _hwm += size;
  assert(_hwm <= end(), "Sanity");
  return p;
}

// Done after all allocations from the metadata heap are finished.
void ArenaMetadataHeap::finalize() {
  // Is it worth to back this with large pages?
#ifdef LINUX
  address thp_advise_boundary = align_up(_hwm, large_page_size);
  if ((size_t)(thp_advise_boundary - _hwm) > (size_t)((double)large_page_size * .25)) {
    thp_advise_boundary -= large_page_size;
  }
  if (thp_advise_boundary > base()) {
    advise_large_page_use(base(), thp_advise_boundary, true);
  }
  _hwm = MAX2(thp_advise_boundary, _hwm);
#endif // Linux
  _hwm = align_up(_hwm, os::vm_page_size());
  assert(_hwm <= end(), "Sanity");
  if (_hwm < end()) {
    os::uncommit_memory((char*)_hwm, end() - _hwm);
  }
}