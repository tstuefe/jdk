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

#ifndef OS_LINUX_MALLOCTRACE_ITEMHEAP_HPP
#define OS_LINUX_MALLOCTRACE_ITEMHEAP_HPP

#include "malloctrace/assertHandling.hpp"
#include "utilities/globalDefinitions.hpp"

namespace sap {

// A pre-allocated slab of memory, a heap of items of type T, including
// freelist management

template <class T, unsigned num_items>
class ItemHeap {
  T _items[num_items];

  struct FreeListEntry {
    FreeListEntry* next;
  };
  STATIC_ASSERT(sizeof(FreeListEntry) <= sizeof(T));
  FreeListEntry* _freelist;

  // Number of items carved out of the heap (only goes up)
  unsigned _hwm;
  // ... of those, number of items in the freelist
  unsigned _in_freelist;

public:

  ItemHeap() {
    reset();
  }

  T* alloc_item() {
    T* item = NULL;
    if (_freelist != NULL) { // take from freelist
      item = (T*) _freelist;
      _freelist = _freelist->next;
      _in_freelist--;
    } else {
      if (_hwm < num_items) {
        item = _items + _hwm;
        _hwm ++;
      }
    }
    return item;
  }

  void return_item(T* item) {
    FreeListEntry* fi = (FreeListEntry*) item;
    fi->next = _freelist;
    _freelist = fi;
    _in_freelist++;
  }

  void reset() {
    ::memset(_items, 0, sizeof(_items));
    _freelist = NULL;
    _in_freelist = 0;
    _hwm = 0;
  };

  // How many items are in use
  unsigned in_use() const {
    return _hwm - _in_freelist;
  }

#ifdef ASSERT
  void verify() const {
    malloctrace_assert(_hwm <= num_items, "sanity");
    malloctrace_assert(_hwm >= _in_freelist, "sanity");
    if (_freelist != NULL) {
      malloctrace_assert(_in_freelist > 0, "sanity");
    } else {
      malloctrace_assert(_in_freelist == 0, "sanity");
    }
  }
#endif
};

} // namespace sap

#endif // OS_LINUX_MALLOCTRACE_SITETABLE_HPP
