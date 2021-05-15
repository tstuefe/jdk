/*
 * Copyright (c) 2021 SAP SE. All rights reserved.
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
#include "jvm_io.h"
#include "runtime/atomic.hpp"
#include "runtime/thread.hpp"
#include "utilities/fatalError.hpp"

static char fatal_detail_buffer[1024];
static volatile int buffer_in_use = 0;
static volatile int fatal_error_count = 0;

char* FatalError::resolve_detail_message(const char* details, va_list detail_args) {
  if (Atomic::cmpxchg(&buffer_in_use, 0, 1) == 0) {
    jio_vsnprintf(fatal_detail_buffer, sizeof(fatal_detail_buffer), details, detail_args);
    return fatal_detail_buffer;
  }
  return NULL;
}

FatalError::FatalError(FatalErrorType type) :
  _type(type),
  _count(Atomic::inc(&fatal_error_count)),
  _thread(Thread::current_or_null_safe()),
  _next(NULL)
{}

// ------

FatalNonCrashError::FatalNonCrashError(FatalErrorType type, const char* message,
                                       const char* details, va_list details_args,
                                       const char* file, int line) :
  FatalError(type),
  _message(message),
  _detail(resolve_detail_message(details, details_args)),
  _file(file), _line(line)
{}

// ------

FatalAssertionError::FatalAssertionError(const char* message,
                                         const char* details, va_list details_args,
                                         const char* file, int line, const void* context) :
  FatalNonCrashError(FatalErrorType::fatal_assertion, message, details, details_args, file, line),
  _context(context)
{}

// ------

FatalOOMError::FatalOOMError(const char* message,
                             const char* details, va_list details_args,
                             const char* file, int line,
                             FatalOOMError oom_type, size_t failsize) :
  FatalNonCrashError(FatalErrorType::fatal_oom, message, details, details_args, file, line),
  _oom_type(oom_type), _failsize(failsize)
{}

// ------

FatalCrash::FatalCrash(signo_t signo, const void* context, const void* siginfo) :
  FatalError(FatalErrorType::fatal_crash),
  _signal_number(signo),
  _context(context),
  _siginfo(siginfo)
{}
