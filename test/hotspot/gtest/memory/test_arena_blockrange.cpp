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

template <typename IndexType>
static void do_test(IndexType block_size) {
  static constexpr uintptr_t fake_address_i = LP64_ONLY(32 * G) NOT_LP64(G);
  const const_blockptr_t fake_address = (const_blockptr_t)fake_address_i;
  constexpr IndexType num_blocks = 1024;
  BlockRange<IndexType> range(fake_address, block_size, num_blocks);
  static constexpr IndexType zero = ((IndexType)0);
  ASSERT_TRUE(range.is_valid_block_index(zero));
  ASSERT_TRUE(range.is_valid_block_index(num_blocks - 1));
  ASSERT_FALSE(range.is_valid_block_index(num_blocks));
  ASSERT_TRUE(range.is_valid_block_index_incl(zero));
  ASSERT_TRUE(range.is_valid_block_index_incl(num_blocks - 1));
  ASSERT_TRUE(range.is_valid_block_index_incl(num_blocks));
  ASSERT_FALSE(range.is_valid_block_index_incl(num_blocks + 1));

  ASSERT_TRUE(range.is_valid_block(fake_address));
  ASSERT_TRUE(range.is_valid_block(fake_address + block_size * (num_blocks - 1)));
  ASSERT_FALSE(range.is_valid_block(fake_address + block_size * num_blocks));
  ASSERT_TRUE(range.is_valid_block_incl(fake_address + (block_size * num_blocks)));

  ASSERT_EQ(zero, range.block_to_index(fake_address));
  ASSERT_EQ(fake_address, range.index_to_block(zero));

  ASSERT_EQ(num_blocks - 1,
            range.block_to_index(range.index_to_block(num_blocks - 1)));

  ASSERT_TRUE(range.contains(fake_address));
  ASSERT_FALSE(range.contains(fake_address - 1));

  const const_address some_inner_pointer = (const_address)fake_address + block_size + block_size + 17;
  ASSERT_EQ((IndexType)2, range.index_of_containing_block(some_inner_pointer));
  ASSERT_EQ(fake_address + block_size + block_size, range.start_of_containing_block(some_inner_pointer));

}


TEST_VM(Arena2_BlockRange, aligned_base) {
  do_test<uintptr_t>(256);
  do_test<unsigned>(32 * K);
}


TEST_VM(Arena2_BlockHelpers, size_class_helpers) {
  ASSERT_EQ(ArenaBlockHelpers::size_class_to_block_size(ArenaBlockHelpers::szclass_256), 256u);
  ASSERT_EQ(ArenaBlockHelpers::size_class_to_block_size(ArenaBlockHelpers::szclass_1K), K);
  ASSERT_EQ(ArenaBlockHelpers::size_class_to_block_size(ArenaBlockHelpers::szclass_32K), 32 * K);
  ASSERT_EQ(ArenaBlockHelpers::size_class_to_block_size(ArenaBlockHelpers::szclass_64K), 64 * K);
  ASSERT_EQ(ArenaBlockHelpers::size_class_to_block_size(ArenaBlockHelpers::szclass_2M), 2 * M);

  block_size_class_t szclass_last = 0;
  for (unsigned i = 0; i <= constexpr_exact_log2(1 * M); i ++) {
    const size_t block_size = nth_bit(i);
    const block_size_class_t szclass = ArenaBlockHelpers::pow2_block_size_to_standard_size_class(block_size);
    ASSERT_LT(szclass, ArenaBlockHelpers::num_size_classes);
    ASSERT_GE(szclass, szclass_last);
    szclass_last = szclass;
  }

  ASSERT_EQ(ArenaBlockHelpers::unaligned_block_size_to_standard_size_class(1),
            ArenaBlockHelpers::szclass_256);
  ASSERT_EQ(ArenaBlockHelpers::unaligned_block_size_to_standard_size_class(256u - 1),
            ArenaBlockHelpers::szclass_256);
  ASSERT_EQ(ArenaBlockHelpers::unaligned_block_size_to_standard_size_class(256u),
            ArenaBlockHelpers::szclass_256);

  ASSERT_EQ(ArenaBlockHelpers::unaligned_block_size_to_standard_size_class(256u + 1),
            ArenaBlockHelpers::szclass_1K);
  ASSERT_EQ(ArenaBlockHelpers::unaligned_block_size_to_standard_size_class(K - 1),
            ArenaBlockHelpers::szclass_1K);
  ASSERT_EQ(ArenaBlockHelpers::unaligned_block_size_to_standard_size_class(K),
            ArenaBlockHelpers::szclass_1K);

  ASSERT_EQ(ArenaBlockHelpers::unaligned_block_size_to_standard_size_class(K + 1),
            ArenaBlockHelpers::szclass_32K);
  ASSERT_EQ(ArenaBlockHelpers::unaligned_block_size_to_standard_size_class(4 * K),
            ArenaBlockHelpers::szclass_32K);
  ASSERT_EQ(ArenaBlockHelpers::unaligned_block_size_to_standard_size_class(32 * K - 1),
            ArenaBlockHelpers::szclass_32K);
  ASSERT_EQ(ArenaBlockHelpers::unaligned_block_size_to_standard_size_class(32 * K),
            ArenaBlockHelpers::szclass_32K);

  ASSERT_EQ(ArenaBlockHelpers::unaligned_block_size_to_standard_size_class(32 * K + 1),
            ArenaBlockHelpers::szclass_64K);

  ASSERT_EQ(ArenaBlockHelpers::unaligned_block_size_to_standard_size_class(M - 1),
            ArenaBlockHelpers::szclass_1M);
  ASSERT_EQ(ArenaBlockHelpers::unaligned_block_size_to_standard_size_class(M),
            ArenaBlockHelpers::szclass_1M);

}
