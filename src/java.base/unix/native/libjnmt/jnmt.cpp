#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <limits.h>
#include <errno.h>

#define EXPORTED extern "C" __attribute__((visibility("default")))

enum State {
    state_newborn = 0,  // before/during earliest initialization
    state_init_0 = 1,   // after init_0
    state_init_1  = 2   // after init_1
};

static State g_state = state_newborn;

#define FULLY_INITED ((int)g_state >= after_init_1)

static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;

struct CriticalSection {
    CriticalSection() {
        pthread_mutex_lock(&g_mutex);
    }
    ~CriticalSection() {
        pthread_mutex_unlock(&g_mutex);
    }
};

////////////////////////////////////////////////////////////////////////////////////

typedef void* (*malloc_t)(size_t size);
typedef void* (*realloc_t)(void* ptr, size_t size);
typedef void  (*free_t)(void* ptr);

typedef void* (*mmap_t) (void *addr, size_t length, int prot, int flags, int fd, off_t offset);
typedef int (*munmap_t) (void *addr, size_t length);

struct functions_t {
    malloc_t fun_malloc;
    realloc_t fun_realloc;
    free_t fun_free;
    mmap_t fun_mmap;
    munmap_t fun_munmap;
};
       
// Entries into libc
static functions_t g_libc_functions;

// Entries into libjvm
static functions_t g_libjvm_functions;

// Entries for the libjvm to call (recursion prevention)
static functions_t g_libjvm_callback_functions;

///////////////////////////////////////////////////////////////////////////

class StringStream {
    char _b[1024];
    int _pos;
public:
    StringStream() : _pos(0) { _b[0] = '\0'; }
    const char* buffer() const { return _b; }
    int len() const { return _pos; }
    void print(const char* fmt, ...) {
        va_list ap;
        va_start(ap, fmt);
        vprint(fmt, ap);
        va_end(ap);
    }

    void vprint(const char* fmt, va_list ap) {
        const size_t remaining = sizeof(_b) - _pos;
        if (remaining > 1) {
            const size_t printed = vsnprintf(_b + _pos, remaining, fmt, ap);
            if (printed > remaining) {
                _b[sizeof(_b) - 1] = '\0';
                _pos += remaining;
            } else {
                _pos += printed;
            }
        }
    }

    void cr() { print("\n"); }
};

/// Options

class Trace {

    const int _level;
    
#define TRCVARNAME "NMT_INTERPOSE_TRACE"
    
    static int level_from_env() {
        const char* s = getenv(TRCVARNAME);
        if (s != nullptr) {
            return atoi(s);
        }
        return 0;
    }

    void do_printvf(const char* fmt, va_list ap) const {
        StringStream ss;
        ss.print(" [%d] [%u] ", g_state, (unsigned) gettid());
        ss.vprint(fmt, ap);
        ss.cr();
        const ssize_t written = write(1, ss.buffer(), ss.len());
    }

    void do_printf(const char* fmt, ...) const {
        va_list ap;
        va_start(ap, fmt);
        do_printvf(fmt, ap);
        va_end(ap);
    }

public:

    Trace() : _level(level_from_env()) {
        if (_level > 0) {
            do_printf("NMT_INTERPOSE_TRACE" "=%d", _level);
        }
    }

#define DEFINE_TRACE_FUN(name, level)           \
    void name (const char* fmt, ...) const {    \
        if (level <= _level) {                  \
            va_list ap;                         \
            va_start(ap, fmt);                  \
            do_printvf(fmt, ap);                \
            va_end(ap);                         \
        }                                       \
    }

DEFINE_TRACE_FUN(error, 0)
DEFINE_TRACE_FUN(info, 1)
DEFINE_TRACE_FUN(debug, 2)
DEFINE_TRACE_FUN(trace, 3)

    // Helper calls to trace specific functions:
    void begin_mmap(const char* context, void *addr, size_t length, int prot, int flags, int fd, off_t offset) const {
        trace("%s mmap (addr=%p, size=%zu, prot=%d, flags=%d, fd=%d, offset=%zu)...",
                 context, addr, length, prot, flags, fd, offset);
    }

    void end_mmap(const char* context, void* rc, int _errno) const {
        trace("%s mmap => %p (%d)", context, rc, _errno);
    }

    void begin_munmap(const char* context, void *addr, size_t length) const {
        trace("%s munmap (addr=%p, size=%zu)...", context, addr);
    }

