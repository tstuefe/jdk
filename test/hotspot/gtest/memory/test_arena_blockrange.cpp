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
#include "utilities/globalDefinitions.hpp"
#include "unittest.hpp"

template <size_t szclass>
static void do_test() {
  static constexpr uintptr_t fake_address_i = LP64_ONLY(32 * G) NOT_LP64(G);
  const const_blockptr_t fake_address = (const_blockptr_t)fake_address_i;
  constexpr int num_blocks = 1024;
  BlockRange<szclass> range(fake_address, num_blocks);
  ASSERT_TRUE(range.is_valid_block_index(0));
  ASSERT_TRUE(range.is_valid_block_index(num_blocks - 1));
  ASSERT_FALSE(range.is_valid_block_index(num_blocks));
  ASSERT_TRUE(range.is_valid_block_incl(0));
  ASSERT_TRUE(range.is_valid_block_incl(num_blocks - 1));
  ASSERT_TRUE(range.is_valid_block_incl(num_blocks));
  ASSERT_FALSE(range.is_valid_block_incl(num_blocks + 1));

  ASSERT_EQ(0, range.block_to_index(fake_address));
  ASSERT_EQ(fake_address, range.index_to_block(0));

  ASSERT_EQ(num_blocks - 1,
            range.block_to_index(range.index_to_block(num_blocks - 1)));

  ASSERT_TRUE(range.contains(fake_address));
  ASSERT_FALSE(range.contains(fake_address - 1));

  const const_address some_inner_pointer = (const_address)fake_address + szclass + szclass + 17;
  ASSERT_EQ(2, range.index_of_containing_block(some_inner_pointer));
  ASSERT_EQ(fake_address + szclass + szclass, range.start_of_containing_block(some_inner_pointer));

}


TEST_VM(Arena2_BlockRange, aligned_base) {
  do_test<256>();
}

TEST_VM(Arena2_BlockRange, unaligned_base) {
  do_test<32 * K>();
}
