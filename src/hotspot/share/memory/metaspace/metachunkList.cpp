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
#include "memory/metaspace/dllist.inline.hpp"
#include "memory/metaspace/metachunkList.hpp"
#include "memory/metaspace/metaspaceCommon.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/ostream.hpp"

namespace metaspace {

#ifdef ASSERT
void MetachunkList::verify() const {
  MetachunkListType::verify();
  auto chunk_verifier = [] (const Metachunk* c) { c->verify(); };
  for_each(chunk_verifier);
}
#endif // ASSERT

size_t MetachunkList::calc_committed_word_size() const {
  size_t s = 0;
  auto count = [&s] (const Metachunk* c) { s += c->committed_words(); };
  for_each(count);
  return s;
}

size_t MetachunkList::calc_word_size() const {
  size_t s = 0;
  auto counter = [&s] (const Metachunk* c) { s += c->word_size(); };
  for_each(counter);
  return s;
}

void MetachunkList::print_on(outputStream* st) const {
  if (count() > 0) {
    auto printer = [st] (const Metachunk* c) {
      st->print(" - <");
      c->print_on(st);
      st->print(">");
    };
    for_each(printer);
    st->print(" - total : %d chunks.", count());
  } else {
    st->print("empty");
  }
}

// Look for the chunk containing the given pointer
const Metachunk* MetachunkList::find_chunk_containing(const MetaWord* p) const {
  auto finder = [p](const Metachunk* c) { return c->contains(p); };
  const Metachunk* result = for_each_until(finder);
  return result;
}

} // namespace metaspace

