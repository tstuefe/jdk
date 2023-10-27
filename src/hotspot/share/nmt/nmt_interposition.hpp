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

#ifndef OS_LINUX_NMT_INTERPOSITION_HPP
#define OS_LINUX_NMT_INTERPOSITION_HPP

#include "memory/allStatic.hpp"
#include "utilities/globalDefinitions.hpp"
#include <sys/mman.h>

class NMTInterposition : public AllStatic {
  static bool _enabled;
public:
  static void initialize();
  static bool enabled() { return _enabled; }

  static void* libjvm_callback_malloc(size_t len);
  static void* libjvm_callback_realloc(void* old, size_t len);
  static void  libjvm_callback_free(void* old);
  static void* libjvm_callback_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
  static int   libjvm_callback_munmap(void *addr, size_t length);
};

// Convenience wrapper
// Call those wherever the libjvm takes care of NMT registration itself. Don't call if the JVM does not
// care about NMT registration.
inline void* raw_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
  return NMTInterposition::enabled() ?
         NMTInterposition::libjvm_callback_mmap(addr, length, prot, flags, fd, offset) :
         ::mmap(addr, length, prot, flags, fd, offset);
}

inline int raw_munmap(void *addr, size_t length) {
  return NMTInterposition::enabled() ?
         NMTInterposition::libjvm_callback_munmap(addr, length) :
         ::munmap(addr, length);
}

#endif // OS_LINUX_NMT_INTERPOSITION_HPP
