# CPU profiling with libperf

libperf gives a retail PS Vita application access to its Cortex-A9 performance
monitor counters. You can measure a function's CPU cycles, count events such as
cache misses and branch mispredictions, and compare different implementations.
The library also provides a global timer for elapsed time.

The current implementation has been tested on **retail/CEX firmware 3.65 with
taiHEN**. It implements nine counter and timebase functions. Razor integration,
trace markers, and interrupt-driven instruction sampling are outside its scope.
Measurements use boundaries you add to your application.

## What you measure

Each profiled thread has six configurable event counters, numbered `0` through
`5`, plus a dedicated cycle counter numbered `31`. An event code chooses what
an event counter counts; it does not choose the counter's number. For example,
you can put the branch-misprediction event `0x10` into counter `0`.

Counters follow the thread through scheduling and migration between the three
user CPU cores. A worker's counters describe that worker's execution. Reading
them from the main thread does not turn them into whole-core statistics.

Use the timebase for elapsed time, including waits and time when the thread was
descheduled. Use the cycle counter for counted CPU work. They answer different
questions, so record both when a workload can block.

## Build the library and examples

You need VitaSDK with its CMake support, the taiHEN development libraries, and
vita2d plus its dependencies for the bundled examples. Set `VITASDK` to your
SDK installation, then run these commands from the repository root:

```sh
cmake -S . -B build
cmake --build build -j4
```

The important outputs are:

| Output | Purpose |
| --- | --- |
| `build/libperf.skprx` | Kernel plugin providing retail PMU access |
| `build/user/libperf.suprx` | User module exporting the profiling functions |
| `build/user/user_stubs/libScePerf_stub_weak.a` | Application imports for runtime module loading |
| `build/examples/retail-perf-test.vpk` | Counter correctness and event tests |
| `build/examples/cpu-function-profiler.vpk` | Named-function measurements |
| `build/examples/libperf-stress.vpk` | Three-core workload and lifecycle tests |

The [example build configuration](../examples/CMakeLists.txt) is a complete
working integration. The [integration guide](agents/integration.md) explains
the dependencies and generated stubs in more detail.

## Install the kernel plugin

On a supported Vita, copy `libperf.skprx` to a taiHEN plugin location, for example
`ur0:tai/libperf.skprx`, and add its path under the active configuration's
existing `*KERNEL` section:

```text
*KERNEL
ur0:tai/libperf.skprx
```

Preserve the other entries in that section and use the configuration your device
actually loads. Reboot to load the kernel plugin. Install a bundled VPK to run
an example; the VPK contains the user module, but does not install or configure
the kernel plugin.

Do not assume another firmware is supported merely because the plugin builds.
Kernel initialization checks firmware-specific code before applying its two
patches. If initialization fails, establish compatibility before proceeding.

## Load the user module in your application

Include this repository's `include/libperf.h`, link the **generated weak
`ScePerf` stub**, and package `build/user/libperf.suprx` at the root of your VPK.
The module will then be available as `app0:libperf.suprx`.

```c
#include <psp2/kernel/modulemgr.h>
#include "libperf.h"

static int loadProfiler(SceUID *module)
{
	int status = 0;
	SceUID id = sceKernelLoadStartModule("app0:libperf.suprx",
					   0, NULL, 0, NULL, &status);
	if (id < 0)
		return id;
	if (status != SCE_KERNEL_START_SUCCESS)
		return status < 0 ? status : -1;
	*module = id;
	return 0;
}
```

Check both the returned module ID and its start status before calling libperf.
Loading starts the user module, which opens PMU access for the process. There
is no separate public `scePerfInit()` call. The kernel plugin must already be
loaded for this step to succeed.

Keep the user module loaded until every profiling call has finished. Weak
imports are resolved by module loading; they are not callable fallbacks when
the module is missing or has been unloaded. The bundled examples keep it loaded
until process exit.

For an existing application target, the relevant CMake setup is:

```cmake
# LIBPERF_SOURCE_DIR and LIBPERF_BUILD_DIR identify this library's checkout/build.
target_include_directories(my_app PRIVATE ${LIBPERF_SOURCE_DIR}/include)
target_link_directories(my_app PRIVATE ${LIBPERF_BUILD_DIR}/user/user_stubs)
target_link_libraries(my_app ScePerf_stub_weak SceLibKernel_stub)

# In your existing vita_create_vpk(...) call, add:
# FILE ${LIBPERF_BUILD_DIR}/user/libperf.suprx libperf.suprx
```

Build libperf before the application. Use the locally generated stubs so the
application imports the intended module.

## Measure a function

Configure events while stopped, reset the counters, start them, execute the
work, stop them, and read the result. All counter calls return an `int`; check
for a negative result before using their outputs.

This helper measures one callback on the calling thread, using counter `0` for
branch mispredictions and counter `31` for cycles:

