/*
 * Copyright (c) 2026, IBM Corp.
 * Copyright (c) 2026, Oracle and/or its affiliates. All rights reserved.
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



#include "memory/arena/arenaBlock.hpp"
#include "memory/arena/arenaBlockCache.hpp"
#include "memory/arena/arenaPerCore.hpp"
#include "memory/arena/arenaSlab.hpp"

#include "runtime/os.hpp"
#include "utilities/debug.hpp"

#include "utilities/globalDefinitions.hpp"
#include "utilities/macros.hpp"
#include "utilities/ostream.hpp"

static constexpr uintptr_t invalid_identity = ((uintptr_t)-1);

THREAD_LOCAL unsigned    ArenaPerCoreDataHolder::_cpu = 0u;


ArenaPerCoreDataHolder::Data::Data(ArenaSlabGroup* slab_group) :
  _affinity(invalid_identity), _lock(), _cache_group(slab_group)
{
}

ArenaPerCoreDataHolder::ArenaPerCoreDataHolder(ArenaMetadataHeap* metadata_heap, ArenaSlabGroup* slab_group) :
  _num_cpus(os::processor_count()),
  _data([this, metadata_heap, slab_group]() {
    assert(_num_cpus > 0, "Sanity");

    metadata_heap->align_hwm_to_padded_size();
    Data* const p = (Data*)metadata_heap->allocate(sizeof(Data) * _num_cpus);
    metadata_heap->align_hwm_to_padded_size();

    // initialize array
    for (unsigned i = 0; i < _num_cpus; i++) {
      new (p + i) Data(slab_group);
    }

    return p;
  } ())
{
}

unsigned ArenaPerCoreDataHolder::current_core_slow() {
  // Set current CPU
  _cpu = os::processor_id();

  // Update affinity
  at(_cpu)->_affinity = this_thread_identity();

  return _cpu;
}