    void end_munmap(const char* context, int rc, int _errno) const {
        trace("%s munmap => %d (%d)", context, rc, _errno);
    }

};

static const Trace g_tracer;

////////////////////////////////////////////////////////////////////////////////////

// We need an own assertion mechanism since glibc assert uses C-heap :-(

#define ASSERT(condition)                               \
    if (!(condition)) {                                 \
        g_tracer.error("Assert %s:%u", __FILE__, __LINE__);    \
        abort();                                        \
    }

#define ASSERT_(condition, fmt, ...)                    \
    if (!(condition)) {                                 \
        g_tracer.error(fmt, __VA_ARGS__);               \
        g_tracer.error("Assert %s:%u", __FILE__, __LINE__);    \
        abort();                                        \
    }

////// Urbuffer /////////////////////////////////////////////////////////

#define align_up_16(num) (((num) + 15) & ~15)

class UrBuffer {
    static constexpr size_t SIZE = 10 * 1024 * 1024;
    uint8_t _d [SIZE];
    size_t _used;
public:
    uint8_t* alloc(size_t len) {
        len = align_up_16(len);
        const size_t remaining = SIZE - _used;
        if (remaining > len) {
            uint8_t* p = _d + _used;
            _used += len;
            return p;
        }
        return nullptr;
    }
    bool contains(void* p) const {
        return p >= _d && p < (_d + SIZE);
    }
};

static UrBuffer g_urbuffer;

////// Mallocheader monkey business /////////////////////////////////////

// For now lets use a hash map to store pointers that have been allocated
// via libjvm_malloc. That alleviates the need to monitor every possible API that
// allocated C heap. (is this needed? would these APIs all not just end up
// in malloc? Investigate!)
// Among other things, it removes the need to implement posix_memalign.
//
// Long term, we may want a malloc header based solution since using a hash map
// may not scale (needs synchronization, may work badly with many pointers)

class PointerMap {
    static constexpr int MAPSIZE = 1024 * 1024;
    static constexpr int SLABSIZE = 64;

    class Slab {
        const void* _pointers[SLABSIZE];
        Slab* _next;

        void* operator new(size_t s) {
            g_tracer.debug("New Slab");
            return g_libc_functions.fun_malloc(s);
        }

    public:
        Slab() : _next(nullptr) {
            memset(_pointers, 0, sizeof(_pointers));
        }

        static Slab* allocate_slab() {
            void* p = g_libc_functions.fun_malloc(sizeof(Slab));
            Slab* s = new Slab();
            return s;
        }

        bool remove(const void* p) {
            for (int i = 0; i < SLABSIZE; i++) {
                if (_pointers[i] == p) {
                    _pointers[i] = nullptr;
                    return true;
                }
            }
            return false;
        }

        bool add(const void* p) {
            int i = 0;
            while (i < SLABSIZE && _pointers[i] != nullptr) i++;
            if (i < SLABSIZE) {
                _pointers[i] = p;
                return true;
            }
            return false;
        }

        Slab* next_slab() const {
            return _next;
        }

        Slab* next_slab_or_add() {
            if (_next == nullptr) {
                _next = allocate_slab();
            }
            return _next;
        }
    };

    #define p2i(p)    ((uintptr_t)p)

    // Lowest bit in pointer indicates this is a Slab, not a raw malloc pointer
    class Ptr {
        uintptr_t _v;
        static constexpr uintptr_t mask = 1;
        // check a supposed malloced pointer
        #define CHKPTR(p) { ASSERT((p2i(p) & 7) == 0); }
    public:
        Ptr() : _v(0) {}
        Ptr(uintptr_t v) : _v(v) {}
        Ptr(const Ptr& v) : _v(v._v) {}
        Ptr& operator=(const Ptr& other)        { _v = other._v; return *this; }
        static Ptr from_slab(Slab* s)           { CHKPTR(s); return Ptr(p2i(s) | mask); }
        static Ptr from_pointer(const void* p)  { CHKPTR(p); return Ptr(p2i(p)); }
        bool is_null() const                    { return _v == 0; }
        bool is_slab() const                    { return (_v & 1) == 1; }
        bool is_pointer() const                 { return !is_slab(); }
        Slab* as_slab() const                   { ASSERT(is_slab()); return (Slab*)(_v & ~mask); }
        const void* as_pointer() const          { ASSERT(!is_slab()); return (const void*)_v; }
    };

