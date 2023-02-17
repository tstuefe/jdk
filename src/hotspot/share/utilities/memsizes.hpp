/*
 * Copyright (c) 2023 SAP SE. All rights reserved.
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

#ifndef SHARE_UTILITIES_MEMSIZES_HPP
#define SHARE_UTILITIES_MEMSIZES_HPP

#include "utilities/align.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"

class outputStream;

class MemorySize {
  size_t _s;

  size_t w2b(size_t words) { return words * BytesPerWord; }

public:
  MemorySize(size_t s)            : _s(s) {}
  MemorySize()                    : _s(0) {}

  size_t as_bytes() const { return _s; }

  size_t as_words_exact() const {
    assert(_s % BytesPerWord == 0, "not aligned to word size");
    return _s / BytesPerWord;
  }

  void set_bytes(size_t bytes) { _s = bytes; }
  void set_words(size_t words) { _s = w2b(words); }
  void add_bytes(size_t bytes) {
    assert(SIZE_MAX - _s >= bytes, "overflow");
    _s += bytes;
  }
  void add_words(size_t words) { add_bytes(w2b(words)); }

  bool operator<(const MemorySize& s) const       { return _s < s._s; }
  bool operator<=(const MemorySize& s) const      { return _s <= s._s; }
  bool operator==(const MemorySize& s) const      { return _s == s._s; }
  bool operator>=(const MemorySize& s) const      { return _s >= s._s; }
  bool operator>(const MemorySize& s) const       { return _s > s._s; }

  MemorySize operator+=(const MemorySize& other)  {
    add_bytes(other.as_bytes()); return *this;
  }

  void reset() { _s = 0; }

  DEBUG_ONLY(void verify() const;)
};

MemorySize operator+(const MemorySize& a, const MemorySize& b) {
  return MemorySize(a.as_bytes() + b.as_bytes());
}

class ResComUsed {
  MemorySize _r; // reserved (bytes)
  MemorySize _c; // committed (bytes)
  MemorySize _u; // used (bytes)

public:

  ResComUsed() :
    _r(0), _c(0), _u(0) {}
  ResComUsed(size_t reserved_bytes, size_t committed_bytes, size_t used_bytes) :
    _r(reserved_bytes), _c(committed_bytes), _u(used_bytes) {}

  void set_bytes(size_t reserved_bytes, size_t committed_bytes, size_t used_bytes) {
    _r.set_bytes(reserved_bytes); _c.set_bytes(committed_bytes); _u.set_bytes(used_bytes);
  }

  void add_bytes(size_t reserved_bytes, size_t committed_bytes, size_t used_bytes) {
    _r.add_bytes(reserved_bytes); _c.add_bytes(committed_bytes); _u.add_bytes(used_bytes);
  }

  void set_words(size_t reserved_words, size_t committed_words, size_t used_words) {
    _r.set_words(reserved_words); _c.set_words(committed_words); _u.set_words(used_words);
  }

  void add_words(size_t reserved_bytes, size_t committed_bytes, size_t used_bytes) {
    _r.add_words(reserved_bytes); _c.add_words(committed_bytes); _u.add_words(used_bytes);
  }

  size_t reserved_bytes() const   { return _r.as_bytes(); }
  size_t committed_bytes() const  { return _c.as_bytes(); }
  size_t used_bytes() const       { return _c.as_bytes(); }

  size_t reserved_words() const   { return _r.as_words_exact(); }
  size_t committed_words() const  { return _c.as_words_exact(); }
  size_t used_words() const       { return _u.as_words_exact(); }

  // For compatibility with existing stats classes; returns byte sizes
  //size_t reserved() const         { return reserved_bytes(); }
  // size_t committed() const        { return committed_bytes(); }
  //  size_t used() const             { return used_bytes(); }

  void reset() { _r.reset(); _c.reset(); _u.reset(); }

  const ResComUsed& operator+= (const ResComUsed& other)  {
    add_bytes(other.reserved_bytes(), other.committed_bytes(), other.used_bytes());
    return *this;
  }

  DEBUG_ONLY(void verify() const;)
};

ResComUsed operator+ (const ResComUsed& a, const ResComUsed& b) {
  return ResComUsed(a.reserved_bytes() + b.reserved_bytes(),
                    a.committed_bytes() + b.committed_bytes(),
                    a.used_bytes() + b.used_bytes());
}


#endif // SHARE_UTILITIES_MEMSIZES_HPP
