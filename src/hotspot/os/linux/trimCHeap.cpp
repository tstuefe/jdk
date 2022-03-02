/*
 * Copyright (c) 2021, 2022 SAP SE. All rights reserved.
 * Copyright (c) 2021, 2022, Oracle and/or its affiliates. All rights reserved.
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
#include "runtime/os.hpp"
#include "runtime/task.hpp"
#include "utilities/debug.hpp"
#include "utilities/events.hpp"
#include "utilities/ostream.hpp"
#include "trimCHeap.hpp"

#include <malloc.h>

#ifdef __GLIBC__
struct memory_footprint_change_t {
  bool have_values;
  os::Linux::meminfo_t before;
  os::Linux::meminfo_t after;
};

// Trim the glibc heap and optionally report size differences in *info if info != NULL
static void trim_and_measure(memory_footprint_change_t* info) {
  bool have_info = false;
  if (info != nullptr) {
    have_info = os::Linux::query_process_memory_info(&(info->before));
  }
  ::malloc_trim(0);
  if (info != nullptr && have_info) {
    have_info = os::Linux::query_process_memory_info(&(info->after));
  }
  info->have_values = have_info;
}

// Print comparison of virtual size, rss and swap to outputstream
static void print_comparison(const memory_footprint_change_t* info, outputStream* st) {
  bool wrote_something = false;
  if (info->have_values) {
    if (info->before.vmsize != -1 && info->after.vmsize != -1) {
      st->print("virt: " SSIZE_FORMAT "k->" SSIZE_FORMAT "k (" SSIZE_FORMAT "k)",
                info->before.vmsize, info->after.vmsize, (info->after.vmsize - info->before.vmsize));
      wrote_something = true;
    }
    if (info->before.vmrss != -1 && info->after.vmrss != -1) {
      st->print(", rss: " SSIZE_FORMAT "k->" SSIZE_FORMAT "k (" SSIZE_FORMAT "k)",
                info->before.vmrss, info->after.vmrss, (info->after.vmrss - info->before.vmrss));
      wrote_something = true;
    }
    if (info->before.vmswap != -1 && info->after.vmswap != -1) {
      st->print(", swap: " SSIZE_FORMAT "k->" SSIZE_FORMAT "k (" SSIZE_FORMAT "k)",
                info->before.vmswap, info->after.vmswap, (info->after.vmswap - info->before.vmswap));
      wrote_something = true;
    }
  }
  if (!wrote_something) {
    st->print_cr("No details available.");
  }
}

// Report result of a trim operation on (optionally) a stream, UL and event log
static void report_trim_result(const memory_footprint_change_t* info, outputStream* st) {
  // we print the report once and use a stack based array. That avoids C-heap allocations,
  // to avoid muddying the waters.
  char tmp[1024];
  stringStream ss_report(tmp, sizeof(tmp));
  ss_report.print("Trim native heap: ");
  print_comparison(info, &ss_report);

  // Print to outputstream only if given
  if (st != nullptr) {
    st->print_raw(ss_report.base());
  }

  // Print to UL and event log
  log_info(os)("%s", tmp);
  Events::log(NULL, "%s", tmp);
}
#endif // __GLIBC__

void TrimCLibcHeapDCmd::execute(DCmdSource source, TRAPS) {
#ifdef __GLIBC__
  memory_footprint_change_t info;
  trim_and_measure(&info);
  report_trim_result(&info, _output);
  _output->cr();
#else
  st->print_cr("Not available.");
#endif // __GLIBC__
}

class AutoTrimmerTask : public PeriodicTask {
  unsigned _count;
public:
  AutoTrimmerTask(int interval_seconds) : PeriodicTask(interval_seconds * 1000), _count(0) {}
  void task() {
    memory_footprint_change_t info;
    trim_and_measure(&info);
    report_trim_result(&info, NULL);
    _count ++;
  }
  unsigned count() const { return _count; }
};
static AutoTrimmerTask* g_autotrimmer = nullptr;

void AutoTrimCHeap::start() {
  if (AutoTrimNativeHeap) {
#ifdef __GLIBC__
    g_autotrimmer = new AutoTrimmerTask(AutoTrimNativeHeapInterval);
    g_autotrimmer->enroll();
    log_info(os)("Auto C-Heap trimmer engaged (%d second intervals)", AutoTrimNativeHeapInterval);
    Events::log(NULL, "Auto C-Heap trimmer engaged (%d second intervals)", AutoTrimNativeHeapInterval);
#else
    log_warning(os)("AutoTrimNativeHeap requires glibc");
#endif
  }
}

// One liner describing auto trimmer state
void AutoTrimCHeap::report(outputStream* st) {
  if (g_autotrimmer != NULL) {
    st->print_cr("Auto C-Heap trimmer active and ran %u times", g_autotrimmer->count());
  } else {
    st->print_cr("Auto C-Heap trimmer inactive");
  }
}
