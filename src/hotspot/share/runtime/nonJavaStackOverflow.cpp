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

#include "logging/log.hpp"
#include "runtime/nonJavaStackOverflow.hpp"
#include "runtime/os.inline.hpp"
#include "utilities/align.hpp"

// Minimum zone size. Just barely enough to run error handler and to give us a stack trace in an hs-err file.
constexpr size_t min_zone_size = 4 * K;

// Max ratio between protection zone size and thread stack size. If the ratio gets
// larger, we won't spend stack space for a protection zone.
constexpr double max_zone_size_ratio = 0.1;

// Initialization after thread is started.
void NonJavaStackOverflow::initialize(address base, address end) {

  const size_t ps = os::vm_page_size();
  const size_t zone_size = align_up(min_zone_size, ps);
  const size_t stack_size = pointer_delta_as_int(base, end);

  if (((double)zone_size / (double) stack_size) > max_zone_size_ratio) {
    _can_be_enabled = false;
    return;
  }

  _stack_base = base;
  _stack_end = end;
  _zone_end = align_up(_stack_end, ps);
  _zone_base = _zone_end + zone_size;
  assert(_zone_base < _stack_base && _zone_end >= _stack_end, "Sanity");
  _can_be_enabled = true;
}

void NonJavaStackOverflow::create_stack_guard_page() {
  if (!os::uses_stack_guard_pages() ||
      !_can_be_enabled ||
      _enabled // nothing to do
      ) {
      log_info(os, thread)("NonJavaStack guard page creation for thread %zu disabled", os::current_thread_id());
    return;
  }

  const address low_addr = _zone_end;
  const size_t len = pointer_delta_as_int(_zone_base, _zone_end);

  assert(is_aligned(low_addr, os::vm_page_size()), "Stack base should be the start of a page");
  assert(is_aligned(len, os::vm_page_size()), "Stack size should be a multiple of page size");

  bool must_commit = os::must_commit_stack_guard_pages();

  if (must_commit && !os::create_stack_guard_pages((char *) low_addr, len)) {
    log_warning(os, thread)("Attempt to allocate stack guard pages failed.");
    return;
  }

  if (os::guard_memory((char *) low_addr, len)) {
    _enabled = true;
  } else {
    log_warning(os, thread)("Attempt to protect stack guard pages failed ("
      PTR_FORMAT "-" PTR_FORMAT ").", p2i(low_addr), p2i(low_addr + len));
  }

  log_debug(os, thread)("NonJavaThread %zu stack guard pages activated: "
    PTR_FORMAT "-" PTR_FORMAT ".",
    os::current_thread_id(), p2i(low_addr), p2i(low_addr + len));
}


void NonJavaStackOverflow::remove_stack_guard_page() {
  if (!_enabled) return;

  const address low_addr = _zone_end;
  const size_t len = pointer_delta_as_int(_zone_base, _zone_end);

  if (os::must_commit_stack_guard_pages()) {
    if (os::remove_stack_guard_pages((char *) low_addr, len)) {
      _enabled = false;
    } else {
      log_warning(os, thread)("Attempt to deallocate stack guard pages failed ("
        PTR_FORMAT "-" PTR_FORMAT ").", p2i(low_addr), p2i(low_addr + len));
      return;
    }
  } else {
    if (os::unguard_memory((char *) low_addr, len)) {
      _enabled = false;
    } else {
      log_warning(os, thread)("Attempt to unprotect stack guard pages failed ("
        PTR_FORMAT "-" PTR_FORMAT ").", p2i(low_addr), p2i(low_addr + len));
      return;
    }
  }

  log_debug(os, thread)("Thread %zu stack guard pages removed: "
    PTR_FORMAT "-" PTR_FORMAT ".",
    os::current_thread_id(), p2i(low_addr), p2i(low_addr + len));
}
