/*
 * Copyright (c) 2021 SAP SE. All rights reserved.
 * Copyright (c) 2021, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_SERVICES_NMT_PREINIT_MALLOC_HASH_TABLE_HPP
#define SHARE_SERVICES_NMT_PREINIT_MALLOC_HASH_TABLE_HPP



#include "memory/allocation.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/macros.hpp"

#if INCLUDE_NMT

class outputStream;


// VM initialization wrt NMT:
//
//---------------------------------------------------------------
//-> launcher dlopen's libjvm                           ^
//   -> dynamic C++ initialization                      |
//           of libjvm                                  |
//                                                      |
//-> launcher starts new thread (maybe)          NMT pre-init phase
//                                                      |
//-> launcher invokes CreateJavaVM                      |
//   -> VM initialization before arg parsing            |
//   -> VM argument parsing                             v
//   -> NMT initialization  -------------------------------------
//                                                      ^
//   ...                                                |
//   -> VM life...                               NMT post-init phase
//   ...                                                |
//                                                      v
//----------------------------------------------------------------


// NMT is initialized after argument parsing, long after the first C-heap allocations happen
//  in the VM. Therefore it misses the first n allocations, and when those allocations are freed,
//  it needs to treat those special.
// To separate pre-init allocations from post-init allocations, pre-init allocations are not
//  taken from C-heap at all but silently redirected from os::malloc() to an NMT internal static
//  preallocated buffer.
//
// This class implements this NMT pre-init-buffer. It consists of two parts:
// - a very small one (128k), allocated upfront right at VM start. It is in 99% of all cases
//   sufficient to bring the VM up to post-init phase.
// - Only if there is a lot of memory allocated during preinit-phase this buffer will not be
//   enough. That can happen e.g. with outlandishly long command lines. In that case, a second,
//   much larger overflow buffer will be dynamically allocated and used.

class NMTPreInitMallocHashTable {

  class PointerSlab {
    static const int size = 4;
    address _v[size];
    PointerSlab* _next;

    int find(address p) const {
      for (int i = 0; i < size; i++) {
        if (_v[i] == p) {
          return i;
        }
        return -1;
      }
    }

  public:

    PointerSlab() : _next(NULL) {
      for (int i = 0; i < size; i++) {
        _v[i] = 0;
      }
    }

    bool try_add(address p) {
      assert(p != NULL, "sanity");
      int i = find(NULL);
      if (i != -1) {
        _v[i] = p;
        return true;
      }
      return false;
    }

    bool try_remove(address p) const {
      assert(p != NULL, "sanity");
      int i = find(p);
      if (i != -1) {
        _v[i] = 0;
        return true;
      }
      return false;
    }

    PointerSlab* next() const { return _next; }
    void set_next(PointerSlab* next) { _next = next; }

    static PointerSlab* create_slab();

  };

  // An address which either can be a pointer to a malloced area, or a pointer
  // to a PointerSlab. Since both are allocated from C-Heap the lower 2-3 bits
  // should be unused, and we use the lowest bit to distinguish the cases.
  class AdornedAddress {
    intptr_t _v;

    static intptr_t set_slab_bit(intptr_t v) {
      return v | 1;
    }

    static intptr_t clear_slab_bit(intptr_t v) {
      return v & ~((intptr_t)1);
    }

    static bool is_slab_bit_set(intptr_t v) {
      return v & 1;
    }

  public:

    AdornedAddress() : _v(0) {}

    void set_from_malloced_pointer(address p) {
      intptr_t v = p2i(p);
      assert(!is_slab_bit_set(v), "sanity");
      _v = v;
    }

    void set_from_slab(PointerSlab* slab) {
      intptr_t v = p2i(slab);
      assert(!is_slab_bit_set(v), "sanity");
      _v = set_slab_bit(v);
    }

    bool is_slab() const {
      return is_slab_bit_set(_v);
    }

    void clear() {
      _v = 0;
    }

    bool is_null() const { return _v == 0; }

    PointerSlab* as_slab() const {
      assert(is_slab_bit_set(_v), "sanity");
      return (PointerSlab*)(clear_slab_bit(_v));
    }

    address as_malloced_pointer() const {
      assert(!is_slab_bit_set(_v), "sanity");
      return (address)_v;
    }

  };

  static const int table_size = 1024;

  AdornedAddress _table[table_size];

  static unsigned calculate_hash(address p) {
    return (unsigned)(p2i(p)); // improve this
  }

  static int get_table_index(address p) {
    const unsigned hash = calculate_hash(p);
    return hash % table_size;
  }

public:

  NMTPreInitMallocHashTable() {}

  void register_malloced_pointer(address p) {
    assert(!find_and_remove_malloced_pointer(p), "double pointer?");
    const int index = get_table_index(p);
    AdornedAddress& a = _table[index];
    if (a.is_null()) {
      a.set_from_malloced_pointer(p);
      return;
    }
    if (a.is_slab() == false) {
      // if this is a singular pointer, create the first slab,
      // replace pointer with slab address, evacuate pointer to first slab.
      PointerSlab* slab = PointerSlab::create_slab();
      address old_address = a.as_malloced_pointer();
      a.set_from_slab(slab);
      // slab has at least two free slots
      bool success = slab->try_add(old_address) && slab->try_add(p);
      assert(success, "sanity");
      assert(a.is_slab(), "sanity");
      return;
    }
    // Its a slab already, maybe more, if our hash is not good. Find a slab to take the pointer.
    PointerSlab* slab = a.as_slab();
    DEBUG_ONLY(int fuse = 2000;) // should never happen.
    for(;;) {
      if (slab->try_add(p)) {
        return;
      }
      if (slab->next() == NULL) {
        slab->set_next(PointerSlab::create_slab());
      }
      assert(fuse-- > 0, "sanity");
    }
  }

  // Given a pointer, remove it from the table if it exists, otherwise do nothing. Returns true if removed.
  bool find_and_remove_malloced_pointer(address p) {
    assert(p != NULL, "sanity");
    const int index = get_table_index(p);
    AdornedAddress& a = _table[index];
    if (a.is_null()) {
      return false;
    }
    if (a.is_slab() == false) {
      // this is a singular pointer
      if (a.as_malloced_pointer() == p) {
        a.clear();
        return true;
      }
      return false;
    }
    // Its a slab. Find pointer in slabs.
    PointerSlab* slab = a.as_slab();
    DEBUG_ONLY(int fuse = 2000;) // should never happen.
    for (PointerSlab* slab = a.as_slab(); slab != NULL; slab = slab->next()) {
      if (slab->try_remove(p)) {
        return true;
      }
      assert(fuse-- > 0, "sanity");
    }
  }

  // print a string describing the current state
  void print_state(outputStream* st) const;

};

#endif // INCLUDE_NMT

#endif // SHARE_SERVICES_NMT_PREINIT_MALLOC_HASH_TABLE_HPP
