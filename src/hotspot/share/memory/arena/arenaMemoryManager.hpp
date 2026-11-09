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


#ifndef SHARE_MEMORY_ARENA_ARENAMANAGER_HPP
#define SHARE_MEMORY_ARENA_ARENAMANAGER_HPP

#include "memory/allocation.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"

class Chunk;
class outputStream;
class ArenaMemoryManagerImpl;

// The singleton which holds slabs and caches together and
// converts Chunk to Blocks
class ArenaMemoryManager {

  ArenaMemoryManagerImpl* _impl;

public:

  ArenaMemoryManager();

  void initialize();
  void cleanup(); // Tests only, the global memory manager is never released.
  bool is_initialized() const { return _impl != nullptr; }

  // Given a chain of chunks, deallocate all of them. After returning
  // from this functions, chunks are invalid and should not be accessed.
  void deallocate_chunks(const Chunk* first_chunk) const;

  // Allocate memory for a single chunk of the given size. Chunk
  // returned may be of larger size. Note that the returned memory is not
  // yet initialized.
  Chunk* allocate_chunk_storage(size_t requested_size, size_t& real_size) const;

  // Returns pointer to the global manager
  static ArenaMemoryManager& instance();

#ifdef ASSERT
  void verify() const;
#endif

  void print_on(outputStream* st) const;
};

#endif // SHARE_MEMORY_ARENA_ARENAMANAGER_HPP
