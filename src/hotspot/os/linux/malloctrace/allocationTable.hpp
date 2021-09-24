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

#ifndef OS_LINUX_MALLOCTRACE_ALLOCATIONTABLE_HPP
#define OS_LINUX_MALLOCTRACE_ALLOCATIONTABLE_HPP

#include "malloctrace/assertHandling.hpp"
#include "malloctrace/itemHeap.hpp"
#include "utilities/globalDefinitions.hpp"

#ifdef __GLIBC__

class outputStream;

namespace sap {

class Site;

///// SiteTable ////////////////////
// A hashmap containing malloc'ed pointers and references to their Site.
// Space for the nodes is pre-allocated when the table is created. Table
// may overflow, in which case further adds will fail.
class AllocationTable {

  struct Entry {
    Entry* next;
    const void* ptr;
    size_t size;
    Site* site;
  };

  static const unsigned _max_entries = (256 * M) / sizeof(Entry);

  // We preallocate all nodes in this table to avoid
  // swamping the VM with internal malloc calls while the
  // trace is running.
  typedef sap::ItemHeap<Entry, _max_entries> EntryHeap;

  EntryHeap _entryheap;
  const static int table_size = 99991; // prime
  Entry* _table[table_size];

  unsigned _size;        // Number of entries
  uint64_t _num_lost;    // lost adds due to table full

  static unsigned calculate_hash(const void* p) {
    uint32_t v = (uint32_t) (((uintptr_t)p) >> 3);
    v = ~v + (v << 15);
    v = v ^ (v >> 12);
    v = v + (v << 2);
    v = v ^ (v >> 4);
    v = v * 2057;
    v = v ^ (v >> 16);
    return (int) v;
  }

  static unsigned slot_for_pointer(const void* p) {
    return calculate_hash(p) % table_size;
  }

  Entry* remove_entry_for_pointer(const void* p) {
    const unsigned slot = slot_for_pointer(p);
    Entry** pe = &(_table[slot]);
    while (*pe) {
      if ((*pe)->ptr == p) {
        Entry* e = *pe;
        *pe = (*pe)->next;
        _size --;
        return e;
      }
      pe = &((*pe)->next);
    }
    return NULL;
  }

public:

  AllocationTable();

  void add_allocation(const void* p, size_t size, Site* site) {
    malloctrace_assert(remove_entry_for_pointer(p) == NULL, "added twice?");
    Entry* e = _entryheap.alloc_item();
    if (e == NULL) { // hashtable too full, reject.
      _num_lost ++;
      return;
    }
    e->ptr = p;
    e->size = size;
    e->site = site;
    const unsigned slot = slot_for_pointer(p);
    e->next = _table[slot];
    _table[slot] = e;
    _size ++;
  }

  Site* remove_allocation(const void* p, size_t* p_size) {
    Entry* e = remove_entry_for_pointer(p);
    if (e != NULL) {
      Site* site = e->site;
      *p_size = e->size;
      _entryheap.return_item(e);
      return site;
    }
    return NULL;
  }

  void print_stats(outputStream* st) const;
  void reset();
  DEBUG_ONLY(void verify() const;)

  // create a table from c-heap
  static AllocationTable* create();

  // Maximum number of entries the table can hold.
  static unsigned max_entries() { return _max_entries; }

  // Number of entries currently in the table.
  unsigned size() const         { return _size; }

  // Number of invocations lost because table was full.
  uint64_t lost() const         { return _num_lost; }

};

} // namespace sap

#endif // __GLIBC__

#endif // OS_LINUX_MALLOCTRACE_SITETABLE_HPP
