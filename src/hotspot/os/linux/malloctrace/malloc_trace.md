(c) SAP 2021

# The SapMachine malloctrace facility

## Motivation

We have a number of options when analyzing C-heap memory usage in the VM (without external tools):

1) NMT
2) in the SAP JVM, the SAP JVM malloc statistics

but both statistics only work for instrumented code. NMT only works for the hotspot, the SAP JVM malloc statistics work for hotspot plus instrumented parts of the JDK. Both cases miss uninstrumented allocations from within the VM, from third party code as well as from system libraries. So there is a gap.

In the past we were facing what we strongly suspected were C-heap leaks in third party code, but without the ability to prove it, since we were unable to analyze the situation with tools like valgrind; for these situations a tool is needed which
- catches all malloc calls, regardless of where it happens and whether we instrumented it
- gives us a callstack at that point.

### mtrace?

One simple possibility to do this would be the glibc-internal trace. Using `mtrace(3)`, one can force the glibc to write a trace file for all malloc calls. The problem with that is that its insanely costly. In my experiments, VM slowed down by factor 4-10. This makes sense since the glibc internal malloc trace just writes an uncompressed trace file. Another nit here is that these files would have to be post-processed to get any kind of valuable information.

## The malloc trace facility

The Linux-only malloc trace facility in the SapMachine uses glibc malloc hooks to hook into the allocation process. In that way it works like `mtrace(3)`. But unlike the glibc-internal malloc trace, it does not write a raw file but accumulates call site statistics in memory. This is way faster than `mtrace(3)`, and it can be switched off and on at runtime.

### Usage via jcmd

#### Switch trace on:

```
thomas@starfish$ jcmd AllocCHeap System.malloctrace on
268112:
Tracing activated
```

#### Switch trace off 


```
thomas@starfish$ jcmd AllocCHeap System.malloctrace off
268112:
Tracing deactivated
```

#### Print a malloc trace report. 

Two options exist, a full report which can be lengthy but will show all call sites; or a abridged reports only showing the ten "hottest" call sites. The latter is the default.

```
jcmd (VM) System.malloctrace print [all]
```

Example:

```
thomas@starfish$ jcmd AllocCHeap System.malloctrace print
268112:
---- 10 hottest malloc sites: ----
---- 0 ----
Invocs: 2813 (+0)
Alloc Size Range: 8 - 312
[0x00007fd04159f3d0] sap::my_malloc_hook(unsigned long, void const*)+192 in libjvm.so
[0x00007fd040e0a004] AllocateHeap(unsigned long, MEMFLAGS, AllocFailStrategy::AllocFailEnum)+68 in libjvm.so
[0x00007fd041891eb2] SymbolTable::allocate_symbol(char const*, int, bool)+226 in libjvm.so
[0x00007fd041895c94] SymbolTable::do_add_if_needed(char const*, int, unsigned long, bool)+116 in libjvm.so
[0x00007fd04189669f] SymbolTable::new_symbols(ClassLoaderData*, constantPoolHandle const&, int, char const**, int*, int*, unsigned int*)+95 in libjvm.so
[0x00007fd040fc0042] ClassFileParser::parse_constant_pool_entries(ClassFileStream const*, ConstantPool*, int, JavaThread*)+3026 in libjvm.so
[0x00007fd040fc0282] ClassFileParser::parse_constant_pool(ClassFileStream const*, ConstantPool*, int, JavaThread*)+34 in libjvm.so
[0x00007fd040fc1c9a] ClassFileParser::ClassFileParser(ClassFileStream*, Symbol*, ClassLoaderData*, ClassLoadInfo const*, ClassFileParser::Publicity, JavaThread*)+938 in libjvm.so
[0x00007fd04149dd3e] KlassFactory::create_from_stream(ClassFileStream*, Symbol*, ClassLoaderData*, ClassLoadInfo const&, JavaThread*)+558 in libjvm.so
[0x00007fd0418a3310] SystemDictionary::resolve_class_from_stream(ClassFileStream*, Symbol*, Handle, ClassLoadInfo const&, JavaThread*)+496 in libjvm.so
[0x00007fd041357bce] jvm_define_class_common(char const*, _jobject*, signed char const*, int, _jobject*, char const*, JavaThread*) [clone .constprop.285]+510 in libjvm.so
[0x00007fd041357d06] JVM_DefineClassWithSource+134 in libjvm.so
[0x00007fd0402cf6d2] Java_java_lang_ClassLoader_defineClass1+450 in libjava.so
[0x00007fd0254b453a] 0x00007fd0254b453aBufferBlob (0x00007fd0254afb10) used for Interpreter
---- 1 ----
Invocs: 2812 (+0)
Alloc Size: 16
[0x00007fd04159f3d0] sap::my_malloc_hook(unsigned long, void const*)+192 in libjvm.so
[0x00007fd040e0a004] AllocateHeap(unsigned long, MEMFLAGS, AllocFailStrategy::AllocFailEnum)+68 in libjvm.so
[0x00007fd041895cd6] SymbolTable::do_add_if_needed(char const*, int, unsigned long, bool)+182 in libjvm.so
[0x00007fd04189669f] SymbolTable::new_symbols(ClassLoaderData*, constantPoolHandle const&, int, char const**, int*, int*, unsigned int*)+95 in libjvm.so
[0x00007fd040fc0042] ClassFileParser::parse_constant_pool_entries(ClassFileStream const*, ConstantPool*, int, JavaThread*)+3026 in libjvm.so
[0x00007fd040fc0282] ClassFileParser::parse_constant_pool(ClassFileStream const*, ConstantPool*, int, JavaThread*)+34 in libjvm.so
[0x00007fd040fc1c9a] ClassFileParser::ClassFileParser(ClassFileStream*, Symbol*, ClassLoaderData*, ClassLoadInfo const*, ClassFileParser::Publicity, JavaThread*)+938 in libjvm.so
[0x00007fd04149dd3e] KlassFactory::create_from_stream(ClassFileStream*, Symbol*, ClassLoaderData*, ClassLoadInfo const&, JavaThread*)+558 in libjvm.so
[0x00007fd0418a3310] SystemDictionary::resolve_class_from_stream(ClassFileStream*, Symbol*, Handle, ClassLoadInfo const&, JavaThread*)+496 in libjvm.so
[0x00007fd041357bce] jvm_define_class_common(char const*, _jobject*, signed char const*, int, _jobject*, char const*, JavaThread*) [clone .constprop.285]+510 in libjvm.so
[0x00007fd041357d06] JVM_DefineClassWithSource+134 in libjvm.so
[0x00007fd0402cf6d2] Java_java_lang_ClassLoader_defineClass1+450 in libjava.so
[0x00007fd0254b453a] 0x00007fd0254b453aBufferBlob (0x00007fd0254afb10) used for Interpreter
...
<snip>
...
Table size: 8171, num_entries: 3351, used slots: 519, longest chain: 5, invocs: 74515, lost: 0, collisions: 5844
Malloc trace on.
 (method: nmt-ish)

74515 captures (0 without stack).
```

