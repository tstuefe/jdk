/*
 * Copyright (c) 2020, 2023, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2020 SAP SE. All rights reserved.
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

#include "precompiled.hpp"
#include "memory/metaspace/freeBlocks.hpp"
#include "memory/metaspace.hpp"
#include "oops/klass.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"

namespace metaspace {

void FreeBlocks::add_block(MetaBlock block) {

  // Book into class block tree iff block can be reused for class space. That
  // is only true if the block is located in class space, is correctly aligned and
  // larger than Klass. Otherwise book either into binlist for non-class, or into
  // blocktree for non-class, depending on the size of the block.
  const bool is_class_space = Metaspace::is_in_class_space(block.base());
  const bool aligned_for_klass = is_aligned(block.base(), AllocationAlignmentWordSize);
  const bool large_enough_for_klass = block.word_size() >= sizeof(Klass);

  if (is_class_space && aligned_for_klass && large_enough_for_klass) {
    _tree_c.add_block(block);
  } else {
    if (block.word_size() >= MaxSmallBlocksWordSize) {
      _tree_nc.add_block(block);
    } else {
      _small_blocks_nc.add_block(block);
    }
  }
}

MetaBlock FreeBlocks::remove_block(size_t word_size, bool for_class) {

  // If this is a class space allocation (in which case the size should be >= sizeof Klass, so not small)
  // we look into the class-space blocktree. Otherwise either in the non-class binlist (for small allocation
  // sizes) or the non-class blocktree.
  MetaBlock result;

  int from = 0;
  if (is_class) {
    assert(word_size >= sizeof(Klass), "Sanity");
    result = _tree_c.remove_block(word_size);
  } else {
    // Non-class allocation.
    if (word_size < _small_blocks_nc.MaxWordSize) {
      result = _small_blocks_nc.remove_block(word_size);
    }
    if (result.is_empty()) {
      result = _small_blocks_nc.remove_block(word_size);
      from = 2;
    }
  }

  return result;
}

} // namespace metaspace

