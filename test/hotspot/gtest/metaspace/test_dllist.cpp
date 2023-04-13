/*
 * Copyright (c) 2023 SAP SE. All rights reserved.
 * Copyright (c) 2023, Oracle and/or its affiliates. All rights reserved.
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
#include "memory/metaspace/dllist.inline.hpp"

#define LOG_PLEASE
#include "metaspaceGtestCommon.hpp"

using metaspace::DlList;

struct X : public DlList<X>::Node {};

static void verify_list(const DlList<X>& l, int num_expected, ...) {
  va_list va;
  va_start(va, num_expected);
  const X* p = l.front();
  for (int i = 0; i < num_expected; i++) {
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(p, va_arg(va, const X*));
    ASSERT_TRUE(l.contains(p));
    p = p->next();
  }
  ASSERT_NULL(p);
  va_end(va);
  EXPECT_EQ(l.count(), num_expected);
  l.verify();
}

#define TESTDATA \
  X x[6]; \
  X* const a = x; \
  X* const b = x + 1; \
  X* const c = x + 2; \
  X* const d = x + 3; \
  X* const e = x + 4; \
  X* const f = x + 5;

#define FILL_ABC(list) \
		list.push_back(a); \
		list.push_back(b); \
		list.push_back(c);

#define FILL_DEF(list) \
    list.push_back(d); \
    list.push_back(e); \
    list.push_back(f);

TEST_VM(metaspace, DlListPushPopEmpty) {
  DlList<X> l;
  verify_list(l, 0);
  ASSERT_NULL(l.pop_front());
  ASSERT_NULL(l.pop_back());
}

TEST_VM(metaspace, DlListPushPop1Front) {
  TESTDATA
  DlList<X> l;
  l.push_front(a);
  verify_list(l, 1, a);
  EXPECT_EQ(l.pop_front(), a);
  verify_list(l, 0);
}

TEST_VM(metaspace, DlListReset) {
  TESTDATA
  DlList<X> l;
  FILL_ABC(l)
  FILL_DEF(l)
  verify_list(l, 6, a, b, c, d, e, f);
  l.reset();
  verify_list(l, 0);
  l.reset();
  verify_list(l, 0);
}

TEST_VM(metaspace, DlListPushPop1Back) {
  TESTDATA
  DlList<X> l;
  l.push_back(a);
  verify_list(l, 1, a);
  EXPECT_EQ(l.pop_back(), a);
  verify_list(l, 0);
}

TEST_VM(metaspace, DlListPushPop) {
  TESTDATA
  DlList<X> l;
  FILL_ABC(l);
  verify_list(l, 3, a, b, c);

  l.push_front(d);
  l.push_front(e);
  l.push_front(f);
  verify_list(l, 6, f, e, d, a, b, c);

  EXPECT_EQ(l.pop_front(), f);
  EXPECT_EQ(l.pop_front(), e);
  EXPECT_EQ(l.pop_front(), d);
  verify_list(l, 3, a, b, c);

  l.push_back(d);
  l.push_back(e);
  l.push_back(f);
  verify_list(l, 6, a, b, c, d, e, f);

  EXPECT_EQ(l.pop_back(), f);
  EXPECT_EQ(l.pop_back(), e);
  EXPECT_EQ(l.pop_back(), d);
  verify_list(l, 3, a, b, c);
}

TEST_VM(metaspace, DlListRemoveFront) {
  TESTDATA
  DlList<X> l;
  FILL_ABC(l);
  verify_list(l, 3, a, b, c);
  l.remove(a);
  verify_list(l, 2, b, c);
  l.remove(b);
  verify_list(l, 1, c);
  l.remove(c);
  verify_list(l, 0);
}

TEST_VM(metaspace, DlListRemoveBack) {
  TESTDATA
  DlList<X> l;
  FILL_ABC(l);
  verify_list(l, 3, a, b, c);
  l.remove(c);
  verify_list(l, 2, a, b);
  l.remove(b);
  verify_list(l, 1, a);
  l.remove(a);
  verify_list(l, 0);
}

TEST_VM(metaspace, DlListRemoveMiddle) {
  TESTDATA
  DlList<X> l;
  FILL_ABC(l);
  verify_list(l, 3, a, b, c);
  l.remove(b);
  verify_list(l, 2, a, c);
}

TEST_VM(metaspace, DlListAddListFront) {
  TESTDATA
  DlList<X> l1, l2;
  FILL_ABC(l1);
  FILL_DEF(l2);
  verify_list(l1, 3, a, b, c);
  verify_list(l2, 3, d, e, f);

  l1.add_list_at_front(l2); // add non-empty to non-empty
  verify_list(l1, 6, d, e, f, a, b, c);
  verify_list(l2, 0);

  l1.add_list_at_front(l2); // add empty to non-empty - nothing should change
  verify_list(l1, 6, d, e, f, a, b, c);
  verify_list(l2, 0);

  l2.add_list_at_front(l1); // add non-empty to empty - lists should swap
  verify_list(l1, 0);
  verify_list(l2, 6, d, e, f, a, b, c);
}

TEST_VM(metaspace, DlListAddListBack) {
  TESTDATA
  DlList<X> l1, l2;
  FILL_ABC(l1);
  FILL_DEF(l2);
  verify_list(l1, 3, a, b, c);
  verify_list(l2, 3, d, e, f);

  l1.add_list_at_back(l2); // add non-empty to non-empty
  verify_list(l1, 6, a, b, c, d, e, f);
  verify_list(l2, 0);

  l1.add_list_at_front(l2); // add empty to non-empty - nothing should change
  verify_list(l1, 6, a, b, c, d, e, f);
  verify_list(l2, 0);

  l2.add_list_at_front(l1); // add non-empty to empty - lists should swap
  verify_list(l1, 0);
  verify_list(l2, 6, a, b, c, d, e, f);
}

TEST_VM(metaspace, DlListAddSingleItemListFront) {
  TESTDATA
  DlList<X> l1, l2;
  FILL_ABC(l1);
  l2.push_front(d);
  verify_list(l1, 3, a, b, c);
  verify_list(l2, 1, d);

  l1.add_list_at_front(l2); // add non-empty to non-empty
  verify_list(l1, 4, d, a, b, c);
  verify_list(l2, 0);
}

TEST_VM(metaspace, DlListAddSingleItemListBack) {
  TESTDATA
  DlList<X> l1, l2;
  FILL_ABC(l1);
  l2.push_front(d);
  verify_list(l1, 3, a, b, c);
  verify_list(l2, 1, d);

  l1.add_list_at_back(l2); // add non-empty to non-empty
  verify_list(l1, 4, a, b, c, d);
  verify_list(l2, 0);
}

TEST_VM(metaspace, DlListForEach) {
  // Verify that for_each iterates the whole list
  TESTDATA
  DlList<X> l;
  FILL_ABC(l);
  verify_list(l, 3, a, b, c);

  int num = 0;
  const X* first = nullptr, *last = nullptr;
  auto lam = [&num, &first, &last] (const X* p) {
    num ++;
    if (first == nullptr) {
      first = p;
    }
    last = p;
  };

  l.for_each(lam);
  EXPECT_EQ(num, 3);
  EXPECT_EQ(first, l.front());
  EXPECT_EQ(first, a);
  EXPECT_EQ(last, l.back());
  EXPECT_EQ(last, c);
}

TEST_VM(metaspace, DlListForEachUntilNegative) {
  // Verify that for_each_until iterates the whole list if not aborted
  TESTDATA
  DlList<X> l;
  FILL_ABC(l);
  verify_list(l, 3, a, b, c);

  int num = 0;
  const X* first = nullptr, *last = nullptr;
  auto lam = [&num, &first, &last] (const X* p) {
    num ++;
    if (first == nullptr) {
      first = p;
    }
    last = p;
    return false;
  };

  // iterate non-empty list
  l.for_each_until(lam);
  EXPECT_EQ(num, 3);
  EXPECT_EQ(first, l.front());
  EXPECT_EQ(first, a);
  EXPECT_EQ(last, l.back());
  EXPECT_EQ(last, c);
}

TEST_VM(metaspace, DlListForEachUntilPositive) {
  // Verify that for_each_until interrupts looping
  TESTDATA
  DlList<X> l;
  FILL_ABC(l);
  verify_list(l, 3, a, b, c);

  // We interrupt at "b"
  auto lam = [] (const X* p) { return (p == b); };
  const X* found = l.for_each_until(lam);
  EXPECT_EQ(found, b);
}

TEST_VM(metaspace, DlListContains) {
  TESTDATA
  DlList<X> l;
  FILL_ABC(l);
  verify_list(l, 3, a, b, c);

  EXPECT_TRUE(l.contains(a));
  EXPECT_TRUE(l.contains(b));
  EXPECT_TRUE(l.contains(c));

  EXPECT_FALSE(l.contains(d));
  EXPECT_FALSE(l.contains(e));
  EXPECT_FALSE(l.contains(f));
}

