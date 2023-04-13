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

#ifndef SHARE_MEMORY_METASPACE_DLLIST_HPP
#define SHARE_MEMORY_METASPACE_DLLIST_HPP

#include "memory/metaspace/counters.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/debug.hpp"

namespace metaspace {

template <class T>
class DlList {
  NONCOPYABLE(DlList<T>);

  T* _front;
  T* _back;
  IntCounter _num;

protected:

  static void cap_prev(T* p) { p->set_prev(nullptr); }
  static void cap_next(T* p) { p->set_next(nullptr); }
  static void cap(T* p) {
    cap_prev(p); cap_next(p);
  }
  static bool is_prev_capped(const T* p)  { return p->prev() == nullptr; }
  static bool is_next_capped(const T* p)  { return p->next() == nullptr; }
  static bool is_capped(const T* p)       { return is_prev_capped(p) || is_next_capped(p); }

  void assert_isolated_node(const T* p) {
    assert(p != nullptr, "null node?");
    assert(is_capped(p), "uncapped?");
  }
  void assert_isolated_chain(const T* p1, const T* p2, int num) {
    assert(p1!= nullptr,  "null front node?");
    assert(p2 != nullptr, "null back node?");
    assert(is_prev_capped(p1), "front node uncapped?");
    assert(is_next_capped(p2), "back node uncapped?");
    assert(num > 0, "count?");
  }

  static void connect(T* a, T* b) {
    b->set_prev(a);
    a->set_next(b);
  }

  // Set chain to one element
  void set(T* p) {
    _front = _back = p;
    cap(p);
    _num.set(1);
  }

  // Set chain to a given chain (given both ends)
  void set_chain(T* p1, T* p2, int num) {
    _front = p1;
    _back = p2;
    cap_prev(p1);
    cap_next(p2);
    _num.set(num);
  }

  void append_single(T* p) {
    assert_isolated_node(p);
    if (_back == nullptr) {
      set(p);
    } else {
      connect(_back, p);
      cap_next(p);
      _back = p;
      _num.increment();
    }
  }

  void prepend_single(T* p) {
    assert_isolated_node(p);
    if (_front == nullptr) {
      set(p);
    } else {
      connect(p, _front);
      cap_prev(p);
      _front = p;
      _num.increment();
    }
  }

  void append_chain(T* p1, T* p2, int num) {
    assert_isolated_chain(p1, p2, num);
    if (_back == nullptr) {
      set_chain(p1, p2, num);
    } else {
      connect(_back, p1);
      cap_next(p2);
      _back = p2;
      _num.increment_by(num);
    }
  }

  void prepend_chain(T* p1, T* p2, int num) {
    assert_isolated_chain(p1, p2, num);
    if (_front == nullptr) {
      set_chain(p1, p2, num);
    } else {
      connect(p2, _front);
      cap_prev(p1);
      _front = p1;
      _num.increment_by(num);
    }
  }

public:

  DlList<T>() : _front(nullptr), _back(nullptr) {}

  T* front()              { return _front; }
  const T* front() const  { return _front; }
  T* back()               { return _back; }
  const T* back() const   { return _back; }
  int count() const       { return _num.get(); }
  bool empty() const      { return count() == 0; }

  void reset() {
    _front = _back = nullptr;
    _num.set(0);
  }

  void push_front(T* p) {
    prepend_single(p);
  }

  void push_back(T* p) {
    append_single(p);
  }

  // Adds content of other list to front and empties other list.
  void add_list_at_front(DlList<T>& l) {
    if (!l.empty()) {
      prepend_chain(l.front(), l.back(), l.count());
      l.reset();
    }
  }

  // Adds content of other list to front and empties other list.
  void add_list_at_back(DlList<T>& l) {
    if (!l.empty()) {
      append_chain(l.front(), l.back(), l.count());
      l.reset();
    }
  }

  void remove(T* p) {
    assert(p != nullptr, "null?");
    assert(contains(p), "Not contained");
    T* p_next = p->next();
    T* p_prev = p->prev();
    if (p == _front) {
      _front = p_next;
    }
    if (p == _back) {
      _back = p_prev;
    }
    if (p_prev != nullptr) {
      p_prev->set_next(p_next);
    }
    if (p_next != nullptr) {
      p_next->set_prev(p_prev);
    }
    _num.decrement();
    cap(p);
  }

  T* pop_front() {
    T* p = _front;
    if (p != nullptr) {
      remove(p);
    }
    return p;
  }

  T* pop_back() {
    T* p = _back;
    if (p != nullptr) {
      remove(p);
    }
    return p;
  }

  // Functor in this form: void f(const T* p). Don't modify list while iterating.
  template <class Functor>
  void for_each(Functor f) const {
    for (const T* p = front(); p != nullptr; p = p->next()) {
      f(p);
    }
  }

  // Functor in this form: void f(T* p). Don't modify list while iterating.
  template <class Functor>
  void for_each(Functor f) {
    for (T* p = front(); p != nullptr; p = p->next()) {
      f(p);
    }
  }

  // Functor in this form: bool f(const T* p). Functor should return true to abort the loop,
  // false otherwise.
  // Return the pointer to the element at which the loop was aborted, or nullptr if the full list
  // was iterated.
  template <class Functor>
  const T* for_each_until(Functor f) const {
    for (const T* p = front(); p != nullptr; p = p->next()) {
      if (f(p)) {
        return p;
      }
    }
    return nullptr;
  }

  bool contains(const T* x) const {
    // Shortcut for front/back
    if ((is_next_capped(x) && x == back()) ||
        (is_prev_capped(x) && x == front())) {
      return true;
    }
    // search list.
    auto finder = [x](const T* p) { return p == x; };
    return for_each_until(finder) != nullptr;
  }

  // For convenience,
  // but any other object containing prev()/next()/set_prev()/set_next() shall work too
  struct Node {
    T* _prev;
    T* _next;
    Node() : _prev(nullptr), _next(nullptr) {}
    void set_prev(T* x) { _prev = x; }
    void set_next(T* x) { _next = x; }
    T* prev() { return _prev; }
    T* next() { return _next; }
    const T* prev() const { return _prev; }
    const T* next() const { return _next; }
  };

  DEBUG_ONLY(inline virtual void verify() const;)

};

} // namespace metaspace

#endif // SHARE_MEMORY_METASPACE_CHUNKLEVEL_HPP
