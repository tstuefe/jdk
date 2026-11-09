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


#ifndef SHARE_MEMORY_ARENA_ARENAPERCORE_HPP
#define SHARE_MEMORY_ARENA_ARENAPERCORE_HPP


#include "memory/arena/arenaMutex.hpp"
#include "memory/arena/arenaBlock.hpp"
#include "memory/arena/arenaBlockCache.hpp"
#include "memory/arena/arenaSlab.hpp"
#include "utilities/align.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"

// ArenaPerCore contains the logic that holds cache groups
// per core and the access functions needed to effectively select
// the current cache for the core we run on.
//
class ArenaMetadataHeap;
class ArenaSlabGroup;

class ArenaPerCoreDataHolder {
public:

  // This coding has been influenced by zCPU.xxx. Not sure if its worth it on modern kernels,
  // but distros with older glibc (without rseq support for sched_cpu) may benefit.

  struct alignas (DEFAULT_PADDING_SIZE) Data {
    uintptr_t _affinity;
    ArenaMutex _lock;
    ArenaCacheGroup _cache_group; // seg. fits caches for all valid block sizes
    explicit Data(ArenaSlabGroup* slab_group);
  };

private:

  // Note: identity for affinity is really just any number that uniquely identifies
  // this thread. Here, we just use the address of a __thread TLS variable (we just
  // reuse &_cpu). That works and is fast and guaranteed to be unique for every (live)
  // thread. And it does not rely on Thread::current().
  static uintptr_t this_thread_identity() { return (uintptr_t)(&_cpu);}

  const unsigned _num_cpus;
  Data* const _data; // var-sized array, lives in metadata-arena
  
  static THREAD_LOCAL unsigned   _cpu;

  unsigned current_core_slow();

  unsigned current_core() {
    const uintptr_t affinity = at(_cpu)->_affinity;
    if (affinity == this_thread_identity()) {
      return _cpu;
    }
    return current_core_slow();
  }

  Data* at(unsigned cpu) const {
    assert(cpu < _num_cpus, "Sanity");
    return _data + cpu;
  }

  static constexpr uintptr_t invalid_identity = ((uintptr_t)-1);

  // Execute operation on every (active) CPU.
  // Under lock protection. Calls (unsigned cpu, Data* data)
  template <typename Operation>
  void iterate(Operation operation) const {
    for (unsigned i = 0; i < _num_cpus; i++) {
      Data* const data = (_data + i);
      assert(data->_affinity > 0, "Sanity");
      ArenaMutexLocker locker(data->_lock);
      if (data->_affinity != invalid_identity) {
        operation(i, data);
      }
    }
  }

public:

  ArenaPerCoreDataHolder(ArenaMetadataHeap* metadata_heap, ArenaSlabGroup* slab_group);

  Data* get_current_core_data() { return at(current_core()); }

  unsigned num_cpus() const { return _num_cpus; }

  // Tick all caches
  void tick();

  void print_on(outputStream* st) const;

  DEBUG_ONLY(void verify() const);

};

#endif // SHARE_MEMORY_ARENA_ARENAPERCORE_HPP
