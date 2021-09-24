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
#include "malloctrace/assertHandling.hpp"
#include "malloctrace/locker.hpp"
#include "malloctrace/mallocTrace.hpp"
#include "malloctrace/siteTable.hpp"
#include "memory/allStatic.hpp"
#include "runtime/globals.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/ostream.hpp"

#include <malloc.h>

#ifdef __GLIBC__

namespace sap {

// Needed to stop the gcc from complaining about malloc hooks being deprecated.
PRAGMA_DISABLE_GCC_WARNING("-Wdeprecated-declarations")

typedef void* (*malloc_hook_fun_t) (size_t len, const void* caller);
typedef void* (*realloc_hook_fun_t) (void* old, size_t len, const void* caller);
typedef void* (*memalign_hook_fun_t) (size_t alignment, size_t size, const void* caller);
typedef void  (*free_hook_fun_t) (void* old, const void* caller);

static void* my_malloc_hook(size_t size, const void *caller);
static void* my_realloc_hook(void* old, size_t size, const void *caller);
static void* my_memalign_hook(size_t alignment, size_t size, const void *caller);
static void  my_free_hook(void* old, const void *caller);

// Hook changes, hook ownership:
//
// Hooks are a global resource and everyone can change them concurrently. In practice
// this does not happen often, so using them for our purposes here is generally safe
// and we can generally rely on us being the sole changer of hooks.
//
// Exceptions:
// 1) gdb debugging facilities like mtrace() or MALLOC_CHECK_ use them too
// 2)  there is a initialization race: both hooks are initially set to glibc-internal
//    initialization functions which will do some stuff, them set them to NULL for the
//    rest of the program run. These init functions (malloc_hook_ini() and realloc_hook_ini()),
//    see malloc/hooks.c) run *lazily*, the first time malloc or realloc is called.
//    So there is a race window here where we could possibly install our hooks while
//    some other thread calls realloc, still sees the original function pointer, executed
//    the init function and resets our hook. To make matters worse and more surprising, the
//    realloc hook function also resets the malloc hook for some reason (I consider this a
//    bug since realloc(3) may run way later than malloc(3)).
//
// There is nothing we can do about (1) except, well, not do it. About (2), we can effectively
//  prevent that from happening by calling malloc and realloc very early. The earliest we
//  can manage is during C++ dyn init of the libjvm:
struct RunAtDynInit {
  RunAtDynInit() {
    // Call malloc, realloc, free, calloc and posix_memalign.
    // This may be overkill, but I want all hooks to have executed once, in case
    // they have side effects on the other hooks (like the realloc hook which resets the malloc
    // hook)
    void* p = ::malloc(10);
    p = ::realloc(p, 20);
    ::free(p);
    if (::posix_memalign(&p, 8, 10) == 0) {
      ::free(p);
    }
  }
};
static RunAtDynInit g_run_at_dyn_init;

class HookControl : public AllStatic {
  static bool _hooks_are_active;
  static malloc_hook_fun_t    _old_malloc_hook;
  static realloc_hook_fun_t   _old_realloc_hook;
  static memalign_hook_fun_t  _old_memalign_hook;
  static free_hook_fun_t      _old_free_hook;

public:

#ifdef ASSERT
  static void verify() {
    if (_hooks_are_active) {
      malloctrace_assert(__malloc_hook == my_malloc_hook && __realloc_hook == my_realloc_hook &&
                         __memalign_hook == my_memalign_hook,
                         "Expected my hooks to be active, but found: "
                         "__malloc_hook=" PTR_FORMAT ", __realloc_hook=" PTR_FORMAT
                         ", __memalign_hook=" PTR_FORMAT " instead.",
                         (intptr_t)__malloc_hook, (intptr_t)__realloc_hook,
                         (intptr_t)__memalign_hook);
    } else {
      malloctrace_assert(__malloc_hook != my_malloc_hook && __realloc_hook != my_realloc_hook &&
                         __memalign_hook != my_memalign_hook,
                         "Expected my hooks to be inactive, but found: "
                         "__malloc_hook=" PTR_FORMAT ", __realloc_hook=" PTR_FORMAT
                         ", __memalign_hook=" PTR_FORMAT " instead.",
                         (intptr_t)__malloc_hook, (intptr_t)__realloc_hook,
                         (intptr_t)__memalign_hook);
    }
  }
#endif

