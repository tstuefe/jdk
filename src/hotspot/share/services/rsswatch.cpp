/*
 * Copyright (c) 2023, Red Hat, Inc. All rights reserved.
 * Copyright (c) 2023, Oracle and/or its affiliates. All rights reserved.
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

#include "compiler/compilationMemoryStatistic.hpp"
#include "logging/log.hpp"
#include "logging/logStream.hpp"
#include "nmt/memTracker.hpp"
#include "nmt/memMapPrinter.hpp"
#include "runtime/java.hpp"
#include "runtime/globals.hpp"
#include "runtime/os.hpp"
#include "runtime/task.hpp"
#include "services/rsswatch.hpp"
#include "utilities/parseInteger.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"


static double now() { return os::elapsedTime(); }

static size_t percent_of_physical(double percent) {
  return (size_t)((double)os::physical_memory() * (percent / 100.0));
}

class RssLimitTask : public PeriodicTask {

  // The error threshold we must surpass to dump an RSS report. It is either the
  // absolute limit or calculated (and possibly recalculated) from the _percent_limit.
  // Upon reaching this threshold, we write an info dump (to UL with os+rss flags) and
  // optionally end the VM with a fatal native OOM error.
  // Note: if we don't end the VM, the OOM report is only generated once; subsequent
  // RSS peaks are ignored.
  size_t _threshold_100;

  // The time at which we reached the _threshold_100 (0.0 if not reached)
  double _time_threshold_100;

  // The warning threshold; upon reaching this threshold, we print out a first warning
  // RSS report (to UL with os+rss flags). It is 80% of the error threshold.
  // Only generated once.
  size_t _threshold_80;

  // The time at which we reached the _time_threshold_80 (0.0 if not reached)
  double _time_threshold_80;

  // The absolute limit (RssLimit), if one was given, in number of bytes; 0 otherwise
  const size_t _absolute_limit;

  // The relative limit (RssLimitPercent) in a (0.01 .. 100.0) range;
  // 0.0 if not set
  const double _percent_limit;

  // The time that needs to pass between re-calculations of the percentage limit
  // (when os::physical_memory() changes)
  static constexpr double limit_min_seconds_between_recalculating = 5.0f;

  // When we last recalculated the percentage limit
  double _last_limit_recalculation_time;

  // Whether to trigger a fatal error when reaching the threshold
  const bool _is_fatal;

  // Number of total ticks
  uint64_t _ticks;

  // A history of recent measurements, FIFO buffer; for very high check frequencies, we spread
  // these measurements out to at least 16 seconds
  static constexpr int history_size = 16;
  static constexpr double history_min_seconds_elapsed_between_samples = 1.0f;
  double _last_history_add_time;
  struct {
    double _time;
    size_t _rss;
  } _history[history_size];
  int _history_pos;

  void add_history(size_t rss, double t_now) {
    _history[_history_pos]._rss = rss;
    _history[_history_pos]._time = t_now;
    _history_pos++;
    _history_pos = (_history_pos + 1) % history_size;
    _last_history_add_time = t_now;
  }

  void print_history(outputStream* st) const {
    int pos = _history_pos;
    for (int i = 0; i < history_size; i ++) {
      const size_t rss = _history[pos]._rss;
      if (rss > 0) {
        os::print_elapsed_time(st, _history[pos]._time);
        st->print_cr(": %zu", rss);
      }
      pos = (pos + 1) % history_size;
    }
  }

  void log_report(char* headerline, size_t rss_now, double t_now) {

    // Header line with warning level
    log_warning(os, rss)("%s", headerline);

    Log(os, rss) log;
    if (!log.is_info()) {
      log_warning(os, rss)("(enable -Xlog:os+rss to get more information)");
      return;
    }

    // Full report with info level
    LogStream ls(log.info());

    ls.cr();
    ls.print_cr("Settings:");
    {
      StreamIndentor si(&ls, 4);
      print_state(&ls);
    }

    ls.print_cr("RSS History:");
    {
      StreamIndentor si(&ls, 4);
      print_history(&ls);
    }
    ls.cr();

    ls.print_cr("Process Memory Info:");
    {
      StreamIndentor si(&ls, 4);
      os::print_process_memory_info(&ls);
      ls.cr();
    }

    ls.print_cr("Native Memory Tracking:");
    {
      StreamIndentor si(&ls, 4);
      if (MemTracker::enabled()) {
        MemTracker::report(true, &ls, K);
      } else {
        ls.print("Not enabled");
      }
      ls.cr();
    }

    ls.print_cr("Compilation Memory History:");
    {
      StreamIndentor si(&ls, 4);
      CompilationMemoryStatistic::print_brief_report(&ls);
      ls.cr();
    }

    ls.print_cr("Memory Map:");
    {
      StreamIndentor si(&ls, 4);
      if (!MemTracker::enabled()) {
        ls.print_cr("(NMT is disabled, will not annotate mappings).");
      }
      MemMapPrinter::print_all_mappings(&ls);
      ls.cr();
    }
  }

  bool uses_relative_limit() const {
    return _percent_limit > 0.0;
  }

  void update_limit_thresholds(double t_now) {
    const size_t old_threshold_100 = _threshold_100;
    if (uses_relative_limit()) {
      _threshold_100 = percent_of_physical(_percent_limit);
    } else {
      _threshold_100 = _absolute_limit;
    }
    // Warning threshold is 80% of that
    _threshold_80 = (size_t)((double) _threshold_100 * 0.8);
    // Both thresholds are page-aligned
    _threshold_100 = align_down(_threshold_100, os::vm_page_size());
    _threshold_80 = align_down(_threshold_80, os::vm_page_size());
    if (old_threshold_100 != _threshold_100) { // limit changed?
      log_info(os, rss)("Recalculated rss limit threshold (%zu bytes)", _threshold_100);
    }
    _last_limit_recalculation_time = t_now;
  }

  void tick() {
    const size_t rss_now = os::rss();
    const double t_now = now();
    _ticks++;

    LogTarget(Trace,  os, rss) lt;
    if (lt.is_enabled()) {
      LogStream ls(lt);
      os::print_elapsed_time(&ls, t_now);
      ls.print(": %zu", rss_now);
    }

    // For RssLimit, calculate threshold once; for RssLimitPercent, recalc thresholds at periodic intervals
    if (uses_relative_limit() && _last_limit_recalculation_time < (t_now - limit_min_seconds_between_recalculating)) {
      update_limit_thresholds(t_now);
    }

    // Update history
    if (_ticks == 1 ||
        _last_history_add_time < (t_now - history_min_seconds_elapsed_between_samples)) {
      add_history(rss_now, t_now);
    }

    // check limits
    if (_time_threshold_100 == 0.0 && rss_now > _threshold_100) {
      _time_threshold_100 = t_now;
      if (_time_threshold_80 == 0.0) { // fast spike?
        _time_threshold_80 = t_now;
      }

      char tmp[256];
      os::snprintf(tmp, sizeof(tmp), "*** Error: rss (%zu) over limit threshold (%zu) ***", rss_now, _threshold_100);
      log_report(tmp, rss_now, t_now);

      // Optionally abort VM
      if (_is_fatal) {
        fatal("%s", tmp);
      }

      // No need to continue measuring. This also preserves the RSSLimit section
      // in VM.info and hs-err to show the state when 100% was reached (e.g. let
      // history end here)
      disenroll();

    } else if (_time_threshold_80 == 0.0 && rss_now > _threshold_80) {
      _time_threshold_80 = t_now;
      char tmp[256];
      os::snprintf(tmp, sizeof(tmp), "*** Warning: rss (%zu) over 80%% of limit threshold (%zu) ***", rss_now, _threshold_100);
      log_report(tmp, rss_now, t_now);
    }
  }

public:

  RssLimitTask(size_t absolute_limit, double percent_limit, bool is_fatal)
    : PeriodicTask(RssLimitCheckInterval),
      _threshold_100(0), _time_threshold_100(0.0),
      _threshold_80(0), _time_threshold_80(0.0),
      _absolute_limit(absolute_limit),
      _percent_limit(percent_limit),
      _last_limit_recalculation_time(0.0),
      _is_fatal(is_fatal),
      _ticks(0),
      _last_history_add_time(0.0),
      _history_pos(0)
  {
    memset(_history, 0, sizeof(_history));
    assert((_absolute_limit == 0) != (_percent_limit == 0.0),
           "Either one of RSSLimit or RSSLimitPercent must be set");
    update_limit_thresholds(now());
  }

  void print_state(outputStream* st) const {
    st->print_cr("RssLimit:                    %s", RssLimit != nullptr ? RssLimit : "not set");
    st->print_cr("RssLimitPercent:             %s", RssLimitPercent != nullptr ? RssLimitPercent : "not set");
    st->print_cr("RssLimitCheckInterval:       %ums", RssLimitCheckInterval);
    st->print_cr("physical memory:             %zu", os::physical_memory());
    st->print_cr("abs limit:                   %zu", _absolute_limit);
    st->print_cr("rel limit percent:           %.3f", _percent_limit);
    st->print_cr("limit threshold:             %zu", _threshold_100);
    st->print   ("limit threshold reached:     ");
    if (_time_threshold_100 > 0.0) {
      st->print("after ");
      os::print_elapsed_time(st, _time_threshold_100);
    } else {
      st->print("no");
    }
    st->cr();
    st->print_cr("warning threshold:           %zu", _threshold_80);
    st->print   ("warning threshold reached:   ");
    if (_time_threshold_80 > 0.0) {
      st->print("after ");
      os::print_elapsed_time(st, _time_threshold_80);
    } else {
      st->print("no");
    }
    st->cr();
    st->print_cr("threshold aborts VM:         %s", _is_fatal ? "yes" : "no");
    st->print_cr("ticks:                       " UINT64_FORMAT, _ticks);
    st->cr();
  }

  // called from VMError::report or from VM.info
  void print_on_error_report(outputStream* st) const {
    st->print_cr("Settings:");
    {
      StreamIndentor si(st, 4);
      print_state(st);
    }

    st->print_cr("History:");
    {
      StreamIndentor si(st, 4);
      print_history(st);
    }
  }

  void task() override {
    tick();
  }
};

static RssLimitTask* _rss_limit_task = nullptr;

// Helper for parsing RssLimit/RssLimitPercent
// Scans flags (we only have one atm)
static void scan_flags(const char* s, bool* is_fatal) {
  assert(s != nullptr, "sanity");
  if (strcmp(s, "fatal") == 0) {
    (*is_fatal) = true;
    return;
  }
  vm_exit_during_initialization("RssLimit/RssLimitPercent: invalid flag");
}

void RssWatcher::initialize() {

  if (RssLimit == nullptr && RssLimitPercent == nullptr) {
    return;
  } else if (RssLimit != nullptr && RssLimitPercent != nullptr) {
    vm_exit_during_initialization("Please specify either RssLimit or RssLimitPercent, but not both");
  }

  // Sanity-check the interval given. We use PeriodicTask, and that has some limitations:
  // - minimum task time
  // - task time aligned to (non-power-of-2) alignment.
  // For convenience, we just adjust the interval.
  unsigned interval = RssLimitCheckInterval;
  interval /= PeriodicTask::interval_gran;
  interval *= PeriodicTask::interval_gran;
  interval = MAX2(interval, (unsigned)PeriodicTask::min_interval);
  if (interval != RssLimitCheckInterval) {
    log_warning(os, rss)("RssLimit interval has been adjusted to %ums", interval);
  }

  const size_t rss_now = os::rss();
  const double t_now = now();

  if (rss_now == 0) {
    // Not all OSes implement os::rss (AIX is missing)
    log_warning(os, rss)("RssLimit not supported.");
    return;
  }

  size_t absolute_limit = 0;
  double percent_limit = 0.0f;
  bool is_fatal = false;

  // Parse RssLimit or RssLimitPercent
  char* end;
  if (RssLimit != nullptr) {
    // RssLimit is an absolute memory size
    if (!parse_integer<size_t>(RssLimit, &end, &absolute_limit)) {
      vm_exit_during_initialization("RssLimit: invalid value");
    }
    if (absolute_limit < M) {
      vm_exit_during_initialization("RssLimit: too low");
    }
  } else {
    // RssLimitPercent is parsed as a float because we need to allow for
    // fractions of percent on machines with very large physical memory.
    assert(RssLimitPercent != nullptr, "Sanity");
    percent_limit = strtod(RssLimitPercent, &end);
    if (errno == ERANGE || percent_limit == 0.0) {
      vm_exit_during_initialization("RssLimitPercent: invalid number format");
    }
    if (end[0] == '.' || end[0] == ',') {
      // possible localization issue
      vm_exit_during_initialization("RssLimitPercent: invalid number format");
    }
    if (percent_limit > 100.0) {
      vm_exit_during_initialization("RssLimitPercent: too high");
    }
    if (percent_of_physical(percent_limit) < M) {
      vm_exit_during_initialization("RssLimitPercent: too low");
    }
  }
  if (end[0] != '\0') {
    if (end[0] == ':') {
      scan_flags(end + 1, &is_fatal);
    } else {
      vm_exit_during_initialization("RssLimit/RssLimitPercent: invalid flag format");
    }
  }

  // Start watcher task
  _rss_limit_task = new RssLimitTask(absolute_limit, percent_limit, is_fatal);
  _rss_limit_task->enroll();

  LogTarget(Info,  os, rss) lt;
  if (lt.is_enabled()) {
    LogStream ls(lt);
    ls.print_cr("RssLimit watcher enabled (current rss: %zu)", rss_now);
    {
      StreamIndentor si(&ls, 4);
      _rss_limit_task->print_state(&ls);
    }
  }
}

void RssWatcher::print_state(outputStream* st) {
  st->print("RssWatcher state: ");
  if (_rss_limit_task != nullptr) {
    StreamIndentor si(st, 4);
    st->cr();
    _rss_limit_task->print_state(st);
  } else {
    st->print_cr("Not enabled");
  }
}
