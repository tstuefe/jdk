/*
 * Copyright (c) 2026 IBM Corporation. All rights reserved.
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

#ifndef SHARE_MEMORY_ARENA_CHUNK_VECTOR_HPP
#define SHARE_MEMORY_ARENA_CHUNK_VECTOR_HPP

#include "memory/allocation.hpp"
#include "memory/arena.hpp"
#include "nmt/memTag.hpp"
#include "runtime/os.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/ostream.hpp"

template <class T_Extra>
class ArenaChunkVector : public CHeapObj<mtChunkMisc> {
public:
  struct Item {
    Chunk* chunk;
    T_Extra extra_info;
  };

private:
  int _capacity;
  int _index_hot;   // one-past last written hot element
  int _index_cold;  // index of coldest (oldest) written element
  int _num_chunks;
  Item* _items;

  int before(int index) const { return (index == 0 ? _capacity : index) - 1; }
  int after(int index) const { return (index == _capacity - 1) ? 0 : index + 1; }
  bool is_valid_index(int index) const;

  void grow();

public:

  explicit ArenaChunkVector(int initial_capacity);
  NONCOPYABLE(ArenaChunkVector);

  ~ArenaChunkVector() {
    FREE_C_HEAP_ARRAY(_items);
  }

  int capacity() const { return _capacity; }
  int num() const { return _num_chunks; }

  void push_hot(Chunk* chunk, T_Extra extra_info);
  Chunk* pop_hot();

  // iterate, coldest first
  template <typename IteratorFunction>
  void iterate_coldest_first(IteratorFunction iterator) const;

  // Conditional bulk-remove a number of the coldest items.
  // Will iterate coldest first and call the provided remover
  // function until remover function returns false.
  // Remover is bool f(Item*).
  template <typename RemoverFunction>
  void bulk_remove_coldest_first(RemoverFunction remover_function);

#ifdef ASSERT
  // Verify vector.
  // Verifier is supposed to assert on error.
  template <typename VerifyFunction>
  void verify_coldest_first(VerifyFunction verifier_function, bool paranoid) const;
#endif

  template <typename ExtraPrinter>
  void print_on(outputStream* st, ExtraPrinter extra_printer) const;
  void print_on(outputStream* st) const;
};

template <class T_Extra>
ArenaChunkVector<T_Extra>::ArenaChunkVector(int initial_capacity) :
  _capacity(initial_capacity), _index_hot(0), _index_cold(0), _num_chunks(0) {
  assert(initial_capacity > 0, "Sanity");
  _items = NEW_C_HEAP_ARRAY(Item, _capacity, mtChunkMisc);
  for (int i = 0; i < _capacity; i++) {
    _items[i].chunk = nullptr;
  }
}

template <class T_Extra>
bool ArenaChunkVector<T_Extra>::is_valid_index(int index) const {
  assert(index >= 0 && index < _capacity, "Sanity");
  if (_index_cold == _index_hot) {
    return _num_chunks > 0;
  }
  if (_index_cold < _index_hot) {
    return index >= _index_cold && index < _index_hot;
  }
  return index < _index_hot || index >= _index_cold;
}

// iterate, coldest first
template <class T_Extra>
template <typename IteratorFunction>
void ArenaChunkVector<T_Extra>::iterate_coldest_first(IteratorFunction iterator) const {
  if (_num_chunks == 0) return;
  int index = _index_cold;
  do {
    iterator(_items + index);
    index = after(index);
  } while (index != _index_hot);
}

template <class T_Extra>
void ArenaChunkVector<T_Extra>::push_hot(Chunk* chunk, T_Extra extra_info) {
  if (_num_chunks == _capacity) {
    grow();
    assert(_num_chunks < _capacity, "Sanity");
    assert(_index_hot < _capacity, "Sanity");
  }
  assert(_items[_index_hot].chunk == nullptr, "Sanity");
  _items[_index_hot].chunk = chunk;
  _items[_index_hot].extra_info = extra_info;
  _index_hot = after(_index_hot);
  _num_chunks++;
}

template <class T_Extra>
Chunk* ArenaChunkVector<T_Extra>::pop_hot() {
  if (_num_chunks == 0) {
    return nullptr;
  }
  _index_hot = before(_index_hot);
  Chunk* const chunk = _items[_index_hot].chunk;
  assert(chunk != nullptr, "Sanity");
  _items[_index_hot].chunk = nullptr;
  _num_chunks--;
  return chunk;
}

// Conditional bulk-remove a number of the coldest items.
// Will iterate coldest first and call the provided remover
// function. Iteration stops if remover function returns false.
// Remover is bool f(Item*)
template <class T_Extra>
template <typename RemoverFunction>
void ArenaChunkVector<T_Extra>::bulk_remove_coldest_first(RemoverFunction remover_function) {
  if (_num_chunks == 0) {
    return;
  }
  do {
    Item* const item = _items + _index_cold;
    assert(item->chunk != nullptr, "Sanity");
    if (!remover_function(item)) {
      break;
    }
    item->chunk = nullptr;
    _num_chunks--;
    _index_cold = after(_index_cold);
  } while (_index_cold != _index_hot);
}

template <class T_Extra>
void ArenaChunkVector<T_Extra>::grow() {
  assert(_num_chunks == _capacity, "Sanity");
  assert(_index_cold == _index_hot, "Sanity");
  assert(_capacity <= (INT_MAX / 2), "Sanity");
  // TODO cache coloring?
  const int new_capacity = _capacity * 2;
  _items = REALLOC_C_HEAP_ARRAY(_items, new_capacity, mtChunkMisc);
  // possibly relocate wrapped hottest elements; wipe new still
  // unused areas
  assert(_items[_capacity - 1].chunk != nullptr, "Sanity");
  int new_index = _capacity;
  for (int index = 0; index < _index_hot; index++) {
    _items[new_index] = _items[index];
    _items[index].chunk = nullptr;
    new_index++;
  }
  _index_hot = new_index;
  memset(_items + _index_hot, 0, sizeof(Item) * (new_capacity - _index_hot));
  _capacity = new_capacity;
}

#ifdef ASSERT
#define ASSERT_HERE(cond) \
  do { if (!(cond)) { \
    this->print_on(tty); \
    assert((cond), "Computer says no"); \
  } } while(0)

template <class T_Extra>
template <typename VerifyFunction>
void ArenaChunkVector<T_Extra>::verify_coldest_first(VerifyFunction verifier_function, bool paranoid) const {
  ASSERT_HERE(_num_chunks <= _capacity);
  ASSERT_HERE(_index_cold < _capacity);
  ASSERT_HERE(_index_hot < _capacity);

  int n = 0;
  for (int index = 0; index < _capacity; index++) {
    if (is_valid_index(index)) {
      n++;
      ASSERT_HERE(_items[index].chunk != nullptr);
    } else {
      ASSERT_HERE(_items[index].chunk == nullptr);
    }
  }
  ASSERT_HERE(n == _num_chunks);

  if (_num_chunks == 0 || _num_chunks == _capacity) {
    ASSERT_HERE(_index_hot == _index_cold);
    if (_num_chunks == 0) {
      return;
    }
  }

  iterate_coldest_first(verifier_function);

  if (paranoid) {
    // Search for duplicates (O(n^2))
    bool duplicate = false;
    auto outer_fun = [&duplicate,this](const Item* outer_item) {
      auto inner_fun = [&duplicate,outer_item](const Item* inner_item) {
        if (!duplicate &&
            (outer_item != inner_item) &&
            (outer_item->chunk == inner_item->chunk)) {
          duplicate = true;
        }
      };
      iterate_coldest_first(inner_fun);
    };
    iterate_coldest_first(outer_fun);
    ASSERT_HERE(duplicate == false);
  }

}
#undef ASSERT_HERE
#endif

template <class T_Extra>
void ArenaChunkVector<T_Extra>::print_on(outputStream* st) const {
  // just print as hex dump
  auto extra_printer = [](outputStream* st2, const T_Extra* extra) {
    const const_address p = (const_address)extra;
    for (size_t i = 0; i < sizeof(T_Extra); i++) {
      st2->print("%02x ", p[i]);
    }
  };
  print_on(st, extra_printer);
}

template <class T_Extra>
template <typename ExtraPrinter>
void ArenaChunkVector<T_Extra>::print_on(outputStream* st, ExtraPrinter extra_printer) const {
  st->print_cr("ArenaChunkVector<> items " PTR_FORMAT ", capacity %d, num_chunks %d, coldest %d, hottest %d",
               p2u(_items), _capacity, _num_chunks, _index_cold, _index_hot);
  if (_index_cold < 0 || _index_hot < 0 ||
      _index_cold >= _capacity || _index_hot >= _capacity) {
    return;
  }
  auto printer = [st, &extra_printer](const Item* item) {
    st->print("@" PTR_FORMAT ": Chunk=" PTR_FORMAT " (sized %zu), extra=",
              p2u(item), p2u(item->chunk),
              (item->chunk != nullptr) ? item->chunk->length() : 0);
    extra_printer(st, &(item->extra_info));
    st->cr();
  };
  iterate_coldest_first(printer);
}


#endif // SHARE_MEMORY_ARENA_CHUNK_VECTOR_HPP
