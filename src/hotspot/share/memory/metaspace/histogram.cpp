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
#include "memory/metaspace/histogram.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/ostream.hpp"

namespace metaspace {

void Histogram::print_on(outputStream* st) const {
  st->print_cr("word size,number");
  for (unsigned interval = 0; interval < num_intervals; interval++) {
    st->print_cr(SIZE_FORMAT ",%u", (interval + 1) * interval_words, _counters[interval].get());
  }
  st->print_cr(SIZE_FORMAT " and larger,%u", (num_intervals - 1) * interval_words, _counters[num_intervals - 1].get());
  st->print_cr("peak word size: " SIZE_FORMAT, _peak_word_size);
}

static Histogram g_histogram_class;
static Histogram g_histogram_nonclass;

Histogram* Histogram::histogram_class() { return &g_histogram_class; }
Histogram* Histogram::histogram_nonclass() { return &g_histogram_nonclass; }

} // namespace metaspace
