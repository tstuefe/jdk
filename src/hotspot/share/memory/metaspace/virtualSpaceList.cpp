/*
 * Copyright (c) 2018, 2023, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2018, 2021 SAP SE. All rights reserved.
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
#include "logging/log.hpp"
#include "memory/metaspace.hpp"
#include "memory/metaspace/chunkManager.hpp"
#include "memory/metaspace/commitLimiter.hpp"
#include "memory/metaspace/counters.hpp"
#include "memory/metaspace/freeChunkList.hpp"
#include "memory/metaspace/metaspaceContext.hpp"
#include "memory/metaspace/metaspaceCommon.hpp"
#include "memory/metaspace/virtualSpaceList.hpp"
#include "memory/metaspace/virtualSpaceNode.hpp"
#include "runtime/atomic.hpp"
#include "runtime/mutexLocker.hpp"

namespace metaspace {

#define LOGFMT         "VsList @" PTR_FORMAT " (%s)"
#define LOGFMT_ARGS    p2i(this), this->_name

// Create a new, empty, expandable list.
VirtualSpaceList::VirtualSpaceList(const char* name, CommitLimiter* commit_limiter) :
  _name(name),
  _first_node(nullptr),
  _can_expand(true),
  _commit_limiter(commit_limiter),
  _reserved_words_counter(),
  _committed_words_counter()
{
}

// Create a new list. The list will contain one node only, which uses the given ReservedSpace.
// It will be not expandable beyond that first node.
VirtualSpaceList::VirtualSpaceList(const char* name, ReservedSpace rs, CommitLimiter* commit_limiter) :
  _name(name),
  _first_node(nullptr),
  _can_expand(false),
  _commit_limiter(commit_limiter),
  _reserved_words_counter(),
  _committed_words_counter()
{
  // Create the first node spanning the existing ReservedSpace. This will be the only node created
  // for this list since we cannot expand.
  VirtualSpaceNode* vsn = VirtualSpaceNode::create_node(rs, _commit_limiter,
                                                        &_reserved_words_counter, &_committed_words_counter);
  assert(vsn != nullptr, "node creation failed");
  _first_node = vsn;
  _first_node->set_next(nullptr);
  _nodes_counter.increment();
}

VirtualSpaceList::~VirtualSpaceList() {
  assert_lock_strong(Metaspace_lock);
  // Delete every single mapping in this list.
  // Please note that this only gets executed during gtests under controlled
  // circumstances, so we do not have any concurrency issues here. The "real"
  // lists in metaspace are immortal.
  VirtualSpaceNode* vsn = _first_node;
  VirtualSpaceNode* vsn2 = vsn;
  while (vsn != nullptr) {
    vsn2 = vsn->next();
    delete vsn;
    vsn = vsn2;
  }
}

// Create a new node and append it to the list. After
// this function, _current_node shall point to a new empty node.
// List must be expandable for this to work.
void VirtualSpaceList::create_new_node(size_t word_size) {
  assert(_can_expand, "List is not expandable");
  assert_lock_strong(Metaspace_lock);
  assert_is_aligned(word_size, chunklevel::MAX_CHUNK_WORD_SIZE);

  VirtualSpaceNode* vsn = VirtualSpaceNode::create_node(word_size,
                                                        _commit_limiter,
                                                        &_reserved_words_counter, &_committed_words_counter);
  vsn->set_next(_first_node);
  Atomic::release_store(&_first_node, vsn);
  _nodes_counter.increment();
}

// Allocate a root chunk from this list.
// Note: this just returns a chunk whose memory is reserved; no memory is committed yet.
// Hence, before using this chunk, it must be committed.
// Also, no limits are checked, since no committing takes place.
Metachunk*  VirtualSpaceList::allocate_root_chunk() {
  assert_lock_strong(Metaspace_lock);

  Metachunk* c = _salvaged_root_chunks.pop_front();
  if (c != nullptr) {
    return c;
  }

  if (_first_node == nullptr ||
      _first_node->free_words() < chunklevel::MAX_CHUNK_WORD_SIZE) {

#ifdef ASSERT
    // Since all allocations from a VirtualSpaceNode happen in
    // root-chunk-size units, and the node size must be root-chunk-size aligned,
    // we should never have left-over space.
    if (_first_node != nullptr) {
      assert(_first_node->free_words() == 0, "Sanity");
    }
#endif

    if (_can_expand) {
      create_new_node(Settings::virtual_space_node_default_word_size());
      UL2(debug, "added new node (now: %d).", num_nodes());
    } else {
      UL(debug, "list cannot expand.");
      return nullptr; // We cannot expand this list.
    }
  }

  c = _first_node->allocate_root_chunk();
  assert(c != nullptr, "This should have worked");

  return c;
}

// Helper function; salvage all remaining root chunks from the
// first node.
void VirtualSpaceList::salvage_first_node() {
  Metachunk* c = nullptr;
  do {
    c = _first_node->allocate_root_chunk();
    if (c != nullptr) {
      _salvaged_root_chunks.push_back(c);
    }
  } while (c != nullptr);
}

// Allocate a series of adjacent root chunks from this list.
// Note: As with allocate_root_chunk(), no memory is committed; this works on reserved space only.
// Returns true and a list of root chunks in &out if it succeded, false if it failed.
bool VirtualSpaceList::allocate_multiple_root_chunks(int num, MetachunkList* out) {
  assert_lock_strong(Metaspace_lock);
  assert(num > 1, "Sanity");

  UL2(debug, "Allocating multiple adjacent root chunks (%d)...", num);

  const size_t needed_words = num * chunklevel::MAX_CHUNK_WORD_SIZE;

  // Try to allocate from current node. If current node remaining address space is too small,
  // or it does not exist, create a new node. If old node was not fully used up, salvage it
  // first.
  if (_first_node == nullptr || _first_node->free_words() < needed_words) {
    if (_can_expand) {
      if (_first_node != nullptr) {
        salvage_first_node();
      }
      const size_t node_size = MAX2(needed_words, Settings::virtual_space_node_default_word_size());
      create_new_node(node_size);
      UL2(debug, "added new node (word size: " SIZE_FORMAT ") (now: %d).", node_size, num_nodes());
    } else {
      // Non-expandable list (e.g. ClassSpace).
      UL2(debug, "list cannot expand (needed word size: " SIZE_FORMAT ").", needed_words);
      return false; // We cannot expand this list.
    }
  }

  for (int i = 0; i < num; i ++) {
    Metachunk* c = _first_node->allocate_root_chunk();
    // We made sure the current node is large enough, so allocation should work.
    assert(c != nullptr, "Should have worked");
    out->push_back(c);
  }

  return true;
}

// Print all nodes in this space list.
void VirtualSpaceList::print_on(outputStream* st) const {
  MutexLocker fcl(Metaspace_lock, Mutex::_no_safepoint_check_flag);

  st->print_cr("vsl %s:", _name);
  const VirtualSpaceNode* vsn = _first_node;
  int n = 0;
  while (vsn != nullptr) {
    st->print("- node #%d: ", n);
    vsn->print_on(st);
    vsn = vsn->next();
    n++;
  }
  st->print_cr("- total %d nodes, " SIZE_FORMAT " reserved words, " SIZE_FORMAT " committed words.",
               n, reserved_words(), committed_words());
}

#ifdef ASSERT
void VirtualSpaceList::verify_locked() const {
  assert_lock_strong(Metaspace_lock);
  assert(_name != nullptr, "Sanity");

  int n = 0;

  if (_first_node != nullptr) {
    size_t total_reserved_words = 0;
    size_t total_committed_words = 0;
    const VirtualSpaceNode* vsn = _first_node;
    while (vsn != nullptr) {
      n++;
      vsn->verify_locked();
      total_reserved_words += vsn->word_size();
      total_committed_words += vsn->committed_words();
      vsn = vsn->next();
    }
    _nodes_counter.check(n);
    _reserved_words_counter.check(total_reserved_words);
    _committed_words_counter.check(total_committed_words);
  } else {
    _reserved_words_counter.check(0);
    _committed_words_counter.check(0);
  }
}

void VirtualSpaceList::verify() const {
  MutexLocker fcl(Metaspace_lock, Mutex::_no_safepoint_check_flag);
  verify_locked();
}
#endif

// Returns true if this pointer is contained in one of our nodes.
bool VirtualSpaceList::contains(const MetaWord* p) const {
  // Note: needs to work without locks.
  const VirtualSpaceNode* vsn = Atomic::load_acquire(&_first_node);
  while (vsn != nullptr) {
    if (vsn->contains(p)) {
      return true;
    }
    vsn = vsn->next();
  }
  return false;
}

// Convenience methods to return the global class-space chunkmanager
//  and non-class chunkmanager, respectively.
VirtualSpaceList* VirtualSpaceList::vslist_class() {
  return MetaspaceContext::context_class() == nullptr ? nullptr : MetaspaceContext::context_class()->vslist();
}

VirtualSpaceList* VirtualSpaceList::vslist_nonclass() {
  return MetaspaceContext::context_nonclass() == nullptr ? nullptr : MetaspaceContext::context_nonclass()->vslist();
}

} // namespace metaspace
