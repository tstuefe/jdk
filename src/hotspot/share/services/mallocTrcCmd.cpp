
#include "precompiled.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"

#include <malloc.h>
#include <services/mallocTrcCmd.hpp>

#ifdef LINUX
static pthread_mutex_t g_malloc_hook_lock = PTHREAD_MUTEX_INITIALIZER;

class Locker {
  bool _locked;
public:
  Locker() : _locked(false) {
    if (::pthread_mutex_lock(&g_malloc_hook_lock) == 0) {
      _locked = true;
    } else {
      ::printf("lock failed\n");
    }
  }
  ~Locker() {
    if (_locked) {
      ::pthread_mutex_unlock(&g_malloc_hook_lock);
    }
  }
};

class MallocStack {
  static const int num_frames = 8;
  address _v[num_frames];
public:

  MallocStack() {
    ::memset(_v, 0, sizeof(_v));
    os::get_native_stack(_v, num_frames, 0);
  }

  unsigned int calculate_hash() const {
    uintptr_t hash = 0;
    for (int i = 0; i < num_frames; i++) {
      hash += (uintptr_t)_v[i];
    }
    // Never return 0
    return hash + 1;
  }

  void copy_from(const MallocStack* other) {
    ::memcpy(_v, other->_v, sizeof(_v));
  }

  bool equals(const MallocStack* other) const {
    return ::memcmp(_v, other->_v, sizeof(_v)) == 0;
  }

  void print_on(outputStream* st) const {
    char tmp[256];
    for (int i = 0; i < num_frames && _v[i] != NULL; i++) {
      st->print("[" PTR_FORMAT "] ", p2i(_v[i]));
      os::print_function_and_library_name(st, _v[i], tmp, sizeof(tmp), true, true, false);
      st->cr();
    }
  }
};


struct site_t {
  MallocStack stack;
  uint64_t invocations;
  uint64_t invocations_delta; // delta since last printing
};

static int compare_sites(const void* s1, const void* s2) {
  return ((const site_t*)s2)->invocations -
         ((const site_t*)s1)->invocations;
}

class Sites {
  static const int max_size = 4096;
  // Open addressing hashtable
  static const int max_slots = 1.5 * max_size; // max 2/3 full
  // We keep hashes separately for faster walking
  unsigned _hashes[max_slots];
  site_t _sites[max_slots];

  // Number of elements
  int _size;

  // Stats
  // Number of lost adds due to table max reached
  int _lost;
  // Number of hash collisions
  int _collisions;
  // Number of too-similar hashes
  int _similars;

  // For printing (avoid dyn. allocation)
  site_t _sorted_sites[max_slots];

#ifdef ASSERT
  void verify() const {
    int num_found = 0;
    for (int i = 0; i < max_slots; i ++) {
      if (_hashes[i] != 0) {
        num_found ++;
        assert(_sites[i].stack.calculate_hash() == _hashes[i], "sanity");
        assert(_sites[i].invocations > 0, "sanity");
        assert(_sites[i].invocations >= _sites[i].invocations_delta, "sanity");
      } else {
        assert(_sites[i].invocations == 0, "sanity");
      }
    }
    assert(num_found <= max_size, "sanity");
    assert(num_found == _size, "sanity");
  }
#endif

public:

  Sites() : _size(0), _lost(0) {
    ::memset(_hashes, 0, sizeof(_hashes));
    ::memset(_sites, 0, sizeof(_sites));
  }

  void add_site(const MallocStack* stack, size_t allocsize) {
    // Find slot in closed hash table
    const unsigned hash = stack->calculate_hash();
    assert(hash != 0, "sanity");
    unsigned slot = hash % max_slots;
    DEBUG_ONLY(const unsigned first_slot = slot;)
    for(;;) {
      if (_hashes[slot] == hash) {
        if (_sites[slot].stack.equals(stack)) {
          // found
          _sites[slot].invocations ++;
          _sites[slot].invocations_delta ++;
          return;
        } else {
          _collisions ++;
        }
      } else if (_hashes[slot] == 0) {
        // This is a new stack. Add it if we still have space.
        if (_size < max_size) {
          _size ++;
        } else {
          _lost ++;
          return;
        }
        _hashes[slot] = hash;
        _sites[slot].stack.copy_from(stack);
        _sites[slot].invocations = _sites[slot].invocations_delta = 1;
        return;
      } else {
        _similars ++;
      }
      slot ++; slot %= max_slots;
      assert(first_slot != slot, "too many elements?");
    }
  }

  void print_all(outputStream* st) {
    DEBUG_ONLY(verify();)
    // Very primitive. We just sort the whole array and print all non-null stacks
    {
      Locker lck;
      ::memcpy(_sorted_sites, _sites, sizeof(_sites));
    }
    ::qsort(_sorted_sites, max_slots, sizeof(site_t), compare_sites);
    int rank = 0;
    for (int i = 0; i < max_slots; i ++) {
      if (_sorted_sites[i].invocations > 0) {
        st->print_cr("---- %d ----", rank);
        st->print_cr("Invocs: " UINT64_FORMAT " (+" UINT64_FORMAT ")",
            _sorted_sites[i].invocations, _sorted_sites[i].invocations_delta);
        _sorted_sites[i].stack.print_on(st);
        rank ++;
      }
    }
    // Reset delta values after printing.
    for (int i = 0; i < max_slots; i ++) {
      _sites[i].invocations_delta = 0;
    }
    // some stats
    st->print_cr("Table size: %d, lost: %d, collisions: %d, similars: %d", _size, _lost, _collisions, _similars);
  }

};

static Sites g_sites;

PRAGMA_DISABLE_GCC_WARNING("-Wdeprecated-declarations")
typedef void* (*malloc_hook_fun_t) (size_t len, const void* caller);

static malloc_hook_fun_t swap_malloc_hook(malloc_hook_fun_t function) {
  malloc_hook_fun_t old = __malloc_hook;
  __malloc_hook = function;
 // ::printf("old: " PTR_FORMAT ", new: " PTR_FORMAT "\n", p2i((void*)old), p2i((void*)function));
  return old;
}

static malloc_hook_fun_t g_original_hook = NULL;
static uint64_t g_total_malloc_calls = 0;

// https://www.gnu.org/software/libc/manual/html_node/Hooks-for-Malloc.html
static void* my_malloc_hook(size_t size, const void *caller) {
  g_total_malloc_calls ++;
  Locker lck;
  MallocStack stack;
  g_sites.add_site(&stack, size);
  swap_malloc_hook(g_original_hook);
//  ::printf("malloc called (" SIZE_FORMAT ") from " PTR_FORMAT ".\n", size, p2i(caller));
  void* p = ::malloc(size);
  swap_malloc_hook((malloc_hook_fun_t)my_malloc_hook);
  return p;
}

#endif

MallocTrcCmd::MallocTrcCmd(outputStream* output, bool heap) :
  DCmdWithParser(output, heap),
  _what("what","what", "STRING", true)
{
  _dcmdparser.add_dcmd_argument(&_what);
}

void MallocTrcCmd::execute(DCmdSource source, TRAPS) {
  if (::strcmp(_what.value(), "on") == 0) {
    g_original_hook = swap_malloc_hook((malloc_hook_fun_t)my_malloc_hook);
  } else if (::strcmp(_what.value(), "print") == 0) {
    g_sites.print_all(_output);
    _output->print_cr("Total hook invocations: " UINT64_FORMAT ".", g_total_malloc_calls);
  } else {
    _output->print_cr("unknown sub command %s", _what.value());
  }
}
