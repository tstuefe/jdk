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


#ifndef SHARE_MEMORY_ARENA_ARENABLOCKALLOCATOR
#define SHARE_MEMORY_ARENA_ARENABLOCKALLOCATOR

#include "memory/allStatic.hpp"
#include "utilities/globalDefinitions.hpp"

class outputStream;

struct ArenaBlockAllocator : public AllStatic {

  // Given a requested block size, return block of that size or larger
  static address allocate_block(size_t request_size, size_t& real_size);

  // Return a block
  static void deallocate_block(address block);

  // print statistics
  static void print_on(outputStream* os);

};

#endif // SHARE_MEMORY_ARENA_ARENABLOCKALLOCATOR
