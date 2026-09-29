/*
 * Copyright (c) 2011, 2026, Oracle and/or its affiliates. All rights reserved.
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
#ifndef SHARE_MEMORY_BUDDY_ALLOC_WRAPPER_HPP
#define SHARE_MEMORY_BUDDY_ALLOC_WRAPPER_HPP

#include "memory/allocation.hpp"
#include "utilities/globalDefinitions.hpp"

class outputStream;
struct buddy;

class BuddyAlloc : public CHeapObj<mtChunkMeta> {
  address _arena_heap;
  address _metadata_heap;
  size_t _metadata_heap_size;
  buddy* _buddy;

public:

  static constexpr size_t min_size = 32 * K; // todo: needs to be page size aligned.
  static constexpr size_t total_size = 4 * G;

  BuddyAlloc();
  ~BuddyAlloc();

  void initialize();
  void cleanup();
  void* allocate_memory(size_t size);
  void deallocate_memory(void* p, size_t size);

};


#endif // SHARE_MEMORY_BUDDY_ALLOC_WRAPPER_HPP

