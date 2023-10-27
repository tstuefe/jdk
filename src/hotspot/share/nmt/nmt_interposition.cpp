/*
 * Copyright (c) 2023, Red Hat Inc. All rights reserved.
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

#include "logging/log.hpp"
#include "nmt/memTracker.hpp"
#include "nmt/nmt_interposition.hpp"
#include "nmt/virtualMemoryTracker.hpp"
#include "runtime/os.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"

#include <sys/mman.h>

typedef void* (*malloc_t)(size_t size);
typedef void* (*realloc_t)(void* ptr, size_t size);
typedef void  (*free_t)(void* ptr);

typedef void* (*mmap_t) (void *addr, size_t length, int prot, int flags, int fd, off_t offset);
typedef int (*munmap_t) (void *addr, size_t length);

struct functions_t {
    malloc_t fun_malloc;
    realloc_t fun_realloc;
    free_t fun_free;
    mmap_t fun_mmap;
    munmap_t fun_munmap;
};

static functions_t g_libjvm_callback_functions;
bool NMTInterposition::_enabled = false;

typedef void (*NMTInterposeInitialize_t) (const functions_t* libjvm_functions, functions_t* libjvm_callback_functions);

static void* libjvm_malloc(size_t len) {
  return os::malloc(len, mtExternal);
}

static void* libjvm_realloc(void* old, size_t len) {
  return os::realloc(old, len, mtExternal);
}

static void libjvm_free(void* old) {
  os::free(old);
}

static void* libjvm_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
  // Call raw mmap but afterwards register the area with NMT
  void* p = g_libjvm_callback_functions.fun_mmap(addr, length, prot, flags, fd, offset);
  if (p != (void*)MAP_FAILED) {
    VirtualMemoryTracker::add_reserved_region((address)p, length, CALLER_PC, mtExternal);
    if ((flags & MAP_NORESERVE) == 0) {
      VirtualMemoryTracker::add_committed_region((address)p, length, CALLER_PC);
    }
  }
  return p;
}

static int libjvm_munmap(void *addr, size_t length) {
  // Call raw munmap but afterwards unregister the area with NMT
  int rc = g_libjvm_callback_functions.fun_munmap(addr, length);
  if (rc == 0) {
    VirtualMemoryTracker::remove_released_region((address)addr, length);
  }
  return rc;
}

void* NMTInterposition::libjvm_callback_malloc(size_t len) {
  return g_libjvm_callback_functions.fun_malloc(len);
}
void* NMTInterposition::libjvm_callback_realloc(void* old, size_t len) {
  return g_libjvm_callback_functions.fun_realloc(old, len);
}
void  NMTInterposition::libjvm_callback_free(void* old) {
  g_libjvm_callback_functions.fun_free(old);
}
void* NMTInterposition::libjvm_callback_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
  return g_libjvm_callback_functions.fun_mmap(addr, length, prot, flags, fd, offset);
}
int   NMTInterposition::libjvm_callback_munmap(void *addr, size_t length) {
  return g_libjvm_callback_functions.fun_munmap(addr, length);
}

void NMTInterposition::initialize() {
  NMTInterposeInitialize_t init_function =
      (NMTInterposeInitialize_t) dlsym(RTLD_DEFAULT, "NMTInterposeInitialize");
  if (init_function != nullptr) {
    functions_t libjvm_functions;
    libjvm_functions.fun_malloc = libjvm_malloc;
    libjvm_functions.fun_realloc = libjvm_realloc;
    libjvm_functions.fun_free = libjvm_free;
    libjvm_functions.fun_mmap = libjvm_mmap;
    libjvm_functions.fun_munmap = libjvm_munmap;
    init_function(&libjvm_functions, &g_libjvm_callback_functions);
    log_debug(os, interpose)("Interpose callbacks: malloc " PTR_FORMAT " realloc " PTR_FORMAT " free " PTR_FORMAT " mmap " PTR_FORMAT " munmap " PTR_FORMAT,
                             p2i((void*)g_libjvm_callback_functions.fun_malloc),
                             p2i((void*)g_libjvm_callback_functions.fun_realloc),
                             p2i((void*)g_libjvm_callback_functions.fun_free),
                             p2i((void*)g_libjvm_callback_functions.fun_mmap),
                             p2i((void*)g_libjvm_callback_functions.fun_munmap) );
    assert(g_libjvm_callback_functions.fun_malloc != nullptr, "Sanity");
    assert(g_libjvm_callback_functions.fun_realloc != nullptr, "Sanity");
    assert(g_libjvm_callback_functions.fun_free != nullptr, "Sanity");
    assert(g_libjvm_callback_functions.fun_mmap != nullptr, "Sanity");
    assert(g_libjvm_callback_functions.fun_munmap != nullptr, "Sanity");
    _enabled = true;
    log_info(os, interpose)("Interpose initialized");
  } else {
    log_info(os, interpose)("Interpose entry not found");
  }
}