    Ptr _map[MAPSIZE];
    size_t _count = 0;

    static unsigned calchash(const void* p) {
        return (uintptr_t)(p2i(p) >> 3);
    }

    static unsigned calcindex(const void* p) {
        return calchash(p) % MAPSIZE;
    }

    void inc_count() {
        _count ++;
        if ((_count % 1024) == 0) {
            print_details();
        }
    }

    void dec_count() {
        ASSERT(_count > 0);
    }

public:

    PointerMap() {}

    bool lookup_and_remove(const void* p) {
        const unsigned idx = calcindex(p);
        ASSERT(idx < MAPSIZE);
        const Ptr& e = _map[idx];
        if (e.is_null()) {
            return false;
        } else if (e.is_pointer()) {
            if (e.as_pointer() == p) {
                _map[idx] = Ptr(); // remove
                dec_count();
                return true;
            } else {
                return false;
            }
        } else {
            ASSERT(e.is_slab());
            for (Slab* slab = _map[idx].as_slab(); slab != nullptr; slab = slab->next_slab()) {
                if (slab->remove(p)) {
                    dec_count();
                    return true;
                }
            }
        }
        return false;
    }

    void add(const void* p) {
        const unsigned idx = calcindex(p);
        ASSERT(idx < MAPSIZE);
        Ptr& e = _map[idx];
        if (e.is_null()) {
            _map[idx] = Ptr::from_pointer(p);
        } else if (e.is_pointer()) {
            const void* p2 = e.as_pointer();
            ASSERT_(p2 != p, "Found %p in map", p);
            Slab* slab = Slab::allocate_slab();
            slab->add(p2);
            slab->add(p);
            _map[idx] = Ptr::from_slab(slab);
        } else {
            ASSERT(e.is_slab());
            for (Slab* slab = e.as_slab();
                 slab != nullptr && !slab->add(p);
                 slab = slab->next_slab_or_add());
        }
        inc_count();
    }

    void print_details() const {
        size_t numslabs = 0;
        for (int i = 0; i < MAPSIZE; i++) {
            const Ptr& e = _map[i];
            if (e.is_slab()) {
                for (Slab* slab = e.as_slab();
                     slab != nullptr;
                     slab = slab->next_slab()) {
                        numslabs++;
                     }
            }

        }
        const size_t sz = sizeof(PointerMap) + numslabs * sizeof(Slab);
        g_tracer.debug("  %zu pointers, %zu slabs, total size %zu", _count, numslabs, sz);
    }

}; // PointerMap

static PointerMap g_pointermap;

////////////////////////////////////////////////////////////////////////////////////

static void init_0() {
    
    ASSERT(g_state == state_newborn);

    g_tracer.info("init_0");

    // Resolve real allocation functions in libc
    g_libc_functions.fun_malloc = (malloc_t) dlsym(RTLD_NEXT, "malloc");
    g_libc_functions.fun_realloc = (realloc_t) dlsym(RTLD_NEXT, "realloc");
    g_libc_functions.fun_free = (free_t) dlsym(RTLD_NEXT, "free");
    g_libc_functions.fun_mmap = (mmap_t) dlsym(RTLD_NEXT, "mmap");
    g_libc_functions.fun_munmap = (munmap_t) dlsym(RTLD_NEXT, "munmap");

    // Create recursive mutex
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_mutex, &attr);

    g_tracer.info("mutex created");

    g_tracer.info("init_0 done");

    g_state = state_init_0;
}

struct RunInit0 { RunInit0() { init_0(); } };
static RunInit0 run_init0;

static void* libjvm_callback_malloc(size_t len);
static void  libjvm_callback_free(void* old);
static void* libjvm_callback_realloc(void* old, size_t len);
static void* libjvm_callback_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
static int   libjvm_callback_munmap(void *addr, size_t length);

static void trace_functions(const char* title, const functions_t* f) {
    g_tracer.info("Functions %s", title);
    g_tracer.info("malloc: -> %p", f->fun_malloc);
    g_tracer.info("realloc: -> %p", f->fun_realloc);
    g_tracer.info("free: -> %p", f->fun_free);
    g_tracer.info("mmap: -> %p", f->fun_mmap);
    g_tracer.info("munmap: -> %p", f->fun_munmap);

}

