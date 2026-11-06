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

  ArenaMemoryManagerImpl* const _impl;

  ArenaMemoryManager(ArenaMemoryManagerImpl* impl);

public:

  ArenaMemoryManager();
  ~ArenaMemoryManager();

  void initialize();
  void cleanup();

  // Given a chain of chunks, deallocate all of them. After returning
  // from this functions, chunks are invalid and should not be accessed.
  // The arena_size_hint is a (upper ceiling) of the total size all chunks
  // together are expected to have.
  void deallocate_chunks(Chunk* chunk, size_t arena_size_hint);

  // Allocate a single chunk of the given size. Chunk
  // returned may be of larger size.
  Chunk* allocate_chunk(size_t requested_size);

  // Returns pointer to the global manager
  static ArenaMemoryManager* the_manager();

};

#endif // SHARE_MEMORY_ARENA_ARENAMANAGER_HPP
