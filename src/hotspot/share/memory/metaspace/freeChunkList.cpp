/*
 * Copyright (c) 2020, 2023, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2020, 2022 SAP SE. All rights reserved.
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
#include "memory/metaspace/freeChunkList.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/ostream.hpp"

namespace metaspace {

#ifdef ASSERT
void FreeChunkList::verify() const {
  _list.verify();
  const Metachunk* last = nullptr;
  auto verifier = [&last] (const Metachunk* c) {
    assert(c->is_free(), "Chunks in freelist should be free");
    assert(c->used_words() == 0, "Chunk in freelist should have not used words.");
    if (last != nullptr) {
      assert(last->level() == c->level(), "wrong level");
      if (last->committed_words() == 0) {
        assert(c->committed_words() == 0, "unordered");
      }
    }
    last = c;
  };
  _list.for_each(verifier);
}
#endif // ASSERT

// Returns total size in all lists (regardless of commit state of underlying memory)
size_t FreeChunkListVector::word_size() const {
  size_t sum = 0;
  for (chunklevel_t l = chunklevel::LOWEST_CHUNK_LEVEL; l <= chunklevel::HIGHEST_CHUNK_LEVEL; l++) {
    sum += list_for_level(l)->num_chunks() * chunklevel::word_size_for_level(l);
  }
  return sum;
}

// Calculates total number of committed words over all chunks (walks chunks).
size_t FreeChunkListVector::calc_committed_word_size() const {
  size_t sum = 0;
  for (chunklevel_t l = chunklevel::LOWEST_CHUNK_LEVEL; l <= chunklevel::HIGHEST_CHUNK_LEVEL; l++) {
    sum += calc_committed_word_size_at_level(l);
  }
  return sum;
}

size_t FreeChunkListVector::calc_committed_word_size_at_level(chunklevel_t lvl) const {
  return list_for_level(lvl)->calc_committed_word_size();
}

// Returns total committed size in all lists
int FreeChunkListVector::num_chunks() const {
  int n = 0;
  for (chunklevel_t l = chunklevel::LOWEST_CHUNK_LEVEL; l <= chunklevel::HIGHEST_CHUNK_LEVEL; l++) {
    n += list_for_level(l)->num_chunks();
  }
  return n;
}

// Look for a chunk: starting at level, up to and including max_level,
//  return the first chunk whose committed words >= min_committed_words.
// Return null if no such chunk was found.
Metachunk* FreeChunkListVector::search_chunk_ascending(chunklevel_t level, chunklevel_t max_level, size_t min_committed_words) {
  assert(min_committed_words <= chunklevel::word_size_for_level(max_level),
         "min chunk size too small to hold min_committed_words");
  for (chunklevel_t l = level; l <= max_level; l++) {
    FreeChunkList* list = list_for_level(l);
    Metachunk* c = list->first_minimally_committed(min_committed_words);
    if (c != nullptr) {
      list->remove(c);
      return c;
    }
  }
  return nullptr;
}

// Look for a chunk: starting at level, down to (including) the root chunk level,
// return the first chunk whose committed words >= min_committed_words.
// Return null if no such chunk was found.
Metachunk* FreeChunkListVector::search_chunk_descending(chunklevel_t level, size_t min_committed_words) {
  for (chunklevel_t l = level; l >= chunklevel::LOWEST_CHUNK_LEVEL; l --) {
    FreeChunkList* list = list_for_level(l);
    Metachunk* c = list->first_minimally_committed(min_committed_words);
    if (c != nullptr) {
      list->remove(c);
      return c;
    }
  }
  return nullptr;
}

// Look for a root chunk that has <num> adjacent root chunks following in memory
Metachunk* FreeChunkListVector::search_adjacent_root_chunks(int num) {

  FreeChunkList* rootchunks = list_for_level(chunklevel::ROOT_CHUNK_LEVEL);
  if (rootchunks->num_chunks() < num) {
    return nullptr;
  }

  // All chunks are wired up to their adjacent in-memory neighbors via
  // next/prev_in_vs().
  Metachunk* c = nullptr;

  // Look for a free root chunk that has num-1 equally free root chunks
  // adjacent in memory.
  auto finder = [&c, num](Metachunk* candidate) {
    int len = 0;
    for (const Metachunk* f = candidate->next_in_vs();
          f != nullptr && f->is_root_chunk() && f->is_free();
          f = f->next_in_vs()) {
      if (++len == num) {
        c = candidate;
        return true;
      }
    }
    return false;
  };
  rootchunks->_list.for_each_until(finder);
  return c;

}

void FreeChunkListVector::print_on(outputStream* st) const {
  for (chunklevel_t l = chunklevel::LOWEST_CHUNK_LEVEL; l <= chunklevel::HIGHEST_CHUNK_LEVEL; l++) {
    st->print("-- List[" CHKLVL_FORMAT "]: ", l);
    list_for_level(l)->print_on(st);
    st->cr();
  }
  st->print_cr("total chunks: %d, total word size: " SIZE_FORMAT ".",
               num_chunks(), word_size());
}

#ifdef ASSERT

void FreeChunkListVector::verify() const {
  for (chunklevel_t l = chunklevel::LOWEST_CHUNK_LEVEL; l <= chunklevel::HIGHEST_CHUNK_LEVEL; l++) {
    list_for_level(l)->verify();
  }
}

bool FreeChunkListVector::contains(const Metachunk* c) const {
  for (chunklevel_t l = chunklevel::LOWEST_CHUNK_LEVEL; l <= chunklevel::HIGHEST_CHUNK_LEVEL; l++) {
    if (list_for_level(l)->contains(c)) {
      return true;
    }
  }
  return false;
}

#endif // ASSERT

} // namespace metaspace

