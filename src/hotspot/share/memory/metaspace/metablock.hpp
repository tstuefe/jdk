/*
 * Copyright (c) 2020, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_MEMORY_METASPACE_METABLOCK_HPP
#define SHARE_MEMORY_METASPACE_METABLOCK_HPP

#include "utilities/globalDefinitions.hpp"

namespace metaspace {

// Tiny structure to be passed by value
class MetaBlock {
  MetaWord* const _base;
  const size_t _word_size;
public:
  MetaBlock() : _base(nullptr), _word_size(0) {}
  MetaBlock(MetaWord* p, size_t word_size) :
    _base(p), _word_size(0) {}

  MetaWord* base() const { return _base; }
  const MetaWord* end() const { return _base + _word_size; }
  size_t word_size() const { return _word_size; }
  bool is_empty() const { return _base == nullptr; }

  // Return a block at the tail end of this block,
  // at a given distance from base.
  MetaBlock tail(size_t head_size) const {
    if (!is_empty()) {
      if (head_size < _word_size) {
        return MetaBlock(_base + head_size, _word_size - head_size);
      }
    }
    return MetaBlock(); // empty block
  }

  // Return a block with an aligned base. Returns empty block
  // if that is not possible.
  MetaBlock aligned_block(size_t word_alignment) const {
    if (!is_empty()) {
      MetaWord* const aligned_base = align_up(_base, word_alignment * BytesPerWord);
      if (aligned_base < end()) {
        const size_t l = _word_size - (aligned_base - _base);
        return MetaBlock(aligned_base, l);
      }
    }
    return MetaBlock(); // empty block
  }

};

}

#endif // SHARE_MEMORY_METASPACE_METABLOCK_HPP
