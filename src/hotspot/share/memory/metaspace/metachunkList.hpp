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

#ifndef SHARE_MEMORY_METASPACE_METACHUNKLIST_HPP
#define SHARE_MEMORY_METASPACE_METACHUNKLIST_HPP

#include "memory/metaspace/metachunk.hpp"
#include "memory/metaspace/dllist.inline.hpp"
#include "utilities/globalDefinitions.hpp"

class outputStream;

namespace metaspace {

typedef DlList<Metachunk> MetachunkListType;

// A simple list of chunks.
class MetachunkList : public MetachunkListType {
public:

#ifdef ASSERT
  void verify() const override;
#endif

  size_t calc_committed_word_size() const;
  size_t calc_word_size() const;

  // Look for the chunk containing the given pointer
  const Metachunk* find_chunk_containing(const MetaWord* p) const;

  void print_on(outputStream* st) const;

};

} // namespace metaspace

#endif // SHARE_MEMORY_METASPACE_METACHUNKLIST_HPP
