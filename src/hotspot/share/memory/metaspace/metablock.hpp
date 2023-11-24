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

  MetaWord* _base;
  size_t _word_size;

public:

  MetaBlock() : _base(nullptr), _word_size(0) {}
  MetaBlock(MetaWord* p, size_t word_size) :
    _base(p), _word_size(0) {}

  MetaWord* base() const { return _base; }
  const MetaWord* end() const { return _base + _word_size; }
  size_t word_size() const { return _word_size; }
  bool is_empty() const { return _base == nullptr; }

  MetaBlock split_off_tail(size_t head_size) const {
    MetaBlock result;
    if (!is_empty()) {
      if (head_size < _word_size) {
        const size_t tail_size = _word_size - head_size;
        result = MetaBlock(_base + head_size, tail_size);
        _word_size -= tail_size;
      }
    }
    return result;
  }
};

#define METABLOCKFORMAT             "block (@" PTR_FORMAT " size " SIZE_FORMAT ")"
#define METABLOCKFORMATARGS(__block__)  p2i((__block__).base()), (__block__).word_size()

}

#endif // SHARE_MEMORY_METASPACE_METABLOCK_HPP
