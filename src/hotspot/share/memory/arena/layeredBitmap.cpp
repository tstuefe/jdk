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


#include "memory/arena/arenaHeap.hpp"
#include "memory/arena/layeredBitmap.hpp"
#include "runtime/os.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"


LayeredBitmap::LayeredBitmap(ArenaMetadataHeap* heap, unsigned num_bits) :
  _data([heap, num_bits]() {
    const size_t words_needed_for_summary = BitMap::calc_size_in_words(num_segments(num_bits));
    const size_t words_needed_for_master = BitMap::calc_size_in_words(num_bits);
    const size_t total_words = words_needed_for_summary * 2 + words_needed_for_master;
    // full (offset 0) | nonempty | master
    Data data;
    data._num_bits = num_bits;
    data._offset_nonempty_summary = words_needed_for_summary;
    data._offset_master_bitmap = words_needed_for_summary * 2;
    // Bitmap lives in its own cache lines
    heap->align_hwm_to_padded_size();
    data._storage = (WordType*)heap->allocate(total_words * BytesPerWord);
    heap->align_hwm_to_padded_size();
    return data;
  }
  () )
{
  assert(num_bits <= _max_num_bits, "Sanity");
  assert(num_bits >= 1, "Sanity");
  STATIC_ASSERT(BitsPerWord == _num_bits_per_segment);
}
