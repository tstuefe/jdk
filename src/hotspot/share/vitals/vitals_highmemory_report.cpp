/*
 * Copyright (c) 2022 SAP SE. All rights reserved.
 * Copyright (c) 2022, Oracle and/or its affiliates. All rights reserved.
 *
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
#include "gc/shared/collectedHeap.hpp"
#include "memory/metaspaceUtils.hpp"
#include "memory/universe.hpp"
#include "runtime/arguments.hpp"
#include "runtime/globals.hpp"
#include "runtime/os.hpp"
#include "services/memTracker.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/ostream.hpp"
#include "utilities/vmError.hpp"
#include "vitals/vitals_internals.hpp"
#include "vitals/vitalsLocker.hpp"

#include "runtime/thread.hpp"
#include "runtime/nonJavaThread.hpp"
#include <sys/types.h>
#include <sys/wait.h>

#include <sys/prctl.h>

namespace sapmachine_vitals {

static bool g_high_memory_report_done = false; // We only do this once, if at all

static Lock g_highmem_report_lock("himemlck");

static void print_high_memory_report_header(outputStream* st) {
  char tmp[255];
  st->print_cr("#");
  st->print_cr("# High Memory Threshold reached (" SIZE_FORMAT ").", HighMemoryThreshold);
  st->print("# ");
  os::print_date_and_time(st, tmp, sizeof(tmp));
  st->print_cr("#");
}

static void print_high_memory_report(outputStream* st) {

  AutoLock autlock(&g_highmem_report_lock);

  // Note that this report may be interrupted by VM death, e.g. OOM killed.
  // Therefore we frequently flush, and print the most important things first.

  char buf[O_BUFLEN];

  st->print_cr("#");
  st->print_cr("# High Memory Threshold reached (" SIZE_FORMAT ").", HighMemoryThreshold);
  st->print_cr("#");

  // Most important things first:
  // 1) Vitals

  st->print_cr("Vitals:");
  sapmachine_vitals::print_info_t info;
  sapmachine_vitals::default_settings(&info);
  info.sample_now = true;
  info.no_legend = true;
  sapmachine_vitals::print_report(st, &info);

  st->cr();
  st->cr();
  st->flush();

  // 2) NMT detail report (if available, summary otherwise)
  st->cr();
  st->print_cr("Native Memory Tracking:");
  if (MemTracker::enabled()) {
    MemTracker::vitals_highmemory_report(st);
  } else {
    st->print_cr("disabled.");
  }

  st->cr();
  st->cr();
  st->flush();

  st->print_cr("vm_info: %s", VM_Version::internal_vm_info_string());
  os::print_summary_info(st, buf, sizeof(buf));
  Arguments::print_summary_on(st);

  st->cr();
  st->cr();
  st->flush();

  os::print_os_info(st);

  st->print_cr("#");
  st->print_cr("# END: High Memory Report");
  st->print_cr("#");

  st->flush();
}

// Called by platform samplers
void trigger_high_memory_report() {

  // Note: no tty since I don't want to deal with tty lock recursion or contention
  fdStream fds(2);
  outputStream* const stderr_stream = &fds;
  bool failed_to_open_dump_file = false;

  if (!g_high_memory_report_done) {
    g_high_memory_report_done = true;
    if (DumpReportOnHighMemory) {
      char filename[255];
      jio_snprintf(filename, sizeof(filename), "sapmachine_highmemory_%d.log", os::current_process_id());
      fileStream fs(filename);
      if (fs.is_open()) {
        // Print a short note to stderr
        print_high_memory_report_header(stderr_stream);
        stderr_stream->print_cr("# Dumping report to %s.", filename);
        stderr_stream->print_cr("#");
        // Print full report to dump file
        print_high_memory_report(&fs);
      } else {
        failed_to_open_dump_file = true;
        stderr_stream->print_cr("Failed to open %s for writing. Printing to stderr instead.", filename);
      }
    }
    if (PrintReportOnHighMemory || failed_to_open_dump_file) {
      print_high_memory_report(stderr_stream);
    }
  }
}

static void* spawn_oom_killer_decoy_process(void* dummy) {
  int child = ::fork();
  if (child == 0) {
    printf("oom killer live (%d)\n", ::getpid()); ::fflush(stdout);
    // Obnoxious child, raise oom killer probability drastically
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    prctl(PR_SET_NAME, "oomdecoy");
    int fd = ::open("/proc/self/oom_score_adj", O_WRONLY);
    if (fd == -1) {
      printf("Error opening /proc/self/oom_score_ad (%s)\n", ::strerror(errno));
      return NULL;
    }
    int written = ::write(fd, "1000", 5);
    if (written != 5) {
      printf("Error adjusting oom_score_adj (%s)\n", ::strerror(errno));
      return NULL;
    }
    close(fd);
    for (;;) {
      ::sleep(1000);
    }
  } else {
    int status = 0;
    ::waitpid(child, &status, 0);
    printf("oom killer decoy lost: %d %d %d %d\n", WIFEXITED(status), WEXITSTATUS(status), WIFSIGNALED(status), WTERMSIG(status));
    if (WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL) {
      printf("oom killer decoy was killed. May be OOM");
      trigger_high_memory_report();
    }
    fflush(stdout);
  }
  return NULL;
}

void initialize_decoy_watcher_thread() {
  pthread_t pt;
  pthread_attr_t attr;
  ::pthread_attr_init(&attr);
  ::pthread_attr_setstacksize(&attr, 64 * K);
  ::pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  pthread_create(&pt, &attr, spawn_oom_killer_decoy_process, NULL);
}

} // namespace sapmachine_vitals
