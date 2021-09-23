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

  unsigned _used;

public:

  ItemHeap() : _freelist(NULL), _used(0) {
    ::memset(_items, 0, sizeof(_items));
  }

  T* alloc_item() {
    T* item = NULL;
    if (_freelist != NULL) {
      item = (T*) _freelist;
      _freelist = _freelist->next;
    } else {
      if (_used < num_items) {
        item = _items + _used;
        _used ++;
      }
    }
    return item;
  }

  void return_item(T* item) {
#ifdef ASSERT
    ::memset(item, 0, sizeof(T));
#endif
    FreeListEntry* fi = (FreeListEntry*) item;
    fi->next = _freelist;
    _freelist = fi;
    _used --;
  }

  void reset() {
    ::memset(_items, 0, sizeof(_items));
    _used = 0;
    _freelist = NULL;
  };

};

} // namespace sap

#endif // OS_LINUX_MALLOCTRACE_SITETABLE_HPP