```c
#include <psp2/kernel/threadmgr.h>
#include <stdint.h>
#include "libperf.h"

typedef void (*ProfileWork)(void *context);

typedef struct {
	SceUInt32 cycles;
	SceUInt32 branchMisses;
	SceUInt64 ticks;
} ProfileSample;

static int measureWork(ProfileWork work, void *context, ProfileSample *sample)
{
	const SceUID self = SCE_PERF_ARM_PMON_THREAD_ID_SELF;
	SceUInt64 before, after;
	int ret;

	ret = scePerfArmPmonStop(self);
	if (ret < 0)
		return ret;
	ret = scePerfArmPmonSelectEvent(self, 0,
				     SCE_PERF_ARM_PMON_BRANCH_MISPREDICT);
	if (ret < 0)
		return ret;
	ret = scePerfArmPmonReset(self);
	if (ret < 0)
		return ret;
	ret = scePerfArmPmonStart(self);
	if (ret < 0)
		return ret;

	before = scePerfGetTimebaseValue();
	work(context);
	after = scePerfGetTimebaseValue();

	ret = scePerfArmPmonStop(self);
	if (ret < 0)
		return ret;
	ret = scePerfArmPmonGetCounterValue(self,
					 SCE_PERF_ARM_PMON_CYCLE_COUNTER,
					 &sample->cycles);
	if (ret < 0)
		return ret;
	ret = scePerfArmPmonGetCounterValue(self, 0, &sample->branchMisses);
	if (ret < 0)
		return ret;
	sample->ticks = (after - before) & UINT64_C(0xFFFFFFFFFFFF);
	return 0;
}
```

The callback must finish normally on the same thread. Its cost includes called
helpers and the measurement machinery inside the start/stop interval. Reset
and start affect all six event counters and the cycle counter, so give the
profiler exclusive ownership of that thread's PMU state during measurement.

For very short functions, measure a batch of calls and divide by the call count.
Repeat batches, measure an empty-call baseline using the same path, and retain
the individual samples. The function-profiler example implements this approach.

## Choose useful events

Start with a small set that answers a concrete question:

| Event | Code | Useful question |
| --- | --- | --- |
| D-cache miss | `0x03` | Does a larger working set create more cache pressure? |
| Data micro-TLB miss | `0x05` | Does the access pattern increase translation pressure? |
| Data read / write | `0x06` / `0x07` | How does memory activity change? |
| Branch misprediction | `0x10` | Does unpredictable control flow cost more? |
| Instruction rename | `0x68` | How does instruction activity change? |
| VFP / NEON rename | `0x73` / `0x74` | Is floating-point or vector code active? |
| Software increment | `0x00` | Did a known number of software markers execute? |

Six event counters can run together with the dedicated cycle counter. For more
events, rerun the workload with another event bank. Preserve each bank's own
cycle and time measurements; counters from separate passes are not simultaneous.

Use `31` to read cycles without consuming an event slot. Event code `0x11` also
counts cycles when assigned to a configurable counter; it is not counter `31`.

