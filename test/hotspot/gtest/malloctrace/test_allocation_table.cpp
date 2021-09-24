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
 */

#include "precompiled.hpp"

#ifdef LINUX

#include "jvm_io.h"
#include "malloctrace/allocationTable.hpp"
#include "memory/allocation.hpp"
#include "runtime/os.hpp"
#include "utilities/debug.hpp"
#include "utilities/ostream.hpp"
#include "unittest.hpp"

#ifdef __GLIBC__

using sap::AllocationTable;

static void init_random_randomly() {
  os::init_random((int)os::elapsed_counter());
}

//#define LOG

// Since AllocationTable is too large to be put onto the stack of a test function,
// we need to create it dynamically. I don't want to make it a CHeapObj only
// for the sake of these tests though, so I have to use placement new.
static AllocationTable* create_table() {
  void* p = NEW_C_HEAP_ARRAY(AllocationTable, 1, mtTest);
  return new (p) AllocationTable;
}

static void destroy_table(AllocationTable* s) {
  FREE_C_HEAP_ARRAY(SiteTable, s);
}

static void test_print_table(const AllocationTable* table, int expected_entries) {
  stringStream ss;

  table->print_stats(&ss);
  if (expected_entries != -1) {
    char match[32];
    jio_snprintf(match, sizeof(match),
                 "num_entries: %u", expected_entries);
    ASSERT_NE(::strstr(ss.base(), match), (char*)NULL);
  }
  ss.reset();
}

static const intptr_t g_arbitrary_number = LP64_ONLY(0xFFFFFFFFF0000000ULL) NOT_LP64(0xF000000);

TEST_VM(MallocTrace, site_allocation_table_sequence) {

  init_random_randomly();

  AllocationTable* table = create_table();

  test_print_table(table, 0); // Test printing empty table.

  for (int run = 0; run < 3; run ++) {
    for (int i = 0; i < 10000; i ++) {
      intptr_t unique_arbitrary_number = g_arbitrary_number + i;
      void* p = (void*)unique_arbitrary_number;
      table->add_allocation(p, unique_arbitrary_number, (sap::Site*)unique_arbitrary_number);
    }

    EXPECT_EQ(table->size(), (unsigned)10000);
    test_print_table(table, 10000);
    DEBUG_ONLY(table->verify();)

    for (int i = 0; i < 10000; i ++) {
      intptr_t unique_arbitrary_number = g_arbitrary_number + i;
      void* p = (void*)unique_arbitrary_number;
      size_t size;
      sap::Site* site = table->remove_allocation(p, &size);
      EXPECT_EQ(site, (sap::Site*)unique_arbitrary_number);
      EXPECT_EQ(size, (size_t)unique_arbitrary_number);
      // try again, should fail
      site = table->remove_allocation(p, &size);
      EXPECT_EQ(site, (sap::Site*)NULL);
    }

    EXPECT_EQ(table->size(), (unsigned)0);
    test_print_table(table, 0);
    DEBUG_ONLY(table->verify();)
  }

#ifdef LOG
  //table->print_table(tty, true);
  table->print_stats(tty);
  tty->cr();
#endif

  destroy_table(table);
}

TEST_VM(MallocTrace, site_allocation_table_reset) {

  init_random_randomly();

  AllocationTable* table = create_table();

  for (int run = 0; run < 3; run ++) {
    for (int i = 0; i < 100; i ++) {
      intptr_t unique_arbitrary_number = (intptr_t)os::random();
      if (unique_arbitrary_number == 0) {
        unique_arbitrary_number = 1;
      }
      table->add_allocation((void*)unique_arbitrary_number, unique_arbitrary_number, (sap::Site*)unique_arbitrary_number);
    }

    EXPECT_EQ(table->size(), (unsigned)100);
    test_print_table(table, 100);
    DEBUG_ONLY(table->verify();)

    table->reset();

    EXPECT_EQ(table->size(), (unsigned)0);
    test_print_table(table, 0);
    DEBUG_ONLY(table->verify();)

  }

#ifdef LOG
  //table->print_table(tty, true);
  table->print_stats(tty);
  tty->cr();
#endif

  destroy_table(table);
}

#endif // __GLIBC__

#endif // LINUX
