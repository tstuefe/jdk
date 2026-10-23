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


#ifndef SHARE_MEMORY_ARENA_ARENASLABALLOCATOR_HPP
#define SHARE_MEMORY_ARENA_ARENASLABALLOCATOR_HPP

#include "memory/allStatic.hpp"
#include "memory/arena/arenaBlock.hpp"
#include "utilities/bitMap.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"

struct ArenaSlabAllocator {

  // Note: allocator only ever deals with const_blockptr_t, since it does
  // not write into the blocks themselves.

  virtual ~ArenaSlabAllocator() = default;

  // Given a list of blocks, return them to the associated slab
  virtual void deallocate_blocks(size_t szclass, const_blockptr_t blocks[], int num_blocks) = 0;

  // Allocate a number of blocks and return their addresses in the user-provided array;
  // returns the number of blocks actually allocated (which may be smaller if we cannot allocate)
  // The returned blocks are guaranteed to be ordered by address, low to high.
  virtual int allocate_blocks(size_t szclass, const_blockptr_t blocks[], int num_blocks) = 0;

  // Helper function for tests.
  // Returns a bitmap that maps the slab range, one bit per block, blocks in slab marked
  virtual void get_occupancy_map(BitMap& out) const = 0;

  // returns true if b is in range of this slab
  virtual bool is_in_range(const_blockptr_t b) const = 0;

};


#endif // SHARE_MEMORY_ARENA_ARENASLABALLOCATOR_HPP
