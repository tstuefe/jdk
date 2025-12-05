/*
 * Copyright (c) 2025, Oracle and/or its affiliates. All rights reserved.
 * Copyright 2025 IBM Corporation. All rights reserved.
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

#ifndef SHARE_RUNTIME_NONJAVASTACKOVERFLOW_HPP
#define SHARE_RUNTIME_NONJAVASTACKOVERFLOW_HPP

#include "utilities/debug.hpp"

// A much abridged variant of the StackOverflow class for java threads.

class NonJavaStackOverflow {
 public:

  NonJavaStackOverflow() :
    _can_be_enabled(false),
    _enabled(false),
    _zone_base(nullptr), _zone_end(nullptr),
    _stack_base(nullptr), _stack_end(nullptr) {}

  // Initialization after thread is started.
  void initialize(address base, address end);

 private:

  bool _can_be_enabled;
  bool _enabled;

  // start of protection zone (highest address)
  address    _zone_base;
  address    _zone_end;

  // Support for stack overflow handling, copied down from thread.
  address    _stack_base;
  address    _stack_end;

  address stack_end()  const           { return _stack_end; }
  address stack_base() const           { assert(_stack_base != nullptr, "Sanity check"); return _stack_base; }

  static address calculate_zone_start(address stack_base, address stack_end);

 public:

  // Returns true if address points into the red zone.
  bool in_zone(address a) const {
    return a < _zone_base && a >= stack_end();
  }

  void create_stack_guard_page();
  void remove_stack_guard_page();

  bool enabled() const { return _enabled; }
};

#endif // SHARE_RUNTIME_STACKOVERFLOW_HPP
