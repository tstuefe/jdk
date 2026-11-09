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

#include "logging/log.hpp"
#include "memory/arena/arenaMemoryManager.hpp"
#include "memory/arena/arenaTick.hpp"
#include "runtime/os.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/macros.hpp"
#include "utilities/ostream.hpp"

ArenaTickThread::ArenaTickThread(Tickable* target, unsigned seconds_per_tick) :
  _target(target),
  _seconds_per_tick(seconds_per_tick),
  _ticks(0),
  _stop(false)
{
}

void ArenaTickThread::start() {
  set_name("Arena Tick Thread");
  if (os::create_thread(this, os::vm_thread)) {
    os::start_thread(this);
    log_info(arena)("Ticker thread started, interval %u seconds per tick", _seconds_per_tick);
  }
}

void ArenaTickThread::stop() {
  _stop = true;
}

void ArenaTickThread::run() {
  while (!_stop) {
    ::sleep(_seconds_per_tick);
    _target->tick(_ticks);
    _ticks++;
  }
}