// Called by hotspot. Hands over libjvm functions, returns callback functions for libjvm to call.
static void init_1(const functions_t* libjvm_functions, functions_t* libjvm_callback_functions) {

    ASSERT(g_state == state_init_0);

    g_tracer.info("init_1");

    // copy function vector from libjvm
    g_libjvm_functions = *libjvm_functions;

    // populate function vector for libjvm callbacl
    g_libjvm_callback_functions.fun_malloc = libjvm_callback_malloc;
    g_libjvm_callback_functions.fun_realloc = libjvm_callback_realloc;
    g_libjvm_callback_functions.fun_free = libjvm_callback_free;
    g_libjvm_callback_functions.fun_mmap = libjvm_callback_mmap;
    g_libjvm_callback_functions.fun_munmap = libjvm_callback_munmap;

    // hand over to libjvm
    *libjvm_callback_functions = g_libjvm_callback_functions;

    // tracing
    trace_functions("LIBC", &g_libc_functions);
    trace_functions("LIBJVM", &g_libjvm_functions);
    trace_functions("LIBJVM callback", &g_libjvm_callback_functions);
    
    g_tracer.info("init_1 done");

    g_state = state_init_1;

}


EXPORTED
void NMTInterposeInitialize(const functions_t* libjvm_functions, functions_t* libjvm_callback_functions) {
    CriticalSection cs;
    init_1(libjvm_functions, libjvm_callback_functions);
}

//////// malloc, free, realloc ///////////////////////////////////////////////

// Allocation:
//  Call libjvm function after initialization, raw libc functions beforehand
//  populate header
// Free:
//  Call the fitting free function

static void* the_malloc(size_t len) {
    void* p = nullptr;

    if (len == 0) {
        len = 1;
    }

    if (g_state == state_newborn) {
        // Too early for anything; don't have libc functions yet or am in the process of resolving them;
        // use ur-buffer
        p = g_urbuffer.alloc(len);
        g_tracer.trace("(ur) malloc (%zu): -> %p", len, p);

    } else {

        CriticalSection cs;

//        g_tracer.trace("the_malloc (%zu)...", len);

        // Before hotspot called in, use real malloc. Afterwards, call into libjvm.
        bool use_libjvm_function = g_state == state_init_1;
        p = use_libjvm_function ? 
                g_libjvm_functions.fun_malloc(len) : 
                g_libc_functions.fun_malloc(len);

        if (p != nullptr) {
            g_tracer.trace("%s_malloc (%zu): -> %p", use_libjvm_function ? "libjvm" : "libc", len, p);
            // store in map.
            if (use_libjvm_function) {
                g_pointermap.add(p);
            }
        }
    }
    return p;
}

static void the_free(void* old) {
    if (old == nullptr) {
        return;
    }
    
    if (g_urbuffer.contains(old)) {
        g_tracer.trace("ur_free (%p)", old);
        return;
    }
    
    ASSERT(g_state != state_newborn);
    {
        CriticalSection cs;

//        g_tracer.trace("the_free (%p)...", old);

        const bool allocated_via_libjvm = g_pointermap.lookup_and_remove(old);

        if (allocated_via_libjvm) {
            g_libjvm_functions.fun_free(old);
            g_tracer.trace("real_free (%p)", old);
        } else {
            g_libc_functions.fun_free(old);
            g_tracer.trace("real_free (%p)", old);
        }
    }
}

static void* the_realloc(void* old, size_t len) {
    void* p = nullptr;
    
    if (len == 0) {
        len = 1;
    }

    if (old == nullptr) {
        return the_malloc(len);
    }

    if (g_state == state_newborn) {
    
        // unimplemented
        ASSERT(false);
    
    } else {

       CriticalSection cs;

//        g_tracer.trace("the_realloc (%p, %zu)...", old, len);

        // If original allocation was done with raw malloc:
        // - if we have access to libjvm, we first realloc with libc - now we know the size - then we transfer to libjvm-allocated memory.
        // - otherwise we just raw alloc

        const bool old_was_allocated_via_libjvm = g_pointermap.lookup_and_remove(old);
        bool new_is_allocated_via_libjvm = false;

        if (!old_was_allocated_via_libjvm) {
            p = g_libc_functions.fun_realloc(old, len);
            g_tracer.trace("libc_realloc (%p, %zu): -> %p", old, len, p);
            if (p != nullptr && g_state == state_init_1) {
                // we know the new size. Transfer the whole thing to libjvm memory to have it tracked
                void* q = g_libjvm_functions.fun_malloc(len);
                if (q != nullptr) {
                    g_tracer.trace("transfering to libjvm malloc: %p -> %p", p, q);
                    memcpy(q, p, len);
                    g_libc_functions.fun_free(p);
                    p = q;
                    new_is_allocated_via_libjvm = true;
                }
             }
        } else {
            p = g_libjvm_functions.fun_realloc(old, len);
            g_tracer.trace("libjvm_realloc (%p, %zu): -> %p", old, len, p);
            new_is_allocated_via_libjvm = true;
        }
        
        if (p != nullptr) {
            if (new_is_allocated_via_libjvm) {
                g_pointermap.add(p);
            }
        }
    }

    return p;
}

