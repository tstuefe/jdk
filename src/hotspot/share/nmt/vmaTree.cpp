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

class MappingInfo {
  union {
    void* raw;
    struct {
      VMAState s;
      MEMFLAGS f;
    } data;
  } _v;
  STATIC_ASSERT(sizeof(_v) == sizeof(void*));
public:
  MappingInfo(void* v) {
    _v.raw = v;
  }
  MappingInfo(MEMFLAGS f, VMAState s) {
    _v.data.s = s;
    _v.data.f = f;
  }
  VMAState state() const { return _v.data.s; }
  MEMFLAGS f() const { return _v.data.f; }
  void* raw() const { return _v.raw; }
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
  }

  ~VMATree() {
    rb_tree_free(_tree, (dict_delete_func)nullptr);
  }

};
