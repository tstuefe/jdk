/*
 * Copyright (c) 2025, Oracle and/or its affiliates. All rights reserved.
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


#include "gc/shared/collectedHeap.hpp"
#include "gc/shared/myHeapTracer.hpp"
#include "gc/shared/strongRootsScope.hpp"
#include "memory/iterator.inline.hpp"
#include "memory/universe.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/safepoint.hpp"
#include "utilities/stack.inline.hpp"

typedef Stack<oopDesc*, mtInternal> MyMarkingStack;


void MyHeapTracer::traceHeap(BasicOopIterateClosure* cl) {
  assert(SafepointSynchronize::is_at_safepoint(), "not at safepoint"); // Todo: Is this safe for concurrently evacuating GCs?
  Universe::heap()->ensure_parsability(false); // Is this even needed for heap tracing? What does this do:
                                               // - if true, retire tlabs, but we don't need this here, since we don't do linear iteration
                                               // - calls BarrierSet::make_parseable
                                               // Is this sufficient for all collectors? What about eg. Shenandoah retireLabs, which seems an
                                               // expanded version of ensure_parsability?

  if (UseZGC) {

  }

  {
    StrongRootsScope srs(0);

    MarkingNMethodClosure mark_code_closure(&follow_root_closure,
                                            !NMethodToOopClosure::FixRelocations,
                                            true);

    // Start tracing from roots, there are 3 kinds of roots in full-gc.
    //
    // 1. CLD. This method internally takes care of whether class loading is
    // enabled or not, applying the closure to both strong and weak or only
    // strong CLDs.
    ClassLoaderDataGraph::always_strong_cld_do(&follow_cld_closure);

    // 2. Threads stack frames and active nmethods in them.
    Threads::oops_do(&follow_root_closure, &mark_code_closure);

    // 3. VM internal roots.
    OopStorageSet::strong_oops_do(&follow_root_closure);
  }

}

void MyHeapTracer::walkHeap(BasicOopIterateClosure* cl) {
  assert(SafepointSynchronize::is_at_safepoint(), "not at safepoint"); // Todo: Is this safe for concurrently evacuating GCs?
  Universe::heap()->ensure_parsability(true);

}