static void* the_calloc(size_t num, size_t len) {
    // We keep it simple and stupid and map calloc to malloc
//    g_tracer.trace("the_calloc (%zu %zu)...", num, len);
    const size_t bytes = num * len;
    void* p = the_malloc(bytes);
    if (p != nullptr) {
        memset(p, 0, bytes);
    }
    return p;
}

/////////////// mmap, munmap ///////////////////////////////////////////////

static void* the_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
    
    void* p = nullptr;
    
    // Before hotspot init, use plain mmap. Afterwards, redirect to libjvm.
    const bool use_libjvm_function = (g_state == state_init_1);
    p = use_libjvm_function ? 
        g_libjvm_functions.fun_mmap(addr, length, prot, flags, fd, offset) :
        g_libc_functions.fun_mmap(addr, length, prot, flags, fd, offset);
    
    if (p != ((void*)-1) && g_state != state_newborn) {
        g_tracer.trace("%s_mmap: -> %p", use_libjvm_function ? "libjvm" : "libc", p);
    }

    return p;
}

static int the_munmap(void *addr, size_t length) {

    int rc = -1;
    
    // Before hotspot init, use plain mmap. Afterwards, redirect to libjvm.
    const bool use_libjvm_function = (g_state == state_init_1);
    rc = use_libjvm_function ? 
        g_libjvm_functions.fun_munmap(addr, length) :
        g_libc_functions.fun_munmap(addr, length);

    if (rc == 0 && g_state != state_newborn) {
        g_tracer.trace("%s_munap: -> %d", use_libjvm_function ? "libjvm" : "libc", rc);
    }

    return rc;

}

////////////////////////////////////////////////////////////////////////////

EXPORTED void* malloc(size_t len)             { return the_malloc(len); }
EXPORTED void  free(void* old)                { return the_free(old); }
EXPORTED void* realloc(void* old, size_t len) { return the_realloc(old, len); }
EXPORTED void* calloc(size_t num, size_t len) { return the_calloc(num, len); }

EXPORTED void* mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)     { 
    g_tracer.begin_mmap("mmap entry", addr, length, prot, flags, fd, offset);
    void* const rc = the_mmap(addr, length, prot, flags, fd, offset);
    g_tracer.end_mmap("mmap entry", rc, errno);
    return rc;
}

EXPORTED void* mmap64(void *addr, size_t length, int prot, int flags, int fd, off64_t offset)     { 
    g_tracer.begin_mmap("mmap64 entry", addr, length, prot, flags, fd, offset);
    void* const rc = the_mmap(addr, length, prot, flags, fd, offset);
    g_tracer.end_mmap("mmap64 entry", rc, errno);
    return rc;
}

EXPORTED int   munmap(void *addr, size_t length) {
    g_tracer.begin_munmap("munmap entry", addr, length);
    const int rc = the_munmap(addr, length);
    g_tracer.end_munmap("munmap entry", rc, errno);
    return rc;
}

static void* libjvm_callback_malloc(size_t len)                 { return g_libc_functions.fun_malloc(len); }
static void  libjvm_callback_free(void* old)                    { g_libc_functions.fun_free(old); }
static void* libjvm_callback_realloc(void* old, size_t len)     { return g_libc_functions.fun_realloc(old, len); }
static void* libjvm_callback_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)   { return g_libc_functions.fun_mmap(addr, length, prot, flags, fd, offset); }
static int   libjvm_callback_munmap(void *addr, size_t length)  { return g_libc_functions.fun_munmap(addr, length); }










