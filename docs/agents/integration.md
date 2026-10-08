# Build, packaging, and module integration

## Building the library from source

Applications can use existing compatible libperf headers, generated stubs, and
runtime modules. The source-build information below applies when those
artifacts need to be built for the requested task.

The project uses CMake 3.20 or newer and the VitaSDK cross toolchain. Set
`VITASDK` to the SDK installation before configuring. The root CMake file picks
`$VITASDK/share/vita.toolchain.cmake` when no toolchain file was supplied, then
includes VitaSDK's `vita.cmake` helpers.

Kernel development libraries are required, including taiHEN and
`taihenModuleUtils`. All bundled examples are configured by the root build and
require vita2d plus their graphics, controller, font, and kernel stubs. The
profiler also links `ScePower_stub` to observe CPU frequency. The current build
does not offer a documented library-only CMake option.

```sh
cmake -S . -B build
cmake --build build -j4
```

## Target and output map

| CMake target | Inputs | Main outputs |
| --- | --- | --- |
| `kernel` | `src/kernel.c` | Kernel ELF |
| `kernel_stubs` | Root `exports.yml` | `kernel_stubs/libSceKernelPerf_stub.a` and weak variant |
| Kernel SELF generation | Kernel ELF and exports | `libperf.skprx` |
| `user` | `user/src/user.c` | User ELF |
| `user_stubs` | `user/exports.yml` | `user/user_stubs/libScePerf_stub.a` and weak variant |
| User SELF generation | User ELF and exports | `user/libperf.suprx` |
| `perf_test` | `examples/pmu-test/` | `examples/perf-test.self`, `examples/retail-perf-test.vpk` |
| `perf_demo` | `examples/profiler/` | `examples/perf-demo.self`, `examples/cpu-function-profiler.vpk` |
| `perf_stress` | `examples/stress/` | `examples/perf-stress.self`, `examples/libperf-stress.vpk` |

Paths in this table are relative to the build directory. SELF/VPK helper
targets may have generated target names; use the all-target build rather than
assuming those filenames are directly addressable CMake target names.

The root compiler configuration enables `-O3`, `-Wall`, `-Wextra`, `-Werror`,
and `-fno-optimize-sibling-calls`, with Vita relocatable ELF link handling.
Kernel and user modules use `-nostdlib` and entry point `module_start`.
The profiler workload source additionally disables automatic tree and SLP
vectorization so its scalar/vector comparison remains explicit.

Flags used when creating modules are reset before creating user/example SELFs.
Do not accidentally carry a kernel module's SELF settings into an application.

## Which stubs belong where

The user module links the generated strong `SceKernelPerf_stub`, together with
native `SceLibKernel_stub` and `SceKernelThreadMgr_stub`. Its kernel imports must
resolve to a loaded kernel provider before it starts successfully.

Applications that manually load `libperf.suprx` link the generated
`ScePerf_stub_weak`. This permits the application to start before the user
module is loaded and resolve the profiling calls afterwards. The application
checks the module-load return value and module-start status before using
those calls. These are runtime API results, not binary verification.

The PMU test and stress application additionally use the generated
`SceKernelPerf_stub_weak` because they call kernel open/close for lifecycle
tests. The ordinary function-profiler application does not need those direct
kernel imports.

Prefer build-local headers and stubs. The library names also exist in SDK
environments, so linking an unintended archive can produce a build that does
not load the expected provider. Check link directory ordering and the actual
link command if module resolution is surprising.

## Standalone application CMake setup

The following is a minimal packaging pattern for an application with `main.c`.
`LIBPERF_SOURCE_DIR` identifies the headers and `LIBPERF_BUILD_DIR` identifies
an existing compatible build containing the generated stubs and user module.
Set these paths during application configuration and supply the VitaSDK
toolchain in your normal application setup.

```cmake
cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake"
      CACHE PATH "VitaSDK toolchain")
endif()

project(profile_app C)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(LIBPERF_SOURCE_DIR "" CACHE PATH "libperf source directory")
set(LIBPERF_BUILD_DIR "" CACHE PATH "libperf build directory")

add_executable(profile_app main.c)
target_include_directories(profile_app PRIVATE "${LIBPERF_SOURCE_DIR}/include")
target_link_directories(profile_app PRIVATE
  "${LIBPERF_BUILD_DIR}/user/user_stubs")
target_link_libraries(profile_app ScePerf_stub_weak SceLibKernel_stub)

vita_create_self(profile-app.self profile_app UNSAFE)
vita_create_vpk(profile-app.vpk PERFAPP01 profile-app.self
  VERSION 01.00 NAME "CPU Profiling Application"
  FILE "${LIBPERF_BUILD_DIR}/user/libperf.suprx" libperf.suprx)
```

Add the normal system stubs your application's own code uses. Do not add
vita2d solely for profiling; it is a dependency of the example presentation,
not the libperf API. When building within the libperf CMake tree, add target
dependencies on generated stubs as shown in
[`examples/CMakeLists.txt`](../../examples/CMakeLists.txt). Separate builds
cannot use each other's target dependencies and must be sequenced explicitly.

## Runtime sequence

1. The supported device boots with `libperf.skprx` enabled under taiHEN's
   `*KERNEL` section.
2. The application starts with unresolved weak ScePerf imports.
3. It calls `sceKernelLoadStartModule("app0:libperf.suprx", ...)`.
4. The loader links exports, then the user module opens process PMU access.
5. The application checks a nonnegative module ID and
   `SCE_KERNEL_START_SUCCESS` start status.
6. It owns the relevant threads' PMU configuration while profiling.
7. Before teardown it joins or parks profiling callers and stops counting.
8. The bundled examples retain the user module through process exit.

A nonnegative module ID alone is not sufficient evidence of a successful
module start. Retain both values for diagnosis. Do not immediately call a
profiling function to see whether a failed start "really worked".

Multiple independently managed profilers in one process can overwrite each
other's event selection, counter values, or process access state. The library
has no ownership arbitration. Use one coordinator and explicit synchronization.

## Runtime prerequisites

The application payload contains `libperf.suprx`. A compatible
`libperf.skprx` kernel provider must already be loaded for the user module to
start successfully. Packaging the user module in a VPK does not load the
kernel provider.

## Failure distinctions

| Failure boundary | Relevant evidence |
| --- | --- |
| Build configuration | `VITASDK`, toolchain, dependency archives, CMake errors |
| Module generation | Export YAML, ELF imports/relocations, SELF helper flags |
| Kernel plugin start | Native export resolution, heap creation, signature checks |
| User module load | Packaged path, module ID, kernel-provider linkage |
| User module start | Native process PMU-open result and start status |
| Public PMU call | Access state, validated arguments, exact native error |
| Application launch | Installed title ID and installation completion, not just VPK upload |

"Application not found" is an installation/launch problem, not PMU evidence.
"Build succeeded" establishes neither plugin startup nor counting. Avoid
changing kernel code to solve a packaging problem before checking the boundary
that actually failed.

## Lifecycle tests

The bundled test applications declare and import these kernel functions for
their disabled-state checks:

```c
int sceKernelPerfArmPmonOpen(void);
int sceKernelPerfArmPmonClose(void);
```

They park workers, close access through the kernel export, test guarded user
APIs while their weak ScePerf imports remain linked, check the timer still
advances, and reopen access. The six guards and selection exception are
documented in [the API contract](api-contract.md#return-values-and-validation-order).

Do not implement this test by unloading the user module and then calling its
former imports. Likewise, do not stop the kernel module while a user module
still depends on its syscalls. Any hot-unload support requires separate,
explicit lifecycle design and validation.
