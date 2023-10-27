/*
 * Copyright (c) 2005, 2023, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2011, 2023, Red Hat Inc. All rights reserved.
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
#include "nmt/nmt_interposition.hpp"
#include "runtime/os.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"

typedef void* (*malloc_t)(size_t size);
typedef void* (*realloc_t)(void* ptr, size_t size);
typedef void  (*free_t)(void* ptr);

struct malloc_functions_t {
    malloc_t fun_malloc;
    realloc_t fun_realloc;
    free_t fun_free;
};

static malloc_functions_t g_libjvm_callback_functions;
bool NMTInterposition::_enabled = false;

typedef void (*NMTInterposeInitialize_t) (const malloc_functions_t* libjvm_functions, malloc_functions_t* libjvm_callback_functions);

static void* libjvm_malloc(size_t len) {
  return os::malloc(len, mtExternal);
}

static void* libjvm_realloc(void* old, size_t len) {
  return os::realloc(old, len, mtExternal);
}

static void libjvm_free(void* old) {
  os::free(old);
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

void NMTInterposition::initialize() {
  NMTInterposeInitialize_t init_function =
      (NMTInterposeInitialize_t) dlsym(RTLD_DEFAULT, "NMTInterposeInitialize");
  if (init_function != nullptr) {
    malloc_functions_t libjvm_functions;
    libjvm_functions.fun_malloc = libjvm_malloc;
    libjvm_functions.fun_realloc = libjvm_realloc;
    libjvm_functions.fun_free = libjvm_free;
    init_function(&libjvm_functions, &g_libjvm_callback_functions);
    log_debug(os, interpose)("Interpose callbacks: malloc " PTR_FORMAT " realloc " PTR_FORMAT " free " PTR_FORMAT,
                             p2i((void*)g_libjvm_callback_functions.fun_malloc),
                             p2i((void*)g_libjvm_callback_functions.fun_realloc),
                             p2i((void*)g_libjvm_callback_functions.fun_free) );
    assert(g_libjvm_callback_functions.fun_malloc != nullptr, "Sanity");
    assert(g_libjvm_callback_functions.fun_realloc != nullptr, "Sanity");
    assert(g_libjvm_callback_functions.fun_free != nullptr, "Sanity");
    _enabled = true;
    log_info(os, interpose)("Interpose initialized");
  } else {
    log_info(os, interpose)("Interpose entry not found");
  }
}
