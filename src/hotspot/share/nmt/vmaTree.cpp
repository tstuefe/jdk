/*
 * Copyright (c) 2013, 2023, Oracle and/or its affiliates. All rights reserved.
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

class Data {
  union {
    void* raw;
    struct {
      VMAState s;
      MEMFLAGS f;
    } data;
  } _v;
  STATIC_ASSERT(sizeof(_v) == sizeof(void*));
public:
  Data(void* v) {
    _v.raw = v;
  }
  Data(MEMFLAGS f, VMAState s) {
    _v.data.s = s;
    _v.data.f = f;
  }
  VMAState state() const { return _v.data.s; }
  MEMFLAGS f() const { return _v.data.f; }
  void* raw() const { return _v.raw; }

  static constexpr Data hole_data = Data(mtNone, VMAState::none);
};



static int key_compare_func(const void* a, const void* b) {
  if (a == b) {
    return 0;
  } else {
    return a > b ? -1 : 1;
  }
}

class VMATree {
  rb_tree* _tree;

public:
  VMATree() {
    _tree = rb_tree_new(key_compare_func);
    // we seed the tree with a hole-node starting at the lowest possible address.
    dict_insert_result res = rb_tree_insert(_tree, 4 * K);

  }

  ~VMATree() {
    rb_tree_free(_tree, (dict_delete_func)nullptr);
  }

  void register_mapping(address from, address to, MEMFLAGS f, VMAState s) {

    // prepare Data for new range starting at from address
    const Data data_from = Data(f, s);


    // Find nearest lowest pointer
    tree_node* next_lowest = tree_search_le_node(_tree, from);
    if (next_lowest) { // found one (very likely)
      const address address_next_lowest = (address)next_lowest->key;
      assert(address_next_lowest <= from, "Sanity");

      // If it exist, we distinguish three cases:
      // 1) it is at our address. Then we replace the properties with the new ones.
      // 2) it is lower than us. Then:
      // 2.1) if it is a hole-marker, we start a new range at from address
      // 2.2) if it is not a hole-marker, it marks the beginning of a range. Then:
      // 2.2.1) if the properties match, we don't do anything (e.g. reserving with the from address inside an already reserved range)
      // 2.2.2) if the properties don't match, we start a new range at from
      bool add_new_from_node = false;
      if (address_next_lowest == from) {
        // (1)
        next_lowest->datum =

      } else {
        add_new_from_node = true;
      }

    }

  }

};
