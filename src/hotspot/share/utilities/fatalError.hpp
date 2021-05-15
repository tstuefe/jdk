/*
 * Copyright (c) 2014, 2019, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_UTILITIES_FATALERROR_HPP
#define SHARE_UTILITIES_FATALERROR_HPP

#include "utilities/globalDefinitions.hpp"

class Thread;
class outputStream;

// In the VM, we have three classes of fatal errors. Each one is handled slightly differently, and
//  each one carries different detail information:
//
// - Assertions (assert, guarantee, report_xxx_error, ShouldNotReachHere etc):
//   These come with File, Line, a summary message and a detail message, typically with variadic args.
//   They also carry a context if "ShowRegistersOnAssert" is active.
//
// - OOMs
//   They carry File, Line, summary and detail message. In addition to that they carry a memory type
//   and a failsize parameter, but no context.
//
// - Crashes
//   They carry a signal number (Windows: SEH code). They also carry a context and a signalinfo
//   (Windows: an SEH ExceptionInfo).
//
// Buffer handling:
//
// We need memory to carry the resolved variadic detail args. But we should be very careful with
//  dynamically allocating memory in error situations. But fatal errors are a one-time thing: the
//  first fatal error "wins" the reporting race and followup errors in concurrent threads are stalled
//  in VMError::report_and_die(). So we work with a single pre-allocated static buffer which the first
//  error will occupy and never release, and followup errors will just not have a detail message.
//

enum class FatalErrorType {
  fatal_assertion = 0,
  fatal_oom = 2,
  fatal_crash = 3
};

enum class FatalOOMErrorType {
  fatal_oom_malloc = 0,
  fatal_oom_mprotect = 1,
  fatal_oom_mmap = 2,
  fatal_oom_java = 3,
  fatal_oom_undefined = 0xffff
};

// a type for signal number
typedef WINDOWS_ONLY(DWORD) NOT_WINDOWS(int) signo_t;

// Assertions:
//  - message
//  - resolved detail message
//  - context (if ShowRegistersOnAssert)
//  - file+line
// OOMs:
//  - message
//  - resolved detail message
//  - oom type and failsize
//  - file+line
// Crash:
//  - signal number
//  - context
//  - signal info

// All of them:
//  - error type
//  - a unique error counter

class FatalError {

  const FatalErrorType  _type;
  const int             _count;

  // the thread this error happened in; may be NULL.
  const Thread* const   _thread;

  // we keep concurrent/secondary errors in a list for diagnostics
  FatalError*           _next;

protected:

  // Resolve format string; uses an internal static buffer; can only be used once -
  // followup calls will return the unresolved details format string instead.
  static char* resolve_detail_message(const char* details, va_list detail_args);

  FatalError(FatalErrorType type);

public:

  FatalErrorType type() const { return _type; }

  // convenience methods
  bool is_assertion() const { return _type == FatalErrorType::fatal_assertion; }
  bool is_oom() const       { return _type == FatalErrorType::fatal_oom; }
  bool is_crash() const     { return _type == FatalErrorType::fatal_crash; }

  virtual const char* message() const   { return nullptr; }
  virtual const char* detail() const    { return nullptr; }
  virtual const char* file() const      { return nullptr; }
  virtual const char* line() const      { return nullptr; }

  FatalOOMErrorType oom_type() const    { return FatalOOMErrorType::fatal_oom_undefined; }
  size_t failsize() const               { return 0; }

  signo_t signal_number() const         { return 0; }
  virtual const void* context() const   { return nullptr; }
  virtual const void* siginfo() const   { return nullptr; }

};

class FatalNonCrashError : public FatalError {

  const char* const _message;
  const char* const _detail;
  const char* const _file;
  const int         _line;

protected:

  FatalNonCrashError(FatalErrorType type, const char* message, const char* details, va_list details_args, const char* file, int line);

public:

  const char* message() const override  { return _message; }
  const char* detail() const override   { return _detail; }
  const char* file() const override     { return _file; }
  const char* line() const override     { return _line; }

};


class FatalAssertionError : public FatalNonCrashError {

  // Only if ShowRegistersOnAssert:
  const void* const _context;

public:
  FatalAssertionError(const char* message, const char* details, va_list details_args, const char* file, int line, const void* context);

  const void* context() const override { return _context; }
};

class FatalOOMError : public FatalNonCrashError {

  const FatalOOMErrorType _oom_type;
  const size_t            _failsize;

public:

  FatalOOMError(const char* message, const char* details, va_list details_args, const char* file, int line,
                FatalOOMError oom_type, size_t failsize);

  FatalOOMErrorType oom_type() const override { return _oom_type; }
  size_t failsize() const  override           { return _failsize; }
};

class FatalCrash : public FatalError {

  const signo_t         _signal_number;
  const void* const     _context;
  const void* const     _siginfo;

public:

  FatalCrash(signo_t signo, const void* context, const void* siginfo);

  signo_t signal_number() const override { return _signal_number; }
  const void* context() const override  { return _context; }
  const void* siginfo() const override  { return _siginfo; }
};

#endif // SHARE_UTILITIES_FATALERROR_HPP