The [API contract](agents/api-contract.md#accepted-event-codes) lists every
accepted event. Acceptance does not guarantee that a workload produces a
nonzero count. DMB-related events remain unverified in the tested workloads.
Optional-engine events should not prompt attempts to enable unavailable hardware.

## Count software markers

Select event `0x00` for an event counter, then increment it with a bit mask.
Mask bit `n` targets counter `n`. The following operation increments counter
`0` on the calling thread once while it is enabled and configured for software
increments:

```c
int ret = scePerfArmPmonSoftwareIncrement(SCE_PERF_ARM_PMON_COUNTER_MASK_0);
```

`0x3F` addresses all six event counters. A zero mask succeeds without changing
any counter. Bits outside `0` through `5` are invalid. The cycle counter cannot
be incremented this way. Software markers make good correctness checks because
you can compare their result with an exact expected count.

## Profile worker threads

`SCE_PERF_ARM_PMON_THREAD_ID_SELF` is `0`. Pass a live thread's application
thread ID to configure or read that specific thread. Use
`SCE_PERF_ARM_PMON_THREAD_ID_ALL` for reset, select, start, stop, and set operations
across existing threads in the current process. `-1` is also accepted for those
broadcast operations.

ALL is a snapshot, not a persistent rule for future threads. After creating
another worker, configure that worker explicitly or run a new broadcast. An
ALL call is sequential and may partially apply before reporting that a target
thread disappeared. Arrange configuration while your workers are parked when
you need consistent state.

There is no ALL read. Read each worker separately and aggregate only metrics
whose meaning permits aggregation. To get a stable snapshot, have workers stop
their own counters, publish completion, and remain alive while the coordinator
reads them.

For repeatable single-core measurements, pin the worker with
`sceKernelChangeThreadCpuAffinityMask()` and a user-core mask. The example uses
`SCE_KERNEL_CPU_MASK_USER_0 << core` for `core` values `0`, `1`, and `2`.
The [measurement guide](agents/measurement.md) explains synchronization,
counter wraparound, and inclusive versus exclusive function costs.

## Convert elapsed time

The timebase is a free-running **48-bit** counter. This implementation uses the
native microsecond system timer and returns frequency `1` **MHz**, independent
of CPU clock speed and PMU start/stop state.

```c
SceUInt64 ticks = (after - before) & UINT64_C(0xFFFFFFFFFFFF);
SceUInt32 mhz = scePerfGetTimebaseFrequency();
double elapsedUs = (double)ticks / mhz;
double elapsedSeconds = (double)ticks / ((double)mhz * 1000000.0);
```

Here, one tick is one microsecond. Do not multiply by the CPU clock or interpret
the returned `1` as one hertz. The wrap mask handles one wrap; it cannot recover
arbitrarily long intervals spanning multiple wraps.

The PMU counters are 32-bit. For snapshots within an interval shorter than a
full wrap, compute a delta with `(SceUInt32)(after - before)`. At 333 MHz a
continuously counting cycle counter wraps in about 12.9 seconds of counted
cycles. Use shorter intervals; the library does not extend counters to 64 bits.

## Run the bundled applications

| Application | What it does | Controls | Device report |
| --- | --- | --- | --- |
| Retail CPU PMU test | Validates counters, thread operations, timing, and events | Runs at launch; Circle exits | `ux0:data/retail-perf-test/results.txt` |
| CPU Function Profiler | Compares eight named workloads using two banks | Cross reruns; Triangle changes core; Left/Right changes view; Up/Down selects a function; Circle exits | `ux0:data/libperf-demo/results.csv` |
| Three-core libperf Stress Lab | Checks mixed workloads and PMU lifecycle on all three cores | Starts 32 epochs; Cross runs 32; Triangle runs 256; Square runs continuously; Circle stops after the current epoch or exits while idle | `ux0:data/libperf-stress/results.txt` |

The function CSV is overwritten on each run. Save it before switching cores if
you want to compare cores. Stress runs append within one application session;
relaunching the application starts a new report. Wait until stress is idle before
copying its closed report for analysis.

The correctness-test report is useful for diagnosis. The desktop analyzer
accepts the function CSV and stress log formats; it does not parse the
correctness-test report.

## Analyze results on your desktop

Copy the report to your desktop using your usual device file-transfer workflow.
Run the analyzer from the repository root with Python 3.8 or newer. It needs no
third-party Python packages.

```sh
python3 tools/analyze.py results.csv
python3 tools/analyze.py results.csv --bank 0 --sort wall
python3 tools/analyze.py results.txt
python3 tools/analyze.py core0.csv core1.csv core2.csv
python3 tools/analyze.py results.csv --json > analysis.json
```

The default output is a static terminal report with rankings, event counts,
cost comparisons, and sample variation. `--tui` explicitly selects the same
printed report. `--json` exports the complete structured analysis, including
raw batches. Multiple input files remain separate captures.

For function CSVs, the analyzer recomputes each reported median from matching
raw and baseline rows. Missing rows, duplicates, and altered medians are errors.
For stress logs, it checks completed-epoch accounting and reports recorded
`PASS`, `FAIL`, `CANCELLED`, or `INCOMPLETE` status. A valid failed stress run can
be analyzed successfully; exit code `0` means analysis succeeded, not that the
stress test passed. Malformed captures produce exit code `2`.

The [capture-format guide](agents/capture-formats.md) describes the JSON fields
and how to build another consumer.

## Read the numbers carefully

Compare implementations that perform equivalent work and validate their output.
Warm up deliberately, keep rendering and logging outside isolated measurements,
record CPU clock readings, and inspect variation across repeated samples.
Memory costs can change with cache state and other activity even on a pinned
core.

Instruction rename counts are not retired-instruction counts. Hardware events
can overlap and have revision-dependent behavior; NEON rename counts in
particular can be affected by Cortex-A9 errata. A zero DMB count is not evidence
that a DMB did not execute.

The function report's share is the share of summed median cycles for one call
of each listed workload. It is not your game's CPU utilization. Its span is
the observed sample range relative to the median, not a confidence interval.
Stress stage costs are job-weighted averages of gross spans under contention;
they are not baseline-adjusted isolated function measurements.

## Diagnose common failures

| Symptom | Check |
| --- | --- |
| User module will not start | Kernel plugin loaded; both module ID and start status checked; supported firmware and correct module packaged |
| `0x80580005` | PMU access is closed or unavailable to the caller; check initialization and lifecycle |
| `0x80580000` | Counter ID, event code, software mask, thread ID, and output pointer; an ALL read is invalid |
| ALL call fails during thread churn | A target may have vanished after enumeration; earlier targets may already be updated |
| Counters remain zero | Correct event and thread selected; counters started; workload can generate that event |
| Huge or negative-looking delta | Counter width and wrap arithmetic; signed formatting; lifetime and reset boundaries |
| Call crashes after unloading | Weak imports may be unlinked; keep the module loaded while any caller can use it |
| Analyzer rejects a capture | Transfer completeness and exact format; preserve metadata, baseline rows, and raw rows |

Kernel-dependent calls can also return native error codes. Retain the exact
error in hexadecimal and the operation that failed.
