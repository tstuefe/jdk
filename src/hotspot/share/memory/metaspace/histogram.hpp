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

#ifndef SHARE_MEMORY_METASPACE_HISTOGRAM_HPP
#define SHARE_MEMORY_METASPACE_HISTOGRAM_HPP

#include "memory/metaspace/counters.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"

class outputStream;

namespace metaspace {

class Histogram {
  static const size_t interval_words = 8;
  static const unsigned num_intervals = 512;
  IntAtomicCounter _counters[num_intervals];
  size_t _peak_word_size;
  static unsigned get_interval_for_word_size(size_t word_size) {
    return MIN2(num_intervals - 1, (unsigned)(word_size / interval_words));
  }
public:
  Histogram() : _peak_word_size(0) {}
  void register_word_size(size_t word_size) {
    _counters[get_interval_for_word_size(word_size)].increment();
    _peak_word_size = MAX2(_peak_word_size, word_size);
  }
  void print_on(outputStream* st) const;
  static Histogram* histogram_class();
  static Histogram* histogram_nonclass();
};

} // namespace metaspace

#endif // SHARE_MEMORY_METASPACE_METACHUNK_HPP