#### Reset the call site table

```
jcmd (VM) System.malloctrace reset
```

will reset the internal call site table. This can make sense if it was filled with irrelevant call sites, and may make subsequent tracing faster.


### Usage via command line

The trace can be switched on at VM startup via `-XX:+EnableMallocTrace`.

The trace can be printed upon VM shutdown to stdout via `-XX:+PrintMallocTraceAtExit`.

Both options are diagnostic and need to be unlocked in release builds with `-XX:+UnlockDiagnosticVMOptions`.

Note that starting the trace at VM startup may not be the best option since the internal call site table fills with lots of call sites which are only relevant during VM startup and will never turn up again. This may slow down registering new sites considerably.

### Memory costs

The internal data structures cost about ~5M. This is memory needed to keep call stacks and invocation counters for malloc sites. This memory is limited, and it will not grow - if we hit the limit, we won't register further call sites. Note however that the memory is dimensioned to hold 32K call sites, which far exceeds the normal number of malloc call sites in the VM.

### Performance costs

In measurements, tracing enabled brought about 6% slowdown for VM startup for normal applications (measured with eclipse and spring petclinic). The slowdown highly depends on the frequency of malloc calls. If we have an intense amount of them, slowdown can still be factor 2-3.

Note however that the typical way to use this facility would not be to have it enabled at startup, but to enable it for a short time in a suspected leak situation.


### Limitations

This facility uses glibc malloc hooks. 

Glibc malloc hooks are decidedly thread-unsafe, and we use them in a multithreaded context. Therefore what we can do with these hooks is very restricted. It boils down to the problem that after hooking you need to satisfy the user request for memory, and therefore call the original malloc(), which means you need to temporarily disable the hooks while doing that. The alternative would be to roll your very own heap implmenetation. This is possible, but several magnitudes larger than a simple tracer.

Therefore:

1) This is **not** a full leak analysis tool! All we see with this facility is the "hotness" of malloc call sites. These may be innocous; e.g. a malloc call may be followed by a free call right away.
2) **Not every allocation** will be captured. Since there are small time windows where the hooks need to be disabled.

Therefore, this is the right tool to analyze situations with a growing C-heap where a leak is suspected from a hot malloc site. It shows you which malloc sites are hot; nothing more. But it works with third party code too, even with code which just happens to run in the VM process without having anything to do with the VM.











