# IDE-only CMake project for HotSpot (CLion / clangd indexing).
# The real build is still done with make; this project is not meant to link a libjvm.
#
# Flags are taken from compile_commands.json of a macOS/aarch64 fastdebug build
# (../output-fastdebug). Sources are globbed with CONFIGURE_DEPENDS, so new files
# are picked up on the next CMake reload.

cmake_minimum_required(VERSION 3.20)

set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "")
set(CMAKE_OSX_DEPLOYMENT_TARGET 11.0 CACHE STRING "")


# Same SDK as the make build; the default CommandLineTools SDK is too new for this clang.
set(CMAKE_OSX_SYSROOT /Applications/Xcode-15.4.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX14.5.sdk CACHE PATH "")
set(CMAKE_CXX_COMPILER /usr/bin/clang++ CACHE FILEPATH "")

project(hotspot CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# this is the source root (the root of the repo)
set(SRC_ROOT   ${CMAKE_CURRENT_SOURCE_DIR})

# root of hotspot
set(HS         ${SRC_ROOT}/src/hotspot)

# my output dir is *not* under repo_root/build but alongside the repo.
# I hate having output stuff in the source dir.
set(OUTPUT_DIR ${SRC_ROOT}/../output-fastdebug)

# same goes for the generated stuff (adlc etc)
set(GENSRC     ${OUTPUT_DIR}/hotspot/variant-server/gensrc)

# gtest loves somewhere else entirely
set(GTEST_ROOT /shared/projects/openjdk/gtest/googletest-1.14.0)

# Host platform
set(HS_OS     bsd)
set(HS_CPU    aarch64)
set(HS_OS_CPU ${HS_OS}_${HS_CPU})

set(HS_SOURCE_DIRS
  ${HS}/share
  ${HS}/os/${HS_OS}
  ${HS}/os/posix
  ${HS}/cpu/${HS_CPU}
  ${HS}/os_cpu/${HS_OS_CPU}
)

function(glob_sources out)
  set(patterns)
  foreach(dir ${ARGN})
    list(APPEND patterns ${dir}/*.cpp ${dir}/*.hpp ${dir}/*.h)
  endforeach()
  file(GLOB_RECURSE files CONFIGURE_DEPENDS ${patterns})
  set(${out} ${files} PARENT_SCOPE)
endfunction()

#### libjvm

glob_sources(JVM_SOURCES ${HS_SOURCE_DIRS})
# Not part of libjvm: adlc is a separate build tool, zero interpreter is for the zero port only.
list(FILTER JVM_SOURCES EXCLUDE REGEX "/share/adlc/")
list(FILTER JVM_SOURCES EXCLUDE REGEX "/share/interpreter/zero/")

add_library(jvm OBJECT EXCLUDE_FROM_ALL ${JVM_SOURCES}
        src/hotspot/share/memory/arenaChunkList.hpp)

target_include_directories(jvm PUBLIC
  ${GENSRC}/adfiles
  ${HS}/share
  ${HS}/os/${HS_OS}
  ${HS}/os/posix
  ${HS}/cpu/${HS_CPU}
  ${HS}/os_cpu/${HS_OS_CPU}
  ${GENSRC}
  ${HS}/share/include
  ${HS}/os/posix/include
  ${OUTPUT_DIR}/support/modules_include/java.base
  ${OUTPUT_DIR}/support/modules_include/java.base/darwin
  ${SRC_ROOT}/src/java.base/share/native/libjimage
  ${SRC_ROOT}/src/java.base/unix/native/include
  ${SRC_ROOT}/src/java.base/share/native/include
)

target_compile_definitions(jvm PUBLIC
  __STDC_FORMAT_MACROS
  __STDC_LIMIT_MACROS
  __STDC_CONSTANT_MACROS
  _GNU_SOURCE
  MAC_OS_X_VERSION_MIN_REQUIRED=110000
  LIBC=default
  _ALLBSD_SOURCE
  _DARWIN_C_SOURCE
  _XOPEN_SOURCE
  VM_LITTLE_ENDIAN
  _LP64=1
  ASSERT
  CHECK_UNHANDLED_OOPS
  TARGET_ARCH_${HS_CPU}
  INCLUDE_SUFFIX_OS=_${HS_OS}
  INCLUDE_SUFFIX_CPU=_${HS_CPU}
  INCLUDE_SUFFIX_COMPILER=_gcc
  TARGET_COMPILER_gcc
  AARCH64
  HOTSPOT_LIB_ARCH="${HS_CPU}"
  COMPILER1
  COMPILER2
  DTRACE_ENABLED
)

target_compile_options(jvm PUBLIC
  -fno-rtti
  -fno-exceptions
  -fvisibility=hidden
  -fno-strict-aliasing
  -fno-omit-frame-pointer
  -Wall -Wextra -Wformat=2 -Wpointer-arith -Wsign-compare -Wreorder
  -Wunused-function -Wundef -Wunused-value -Woverloaded-virtual
  -Wzero-as-null-pointer-constant
  -Wno-unknown-warning-option -Wno-unused-parameter
  -Wno-delete-non-abstract-non-virtual-dtor -Wno-invalid-offsetof
  -Wno-missing-braces -Wno-sometimes-uninitialized -Wno-unknown-pragmas
  -Wno-unused-but-set-variable -Wno-unused-local-typedef
  -Wno-unused-private-field -Wno-unused-variable
  -Wno-tautological-constant-out-of-range-compare
)

# Version info, only needed by these two files
set_source_files_properties(
  ${HS}/share/runtime/abstract_vm_version.cpp
  ${HS}/share/runtime/arguments.cpp
  PROPERTIES COMPILE_DEFINITIONS
  "HOTSPOT_VERSION_STRING=\"28-internal\";VERSION_STRING=\"28-internal\";VERSION_SHORT=\"28-internal\";VERSION_NUMBER=\"28\";VERSION_SPECIFICATION=\"28\";VERSION_FEATURE=28;VERSION_INTERIM=0;VERSION_UPDATE=0;VERSION_PATCH=0;VERSION_EXTRA1=0;VERSION_EXTRA2=0;VERSION_EXTRA3=0;VERSION_BUILD=0;VERSION_PRE=\"internal\";VERSION_OPT=\"\";VERSION_DATE=\"2027-03-23\";VERSION_CLASSFILE_MAJOR=72;VERSION_CLASSFILE_MINOR=0;VENDOR_VERSION_STRING=\"\";VENDOR_URL=\"https://openjdk.org/\";VENDOR_URL_BUG=\"https://bugreport.java.com/bugreport/\";VENDOR_URL_VM_BUG=\"https://bugreport.java.com/bugreport/crash.jsp\";HOTSPOT_VM_DISTRO=\"OpenJDK\";HOTSPOT_BUILD_TIME=\"\";JVM_VARIANT=\"server\";CPU=\"${HS_CPU}\";DEBUG_LEVEL=\"fastdebug\""
)

#### gtests

glob_sources(GTEST_SOURCES ${SRC_ROOT}/test/hotspot/gtest)

add_library(jvm_gtest OBJECT EXCLUDE_FROM_ALL ${GTEST_SOURCES})

# Inherits all libjvm flags via PUBLIC usage requirements
target_link_libraries(jvm_gtest PRIVATE jvm)

target_include_directories(jvm_gtest PRIVATE
  ${SRC_ROOT}/test/hotspot/gtest
  ${GTEST_ROOT}/googletest/include
  ${GTEST_ROOT}/googlemock/include
)

target_compile_definitions(jvm_gtest PRIVATE
  HOTSPOT_GTEST
  GTEST_OS_MAC=1
)

target_compile_options(jvm_gtest PRIVATE
  -Wno-format-nonliteral -Wno-switch -Wno-undef
  -Wno-tautological-undefined-compare -Wno-self-assign-overloaded
)
