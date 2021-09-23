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

#include "precompiled.hpp"
#include "malloctrace/allocationTable.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/ostream.hpp"

#ifdef __GLIBC__

namespace sap {

#ifdef ASSERT
void AllocationTable::verify() const {
  unsigned num_found = 0;
  for (unsigned slot = 0; slot < table_size; slot ++) {
    for (Entry* e = _table[slot]; e != NULL; e = e->next) {
      num_found ++;
      malloctrace_assert(slot_for_pointer(e->ptr) == slot, "hash mismatch");
      malloctrace_assert(e->site != NULL, "sanity");
      malloctrace_assert(e->size != 0, "sanity");
    }
  }
  malloctrace_assert(num_found <= _max_entries && num_found == _size,
         "mismatch (found: %u, max: %u, size: %u)", num_found, _max_entries, _size);
}
#endif // ASSERT

AllocationTable::AllocationTable() {
  reset();
}

void AllocationTable::reset() {
  _size = 0;
  _lost = _collisions = 0;
  ::memset(_table, 0, sizeof(_table));
  _entryheap.reset();
};

AllocationTable* AllocationTable::create() {
  void* p = ::malloc(sizeof(AllocationTable));
  return new(p) AllocationTable;
}

void AllocationTable::print_stats(outputStream* st) const {
  unsigned longest_chain = 0;
  unsigned used_slots = 0;
  for (unsigned slot = 0; slot < table_size; slot ++) {
    unsigned len = 0;
    for (const Entry* e = _table[slot]; e != NULL; e = e->next) {
      len ++;
    }
    longest_chain = MAX2(len, longest_chain);
    if (len > 1) {
      used_slots ++;
    }
  }
  st->print("Table size: %u, num_entries: %u, used slots: %u, longest chain: %u, lost: " UINT64_FORMAT ", collisions: " UINT64_FORMAT,
             table_size, _size, used_slots, longest_chain,
             _lost, _collisions);
}

} // namespace sap

#endif // GLIBC