  // Return true if my hooks are active
  static bool hooks_are_active() {
    DEBUG_ONLY(verify();)
    return _hooks_are_active;
  }

  static void enable() {
    DEBUG_ONLY(verify();)
    malloctrace_assert(!hooks_are_active(), "Sanity");
    _old_malloc_hook = __malloc_hook;
    __malloc_hook = my_malloc_hook;
    _old_realloc_hook = __realloc_hook;
    __realloc_hook = my_realloc_hook;
    _old_memalign_hook = __memalign_hook;
    __memalign_hook = my_memalign_hook;
    _old_free_hook = __free_hook;
    __free_hook = my_free_hook;
    _hooks_are_active = true;
  }

  static void disable() {
    DEBUG_ONLY(verify();)
    malloctrace_assert(hooks_are_active(), "Sanity");
    __malloc_hook = _old_malloc_hook;
    __realloc_hook = _old_realloc_hook;
    __memalign_hook = _old_memalign_hook;
    __free_hook = _old_free_hook;
    _hooks_are_active = false;
  }

  static void disable_on_error() {
    // Disable before asserting: just set them to raw NULL, this is
    // safest in case we have a problem with our internal logic.
    // Also, don't assert.
    __malloc_hook = NULL;
    __realloc_hook = NULL;
    __memalign_hook = NULL;
    __free_hook = NULL;
    _hooks_are_active = false;
  }
};

bool HookControl::_hooks_are_active = false;
malloc_hook_fun_t HookControl::_old_malloc_hook = NULL;
realloc_hook_fun_t HookControl::_old_realloc_hook = NULL;
memalign_hook_fun_t HookControl::_old_memalign_hook = NULL;
free_hook_fun_t HookControl::_old_free_hook = NULL;

// A stack mark for temporarily disabling hooks - if they are active - and
// restoring the old state
class DisableHookMark {
  const bool _state;
public:
  DisableHookMark() : _state(HookControl::hooks_are_active()) {
    if (_state) {
      HookControl::disable();
    }
  }
  ~DisableHookMark() {
    if (_state) {
      HookControl::enable();
    }
  }
};

/////////////////////////////////////////////////////////////////

static SiteTable* g_sites = NULL;

static bool g_track_memory = false;
static AllocationTable* g_allocations = NULL;

static uint64_t g_num_captures = 0;
static uint64_t g_num_captures_without_stack = 0;

#ifdef ASSERT
static int g_times_enabled = 0;
static int g_times_printed = 0;
#endif

static void unregister_allocation(const void* ptr) {
  if (g_track_memory) {
    malloctrace_assert(g_sites != NULL, "Site table not allocated");
    malloctrace_assert(g_allocations != NULL, "Allocation table not allocated");
    size_t old_size = 0;
    Site* const site = g_allocations->remove_allocation(ptr, &old_size);
    if (site != NULL) {
      // Note: we may have neg. overflow due to missed mallocs. Just cap at 0.
      if (site->num_outstanding_allocations > 0) {
        site->num_outstanding_allocations --;
      }
      if (site->num_outstanding_bytes >= old_size) {
        site->num_outstanding_bytes -= old_size;
      } else {
        site->num_outstanding_bytes = 0;
      }
    }
  }
}

static void register_allocation_with_stack(const Stack* stack, const void* ptr, size_t alloc_size) {

  // First attempt to unregister the pointer. This is because we already may have the pointer in
  // our allocation hash table, and may have missed the free. If we get the same pointer again,
  // we must have missed the free, so retroactively unregister.
  unregister_allocation(ptr);

  Site* const site = g_sites->find_or_add_site(stack); // already increases invoc counters
  if (g_track_memory) {
    site->num_outstanding_allocations ++;
    site->num_outstanding_bytes += alloc_size;
    g_allocations->add_allocation(ptr, alloc_size, site);
  }
}

static void* my_malloc_hook(size_t alloc_size, const void *caller) {
  Locker lck;
  g_num_captures ++;

  // If someone switched off tracing while we waited for the lock, just quietly do
  // malloc/realloc and tippytoe out of this function. Don't modify hooks, don't
  // collect stacks.
  if (HookControl::hooks_are_active() == false) {
    return ::malloc(alloc_size);
  }

  // For the duration of the malloc call, disable hooks.
  //
  // Concurrency note: Concurrent threads will not be disturbed by this since:
  // - either they already entered this function, in which case they wait at the lock
  // - or they call malloc/realloc after we restored the hooks. In that case they
  //   just will end up doing the original malloc. We loose them for the statistic,
  //   but we wont disturb them, nor they us.
  //   (caveat: we assume here that the order in which we restore the hooks - which
  //    will appear random for outside threads - does not matter. After studying the
  //    glibc sources, I believe it does not.)
  HookControl::disable();

  // Do the actual allocation for the caller
  void* p = ::malloc(alloc_size);

  // Reinstate my hooks
  HookControl::enable();

  // all the subsequence code in this function is guaranteed not to malloc itself:
  if (p != NULL) {
    Stack stack;
    if (Stack::capture_stack(&stack)) {
      register_allocation_with_stack(&stack, p, alloc_size);
#ifdef ASSERT
      if ((g_num_captures % 10000) == 0) { // expensive, do this only sometimes
        g_sites->verify();
      }
/*      if (g_track_memory && (g_num_captures % 100000) == 0) { // expensive, do this only sometimes
        g_allocations->verify();
      }*/
#endif
    } else {
      g_num_captures_without_stack ++;
    }
  }


  return p;
}

static void* my_realloc_hook(void* old, size_t alloc_size, const void *caller) {

  if (old == NULL) {
    return my_malloc_hook(alloc_size, caller);
  }

  // >> For explanations, see my_malloc_hook <<

  Locker lck;
  g_num_captures ++;

  if (HookControl::hooks_are_active() == false) {
    return ::realloc(old, alloc_size);
  }

  // We treat realloc as free+malloc
  if (old != NULL) {
    unregister_allocation(old);
  }

  HookControl::disable();
  void* p = ::realloc(old, alloc_size);
  HookControl::enable();

  if (p != NULL) {
    Stack stack;
    if (Stack::capture_stack(&stack)) {
      register_allocation_with_stack(&stack, p, alloc_size);
    } else {
      g_num_captures_without_stack ++;
    }
  }

  return p;

}

static void* posix_memalign_wrapper(size_t alignment, size_t size) {
  void* p = NULL;
  if (::posix_memalign(&p, alignment, size) == 0) {
    return p;
  }
  return NULL;
}

static void* my_memalign_hook(size_t alignment, size_t alloc_size, const void *caller) {
  Locker lck;
  g_num_captures ++;

  // >> For explanations, see my_malloc_hook <<

  if (HookControl::hooks_are_active() == false) {
    return posix_memalign_wrapper(alignment, alloc_size);
  }

  HookControl::disable();
  void* p = posix_memalign_wrapper(alignment, alloc_size);
  HookControl::enable();

  if (p != NULL) {
    Stack stack;
    if (Stack::capture_stack(&stack)) {
      register_allocation_with_stack(&stack, p, alloc_size);
    } else {
      g_num_captures_without_stack ++;
    }
  }

  return p;
}

static void my_free_hook(void* old, const void *caller) {
  Locker lck;

  if (HookControl::hooks_are_active() == false) {
    ::free(old);
    return;
  }

  HookControl::disable();

  // Do the actual free for the caller
  ::free(old);

  // Reinstate my hooks
  HookControl::enable();

  unregister_allocation(old);

}

/////////// Externals /////////////////////////

#define PRINT_SAFELY_TO_STREAM(st, ...) \
if (st != NULL) { \
  st->print_cr(__VA_ARGS__); \
}

void MallocTracer::enable(outputStream* st, bool trace_allocations) {
  Locker lck;
  if (!HookControl::hooks_are_active()) {
    if (g_sites == NULL) {
      // First time malloc trace is enabled, allocate the site table. We don't want to preallocate it
      // unconditionally since it costs several MB.
      g_sites = SiteTable::create();
      if (g_sites != NULL) {
        PRINT_SAFELY_TO_STREAM(st, "Callsite table allocated.");
      } else {
        PRINT_SAFELY_TO_STREAM(st, "No memory for call table");
        return;
      }
    }
    g_track_memory = false;
    if (trace_allocations && g_allocations == NULL) {
      g_allocations = AllocationTable::create();
      if (g_allocations != NULL) {
        g_track_memory = true;
        PRINT_SAFELY_TO_STREAM(st, "Allocation table allocated.");
      } else {
        g_track_memory = false;
        PRINT_SAFELY_TO_STREAM(st, "No memory for allocation table -> allocation trace will remain disabled (only counting invocations, not outstanding bytes)");
      }
    }
    HookControl::enable(); // << from this moment on concurrent threads may enter our hooks but will then wait on the lock
    DEBUG_ONLY(g_times_enabled ++;)
    PRINT_SAFELY_TO_STREAM(st, "Hooks enabled (trace: %d).", g_track_memory);
  } else {
    PRINT_SAFELY_TO_STREAM(st, "Hooks already enabled (trace: %d), nothing changed.", g_track_memory);
  }
  return;
}

void MallocTracer::disable(outputStream* st) {
  Locker lck;
  if (HookControl::hooks_are_active()) {
    HookControl::disable();
    PRINT_SAFELY_TO_STREAM(st, "Hooks disabled.");
  } else {
    PRINT_SAFELY_TO_STREAM(st, "Hooks already disabled, nothing changed.");
  }
}

void MallocTracer::disable_on_error() {
  HookControl::disable_on_error();
}
volatile void* pppp;
void MallocTracer::reset(outputStream* st) {
  {Locker lck;
  g_num_captures = g_num_captures_without_stack = 0;
  if (g_sites != NULL) {
    g_sites->reset();
    PRINT_SAFELY_TO_STREAM(st, "Callsite table was reset.");
  }
  if (g_allocations != NULL) {
    g_allocations->reset();
    PRINT_SAFELY_TO_STREAM(st, "Allocation table was reset.");
  }}
  for (int i = 0; i < 1000000; i++) pppp=::malloc(1);
}

void MallocTracer::print(outputStream* st, bool all) {
  Locker lck;
  if (g_sites != NULL) {
    bool state_now = HookControl::hooks_are_active(); // query hooks before temporarily disabling them
    {
      DisableHookMark disableHookMark;
      g_sites->print_table(st, all);
      st->print("Callsite table stats: ");
      g_sites->print_stats(st);
      st->cr();
      if (g_allocations != NULL) {
        st->print("Allocation table stats: ");
        g_allocations->print_stats(st);
        st->cr();
      }
      st->print_cr("Malloc trace %s.", state_now ? "on" : "off");
      st->cr();
      st->print_cr(UINT64_FORMAT " captures (" UINT64_FORMAT " without stack).", g_num_captures, g_num_captures_without_stack);
      DEBUG_ONLY(g_times_printed ++;)
      DEBUG_ONLY(st->print_cr("%d times enabled, %d times printed", g_times_enabled, g_times_printed));
      DEBUG_ONLY(g_sites->verify();)
    }
  } else {
    // Malloc trace has never been activated.
    st->print_cr("Malloc trace off.");
  }
}

void MallocTracer::print_on_error(outputStream* st) {
  // Don't lock. Don't change hooks. Just print the table stats.
  if (g_sites != NULL) {
    g_sites->print_stats(st);
  }
  if (g_allocations != NULL) {
    g_allocations->print_stats(st);
  }
}

///////////////////////

// test: enable at libjvm load
// struct AutoOn { AutoOn() { MallocTracer::enable(); } };
// static AutoOn g_autoon;

#endif // GLIBC

} // namespace sap
