/*
 * Copyright (c) 2022, Oracle and/or its affiliates. All rights reserved.
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
#include "memory/allocation.hpp"
#include "runtime/lockStack.inline.hpp"
#include "runtime/safepoint.hpp"
#include "runtime/thread.hpp"
#include "utilities/copy.hpp"
#include "utilities/ostream.hpp"


#define LSFORMAT "cap %d, used %d, " \
  PTR_FORMAT ", " PTR_FORMAT ", " PTR_FORMAT ", " PTR_FORMAT ", " \
  PTR_FORMAT ", " PTR_FORMAT ", " PTR_FORMAT ", " PTR_FORMAT

#define LSFORMAT_ARGS CAPACITY, to_index(_offset), \
  p2i(_base[0]), p2i(_base[1]), p2i(_base[2]), p2i(_base[3]), \
  p2i(_base[4]), p2i(_base[5]), p2i(_base[6]), p2i(_base[7])


#define LOGME(me, ...) if (UseNewCode ) { \
  fprintf(stderr, "[tid=%u] ",(unsigned)os::current_thread_id()); \
  fprintf(stderr, "LockStack: " PTR_FORMAT " " LSFORMAT, p2i(this), LSFORMAT_ARGS); \
  fprintf(stderr, __VA_ARGS__); \
  fprintf(stderr, "\n"); \
  fflush(stderr); \
}

LockStack::LockStack() :
 _offset(start_offset()) {
  DEBUG_ONLY(zap_trailing_slots(Poison::poison_init);)
}

int LockStack::start_offset() {
  return in_bytes(JavaThread::lock_stack_base_offset());
}

int LockStack::end_offset() {
  return start_offset() + (CAPACITY * oopSize);
}

#ifndef PRODUCT
void LockStack::validate(const char* msg) const {
  assert(UseFastLocking && !UseHeavyMonitors, "never use lock-stack when fast-locking is disabled");
  assert((_offset <=  end_offset()), "lockstack overflow." LSFORMAT, LSFORMAT_ARGS);
  assert((_offset >= start_offset()), "lockstack underflow." LSFORMAT, LSFORMAT_ARGS);
  const int used_end = to_index(_offset);
  const int end = to_index(end_offset());
  for (int i = 0; i < used_end; i++) {
//    assert(oopDesc::is_oop(_base[i], false), "index %i: not an oop. " LSFORMAT, i, LSFORMAT_ARGS);
    assert((p2i(_base[i]) & ~((intptr_t)0xFF)) != 0, "index %i: dead oop. " LSFORMAT, i, LSFORMAT_ARGS);
    for (int j = i + 1; j < used_end; j++) {
      assert(_base[i] != _base[j], "%d %d : entries must be unique. " LSFORMAT, i, j, LSFORMAT_ARGS);
    }
  }
  for (int i = used_end; i < end; i++) {
    assert((p2i(_base[i]) & ~((intptr_t)0xFF)) == 0, "index %i: expected dead oop. " LSFORMAT, i, LSFORMAT_ARGS);
  }
}
#endif

 void LockStack::oops_do(OopClosure* cl) {
LOGME(this, "--->oops-do");
  validate("pre-oops-do");
  int end = to_index(_offset);
  for (int i = 0; i < end; i++) {
    cl->do_oop(&_base[i]);
  }
  validate("post-oops-do");
LOGME(this, "<---oops-do");
}

void LockStack::print_on(outputStream* st) const {
  st->print("LockStack " LSFORMAT, LSFORMAT_ARGS);
}
