/*
 * Copyright (c) 2020, 2023, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2020, 2023 SAP SE. All rights reserved.
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

#ifndef SHARE_MEMORY_METASPACE_FREECHUNKLIST_HPP
#define SHARE_MEMORY_METASPACE_FREECHUNKLIST_HPP

#include "memory/allocation.hpp"
#include "memory/metaspace/chunklevel.hpp"
#include "memory/metaspace/counters.hpp"
#include "memory/metaspace/metachunk.hpp"
#include "memory/metaspace/metachunkList.hpp"

class outputStream;

namespace metaspace {

class FreeChunkListVector;

// This is the free list underlying the ChunkManager.
//
// Chunks are kept in a vector of double-linked double-headed lists
//  (using Metachunk::prev/next). One list per chunk level exists.
//
// Chunks in these lists are roughly ordered: uncommitted chunks
//  are added to the back of the list, fully or partially committed
//  chunks to the front. We do not use a more elaborate sorting on
//  insert since that path is used during class unloading, hence timing
//  sensitive.
//
// During retrieval (at class loading), we search the list for a chunk
//  of at least n committed words to satisfy the caller requested
//  committed word size. We stop searching at the first fully uncommitted
//  chunk.
//
// Therefore in all likelihood the chunk lists only contain fully committed or
// fully uncommitted chunks; either way search will stop at the first chunk.

class FreeChunkList {
  friend class FreeChunkListVector;

  MetachunkList _list;

public:

  // Remove given chunk from anywhere in the list.
  void remove(Metachunk* c) { _list.remove(c); }

  void add(Metachunk* c) {
    assert(contains(c) == false, "Chunk already in freelist");
    assert(first() == nullptr || first()->level() == c->level(),
           "List should only contains chunks of the same level.");
    assert(c->is_free(), "chunk not free");
    // Uncommitted chunks go to the back, fully or partially committed to the front.
    if (c->committed_words() == 0) {
      _list.push_back(c);
    } else {
      _list.push_front(c);
    }
  }

  // Removes the first chunk from the list and returns it. Returns nullptr if list is empty.
  Metachunk* remove_first() { return _list.pop_front(); }

  // Returns reference to the first chunk in the list, or nullptr
  Metachunk* first() const { return const_cast<Metachunk*>(_list.front()); }

  // Returns reference to the fist chunk in the list with a committed word
  // level >= min_committed_words, or null.
  Metachunk* first_minimally_committed(size_t min_committed_words) const {
    // Since uncommitted chunks are added to the back we can stop looking once
    //  we encounter a fully uncommitted chunk.
    Metachunk* c = first();
    while (c != nullptr &&
           c->committed_words() < min_committed_words &&
           c->committed_words() > 0) {
      c = c->next();
    }
    if (c != nullptr &&
        c->committed_words() >= min_committed_words) {
      return c;
    }
    return nullptr;
  }

#ifdef ASSERT
  bool contains(const Metachunk* c) const { return _list.contains(c); }
  void verify() const;
#endif

  // Returns number of chunks
  int num_chunks() const { return _list.count(); }

  // Calculates total number of committed words over all chunks (walks chunks).
  size_t calc_committed_word_size() const { return _list.calc_committed_word_size(); }

  void print_on(outputStream* st) const { _list.print_on(st); }

};

// A vector of free chunk lists, one per chunk level
class FreeChunkListVector {

  FreeChunkList _lists[chunklevel::NUM_CHUNK_LEVELS];

  const FreeChunkList* list_for_level(chunklevel_t lvl) const         { DEBUG_ONLY(chunklevel::check_valid_level(lvl)); return _lists + lvl; }
  FreeChunkList* list_for_level(chunklevel_t lvl)                     { DEBUG_ONLY(chunklevel::check_valid_level(lvl)); return _lists + lvl; }

  const FreeChunkList* list_for_chunk(const Metachunk* c) const       { return list_for_level(c->level()); }
  FreeChunkList* list_for_chunk(const Metachunk* c)                   { return list_for_level(c->level()); }

public:

  // Remove given chunk from its list. List must contain that chunk.
  void remove(Metachunk* c) {
    list_for_chunk(c)->remove(c);
  }

  // Remove first node unless empty. Returns node or null.
  Metachunk* remove_first(chunklevel_t lvl) {
    Metachunk* c = list_for_level(lvl)->remove_first();
    return c;
  }

  void add(Metachunk* c) {
    list_for_chunk(c)->add(c);
  }

  // Returns number of chunks for a given level.
  int num_chunks_at_level(chunklevel_t lvl) const {
    return list_for_level(lvl)->num_chunks();
  }

  // Returns reference to first chunk at this level, or nullptr if sublist is empty.
  Metachunk* first_at_level(chunklevel_t lvl) const {
    return list_for_level(lvl)->first();
  }

  // Look for a chunk: starting at level, up to and including max_level,
  //  return the first chunk whose committed words >= min_committed_words.
  // Return null if no such chunk was found.
  Metachunk* search_chunk_ascending(chunklevel_t level, chunklevel_t max_level,
                                    size_t min_committed_words);

  // Look for a chunk: starting at level, down to (including) the root chunk level,
  // return the first chunk whose committed words >= min_committed_words.
  // Return null if no such chunk was found.
  Metachunk* search_chunk_descending(chunklevel_t level, size_t min_committed_words);

  // Look for a root chunk that has <num> adjacent root chunks following in memory
  Metachunk* search_adjacent_root_chunks(int num);

  // Returns total size in all lists (including uncommitted areas)
  size_t word_size() const;

  // Calculates total number of committed words over all chunks (walks chunks).
  size_t calc_committed_word_size_at_level(chunklevel_t lvl) const;

  // Calculates total number of committed words over all chunks (walks chunks).
  size_t calc_committed_word_size() const;

  // Returns number of chunks in all lists
  int num_chunks() const;

#ifdef ASSERT
  bool contains(const Metachunk* c) const;
  void verify() const;
#endif

  void print_on(outputStream* st) const;

};

} // namespace metaspace

#endif // SHARE_MEMORY_METASPACE_FREECHUNKLIST_HPP
