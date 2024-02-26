/*
 * Copyright (c) 2024, Red Hat, Inc. All rights reserved.
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
#include "nmt/vmaTree.hpp"
#include "utilities/ostream.hpp"
#include "nmt/libdict/rb_tree.h"
#include "nmt/libdict/tree_common.h"

typedef uint16_t MappingState;
static const MappingState NONE_STATE = 0;

class MappingStateChange {
  union {
    void* raw;
    struct {
      MappingState state_in;  // "incoming" state (active up to address)
      MappingState state_out; // "outgoing" state (active from address on)
    } data;
  } _v;
  // Note the special value 0 means "this is an end of a range followed by nothing"
  STATIC_ASSERT(sizeof(_v) == sizeof(void*));

public:

  MappingStateChange(void* raw)       { _v.raw = raw; }
  MappingStateChange()                { _v.raw = nullptr; }
  void* raw() const                   { return _v.raw; }

  MappingState state_out() const      { return _v.data.state_out; }
  MappingState state_in() const       { return _v.data.state_in; }

  void set_state_out(MappingState s)  { _v.data.state_out = s; }
  void set_state_in(MappingState s)   { _v.data.state_in = s; }

  // A state change is noop if old and new state are identical
  bool is_noop() const {
    return state_in() == state_out();
  }
};

static const char* const vmastate_text[3] = { "000?", "reserved", "committed" };

// Utility class to make it easier to deal with datum
class VMANode {
  rb_node* const _node;
public:
  VMANode(rb_node* node) : _node(node)      {}
  VMANode(rb_itor* it) : _node(it->_node)   {}

  address addr() const { return (address)_node->key; }

  MappingStateChange state_change() const {
    return MappingStateChange(_node->datum);
  }

  void set_state_change(MappingStateChange sc) const {
    _node->datum = sc.raw();
  }

  MappingState state_in() const   { return state_change().state_in(); }
  MappingState state_out() const  {  return state_change().state_out(); }

  void set_state_in(MappingState s)  {
    MappingStateChange sc = state_change();
    sc.set_state_in(s);
    set_state_change(sc);
  }

  void set_state_out(MappingState s)  {
    MappingStateChange sc = state_change();
    sc.set_state_out(s);
    set_state_change(sc);
  }

  bool is_noop() const {
    return state_change().is_noop();
  }
};

class VMAMappingState {
  const MappingState _state;
public:
  VMAMappingState(MEMFLAGS f, VMAState s) : _state(((uint16_t)f << 8) | (uint8_t) s) {}
  VMAMappingState(MappingState s) : _state(s) {}
  MEMFLAGS f() const { return (MEMFLAGS)(_state >> 8); }
  VMAState s() const { return (VMAState)((uint8_t)_state); }
  MappingState state() const { return _state; }
};

static int key_compare_func(const void* a, const void* b) {
  if (a == b) {
    return 0;
  } else {
    return a > b ? 1 : -1;
  }
}

class VMATree {
  rb_tree* _tree;

  // insert new node when there is none existing.
  void insert_new_node(address addr, MappingStateChange sc) {
    dict_insert_result rc = rb_tree_insert(_tree, addr);
    assert(rc.inserted, "Not inserted");
    (*(rc.datum_ptr)) = sc.raw();
  }

  // insert new node when there is none existing.
  void insert_or_update_node(address addr, MappingStateChange sc) {
    dict_insert_result rc = rb_tree_insert(_tree, addr);
    (*(rc.datum_ptr)) = sc.raw();
  }

  // Note: could also a unmapped region, if state_now is "NONE_STATE"
  void register_mapping(address A, address B, MappingState state_now) {

    log_trace(nmtvma)("Adding " PTR_FORMAT " .. " PTR_FORMAT " (%.4x)", p2i(A), p2i(B), state_now);

    assert(B > A, "no empty ranges");
    rb_itor* it = rb_itor_new(_tree);
    assert(it != nullptr, "new");

    bool rc;

    ///////////////////////////
    // First, handle A.
    MappingStateChange sc_A;
    sc_A.set_state_in(NONE_STATE);
    sc_A.set_state_out(state_now);    // A is begin of the range, so "state_now" is the outgoing state

    // This will be the state active at B.out
    MappingStateChange sc_B;
    sc_B.set_state_in(state_now);     // B is begin of the range, so "state_now" is the incoming state
    sc_B.set_state_out(NONE_STATE);

    // We search for the node preceding A:
    if (rb_itor_search_le(it, A)) {
      VMANode n(it);

      // Unless we know better, let B's outgoing state be the outgoing state of the node at or preceding A.
      sc_B.set_state_out(n.state_out());

      // Direct address match.
      if (n.addr() == A) {

        // Take over in state from old address.
        sc_A.set_state_in(n.state_in());

        // But we may now be able to merge two regions:
        // If the node's old state matches the new, it becomes a noop. That happens, for example,
        //  when expanding a committed area: commit [x1, x2); ... commit [x2, x3)
        //  and the result should be a larger area, [x1..x3). In that case, the middle node (x2)
        //  is not needed anymore.
        // So we just remove the old node.
        if (sc_A.is_noop()) {
          rb_itor_remove(it); // invalidates it
        } else {
          // re-use existing node
          n.set_state_change(sc_A);
        }

      } else {
        // The address must be smaller.
        assert(n.addr() < A, "Sanity");

        // We add a new node, but only if there would be a state change. If there would not be a
        // state change, we just omit the node.
        // That happens, for example, when reserving within an already reserved region.
        sc_A.set_state_in(n.state_out()); // .. and the region's prior state is the incoming state
        if (sc_A.is_noop()) {
          // Nothing to do.
        } else {
          // Add new node.
          insert_new_node(A, sc_A);
        }
      }
    } else {
      // There was no entry with a lower address. That means that A is inserted
      // with incoming state=none.
      if (sc_A.is_noop()) {
        // Nothing to do.
      } else {
        // Add new node.
        insert_new_node(A, sc_A);
      }
    }

    // Now we handle B.
    // We first search all nodes that are between A and B. All of these nodes need to be deleted.
    // The last node before B determines B's outgoing state. If there is no node between A and B,
    // its A's incoming state.

    ResourceMark rm;
    GrowableArray<address> to_be_deleted(16);
    bool B_needs_insert = true;

    // Find all nodes between (A, B] and record their addresses. Also update B's outgoing
    // state.
    for (rc = rb_itor_search_gt(it, A);
         rc && VMANode(it).addr() <= B;
         rc = rb_itor_next(it))
    {
      VMANode n(it);
      sc_B.set_state_out(n.state_out());
      if (n.addr() < B) {
        // Delete all nodes preceding B.
        to_be_deleted.push(n.addr());
      } else {
        assert(n.addr() == B, "Sanity");
        // Re-purpose B node, unless it would result in a noop node, in which case
        // delete old node at B.
        if (sc_B.is_noop()) {
          to_be_deleted.push(B);
        } else {
          n.set_state_change(sc_B);
        }
        B_needs_insert = false;
      }
    }

    // Insert B node if needed
    if (B_needs_insert && !sc_B.is_noop()) {
      insert_new_node(B, sc_B);
    }

    // Finally, if needed, delete all nodes between (A, B)
    while (to_be_deleted.length() > 0) {
      const address delete_me = to_be_deleted.pop();
      rb_tree_remove(_tree, delete_me);
    }

    rb_itor_free(it);

    DEBUG_ONLY(verify();) // possibly expensive
  }

public:
  VMATree() {
    _tree = rb_tree_new(key_compare_func);
  }

  ~VMATree() {
    rb_tree_free(_tree, (dict_delete_func)nullptr);
  }

  void register_new_memory_mapping(address from, address to, MappingState state) {
    register_mapping(from, to, state);
  }

  void register_unmapping(address from, address to) {
    register_mapping(from, to, NONE_STATE);
  }

  void print_tree_raw(outputStream* st) const {
    rb_itor* it = rb_itor_new(_tree);
    bool rc;
    for (rc = rb_itor_first(it); rc; rc = rb_itor_next(it)) {
      VMANode n(it);
      st->print_cr(PTR_FORMAT ": in: %.4x out: %.4x",
                       p2i(n.addr()), n.state_in(), n.state_out());
    }
    rb_itor_free(it);
  }

  void print_all_mappings(outputStream* st) const {
    rb_itor* it = rb_itor_new(_tree);
    bool rc;
    address last_addr = nullptr;
    MappingState last_state = NONE_STATE;
    for (rc = rb_itor_first(it); rc; rc = rb_itor_next(it)) {
      VMANode n(it);
      if (last_addr != nullptr) {
        if (last_state != NONE_STATE) {
          const VMAState s = VMAMappingState(last_state).s();
          const MEMFLAGS f = VMAMappingState(last_state).f();
          st->print_cr(PTR_FORMAT "-" PTR_FORMAT ": committed=%d, flag=%d",
                       p2i(last_addr), p2i(n.addr()), (int)s, (int)f);
        }
      }
      last_addr = n.addr();
      last_state = n.state_out();
    }
    rb_itor_free(it);
  }

#ifdef ASSERT
  bool is_valid(address* bad, const char** what) const {
#define ASSERT_HERE(cond, text) \
    if (!(cond)) { \
      (*what) = text; \
      (*bad) = addr; \
      return false; \
    }
    address addr = nullptr;
    ASSERT_HERE(rb_tree_verify(_tree), "Sanity");
    // Iterate through all entries. Check:
    // - addresses rising, no duplicates
    // - incoming and outgoing states must match
    rb_node* last_node = nullptr;
    rb_itor* it = rb_itor_new(_tree);
    for (bool rc = rb_itor_first(it); rc; rc = rb_itor_next(it)) {
      VMANode n(it);
      addr = n.addr();
      if (last_node == nullptr) {
        ASSERT_HERE(addr != nullptr, "Null addr");
        ASSERT_HERE(n.state_in() == NONE_STATE, "very first address must start in NONE state");
      } else {
        VMANode n0(last_node);
        ASSERT_HERE(addr > n0.addr(), "addresses not rising?");
        ASSERT_HERE(n.state_in() == n0.state_out(), "Last node out does not match this node in");
      }
      ASSERT_HERE(!n.is_noop(), "Noop node found");
      last_node = it->_node;
    }
    if (last_node != nullptr) {
      ASSERT_HERE(VMANode(last_node).state_out() == NONE_STATE, "very last address must end in NONE state");
    }
    rb_itor_free(it);
    return true;
#undef ASSERT_HERE
  }

  void verify() const {
    const char* err = nullptr;
    address bad = nullptr;
    if (!is_valid(&bad, &err)) {
      tty->print_cr("Tree invalid (@" PTR_FORMAT ", %s)", p2i(bad), err);
      print_tree_raw(tty);
      assert(false, "Invalid tree (%s)", err);
    }
  }
#endif

  void report_summary(outputStream* st) const {
print_tree_raw(st);
    st->print_cr("VMA Summary");
    size_t reserved[(int)MEMFLAGS::mt_number_of_types] = { 0 };
    size_t committed[(int)MEMFLAGS::mt_number_of_types] = { 0 };
    rb_itor* it = rb_itor_new(_tree);
    bool rc;
    address last_addr = nullptr;
    for (rc = rb_itor_first(it); rc; rc = rb_itor_next(it)) {
      VMANode n(it);
      if (n.state_in() != NONE_STATE) {
        assert(last_addr != nullptr, "Sanity");
        const MappingState region_state = n.state_in();
        const VMAState s = VMAMappingState(region_state).s();
        assert(s == VMAState::reserved || s == VMAState::committed, "Sanity");
        const MEMFLAGS f = VMAMappingState(region_state).f();
        const int fi = NMTUtil::flag_to_index(f);
        assert(NMTUtil::flag_index_is_valid(fi), "Sanity");
        const size_t region_size = n.addr() - last_addr;
        reserved[fi] += region_size;
        if (s == VMAState::committed) {
          committed[fi] += region_size;
        }
        st->print_cr(PTR_FORMAT "-" PTR_FORMAT ": committed=%d, flag=%d",
                     p2i(last_addr), p2i(n.addr()), (int)s, (int)f);

      }
      last_addr = n.addr();
    }
    rb_itor_free(it);
    for (int i = 0; i < (int)mt_number_of_types; i++) {
      if (reserved[i] > 0) {
        st->print_cr("%s : reserved " SIZE_FORMAT ", committed " SIZE_FORMAT,
                     NMTUtil::flag_to_enum_name(NMTUtil::index_to_flag(i)), reserved[i], committed[i]);
      }
    }
    st->print_cr("/VMA Summary");
  }
};

static VMATree _g_vma_tree;

void VMADictionary::register_create_mapping(address from, address to, MEMFLAGS f, VMAState s) {
  _g_vma_tree.register_new_memory_mapping(from, to, VMAMappingState(f, s).state());
}

void VMADictionary::register_release_mapping(address from, address to) {
  _g_vma_tree.register_unmapping(from, to);
}

void VMADictionary::print_all_mappings(outputStream* st) {
  _g_vma_tree.print_all_mappings(st);
}

void VMADictionary::print_tree_raw(outputStream* st) {
  _g_vma_tree.print_tree_raw(st);
}

void VMADictionary::report_summary(outputStream* st) {
  _g_vma_tree.report_summary(st);
}

#ifdef ASSERT
void VMADictionary::verify() {
  _g_vma_tree.verify();
}
#endif

