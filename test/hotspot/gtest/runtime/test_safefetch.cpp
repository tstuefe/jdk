/*
 * Copyright (c) 2020, 2021, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2020 SAP SE. All rights reserved.
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
 */

#include "precompiled.hpp"
#include "runtime/interfaceSupport.inline.hpp"
#include "runtime/safefetch.inline.hpp"
#include "runtime/vmOperations.hpp"
#include "runtime/vmThread.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/vmError.hpp"
#include "unittest.hpp"
#include "testutils.hpp"

// Note: beyond these tests, there exist additional tests testing that safefetch in error handling
// (in the context of signal handling) works, see runtime/ErrorHandling

static const intptr_t patternN = LP64_ONLY(0xABCDABCDABCDABCDULL) NOT_LP64(0xABCDABCD);
static const int pattern32 = 0xABCDABCD;

TEST_VM(os, safefetch_can_use) {
  // Once VM initialization is through,
  // safefetch should work on every platform.
  ASSERT_TRUE(CanUseSafeFetch32());
}

// A little piece of memory in hopefully high address area, so that its address has bits
// set in the upper 32bit word too.
template <class T>
class TestMemory {
  void* const _p;
  const size_t _size;

public:

  TestMemory(bool readable) : _p(GtestUtils::reserve_memory_upstairs(os::vm_allocation_granularity())),
                              _size(os::vm_allocation_granularity())
  {
    assert(_p != NULL, "failed to reserve " SIZE_FORMAT " bytes", _size);
    os::commit_memory_or_exit((char*)_p, _size, false, "testmemory");
    if (!readable) {
      bool rc = os::protect_memory((char*)_p, _size, os::MEM_PROT_NONE);
      assert(rc, "protect memory failed");
    }
  }

  ~TestMemory() {
    if (_p != NULL) {
      os::release_memory((char*)_p, _size);
    }
  }

  T* p() const { return (T*)_p; }

}; // end TestMemory

static void test_safefetchN_positive(intptr_t* location) {
  *location = patternN;
  intptr_t a = SafeFetchN(location, 1);
  ASSERT_EQ(patternN, a);
}

static void test_safefetch32_positive(int* location) {
  *location = pattern32;
  uint64_t a = SafeFetch32(location, 1);
  ASSERT_EQ((uint64_t)pattern32, a);
}

static void test_safefetchN_negative(intptr_t* location) {
  intptr_t a = SafeFetchN(location, patternN);
  ASSERT_EQ(patternN, a);
  a = SafeFetchN(location, ~patternN);
  ASSERT_EQ(~patternN, a);
}

static void test_safefetch32_negative(int* location) {
  int a = SafeFetch32(location, pattern32);
  ASSERT_EQ(pattern32, a);
  a = SafeFetch32(location, ~pattern32);
  ASSERT_EQ(~pattern32, a);
}

TEST_VM(os, safefetchN_positive) {
  TestMemory<intptr_t> tm(true);
  test_safefetchN_positive(tm.p());
}

TEST_VM(os, safefetch32_positive) {
  TestMemory<int> tm(true);
  test_safefetch32_positive(tm.p());
}

TEST_VM(os, safefetchN_negative) {
  TestMemory<intptr_t> tm(false);
  test_safefetchN_negative(tm.p());
  // also test NULL
#ifndef _AIX
  test_safefetchN_negative(NULL);
#endif
}

TEST_VM(os, safefetch32_negative) {
  TestMemory<int> tm(false);
  test_safefetch32_negative(tm.p());
  // also test NULL
#ifndef _AIX
  test_safefetch32_negative(NULL);
#endif
}

// Try with Thread::current being NULL. SafeFetch should work then too.
// See JDK-8282475

class ThreadCurrentNullMark : public StackObj {
  Thread* _saved;
public:
  ThreadCurrentNullMark() {
    _saved = Thread::current();
    Thread::clear_thread_current();
  }
  ~ThreadCurrentNullMark() {
    _saved->initialize_thread_current();
  }
};

TEST_VM(os, safefetchN_positive_current_null) {
  TestMemory<intptr_t> tm(true);
  {
    ThreadCurrentNullMark tcnmark;
    test_safefetchN_positive(tm.p());
  }
}

TEST_VM(os, safefetch32_positive_current_null) {
  TestMemory<int> tm(true);
  {
    ThreadCurrentNullMark tcnmark;
    test_safefetch32_positive(tm.p());
  }
}

TEST_VM(os, safefetchN_negative_current_null) {
  TestMemory<intptr_t> tm(false);
  {
    ThreadCurrentNullMark tcnmark;
    test_safefetchN_negative(tm.p());
    // also test NULL
#ifndef _AIX
    test_safefetchN_negative(NULL);
#endif
  }
}

TEST_VM(os, safefetch32_negative_current_null) {
  TestMemory<int> tm(false);
  {
    ThreadCurrentNullMark tcnmark;
    test_safefetch32_negative(tm.p());
    // also test NULL
#ifndef _AIX
    test_safefetch32_negative(NULL);
#endif
  }
}

class VM_TestSafeFetchAtSafePoint : public VM_GTestExecuteAtSafepoint {
public:
  void doit() {
    // Regression test for JDK-8257828
    // Should not crash.
    test_safefetchN_negative((intptr_t*)VMError::segfault_address);
  }
};

TEST_VM(os, safefetch_negative_at_safepoint) {
  VM_TestSafeFetchAtSafePoint op;
  ThreadInVMfromNative invm(JavaThread::current());
  VMThread::execute(&op);
}
