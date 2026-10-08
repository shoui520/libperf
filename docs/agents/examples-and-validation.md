# Examples and validation

## Application inventory

| Source directory | CMake executable | VPK | Title ID | Purpose |
| --- | --- | --- | --- | --- |
| `examples/pmu-test/` | `perf_test` | `retail-perf-test.vpk` | `PERF00001` | API correctness and event evidence |
| `examples/profiler/` | `perf_demo` | `cpu-function-profiler.vpk` | `PERFDEMO1` | Comparative named-function costs |
| `examples/stress/` | `perf_stress` | `libperf-stress.vpk` | `PERFSTR01` | Concurrent correctness and lifecycle stress |

All VPKs package `libperf.suprx`. They rely on an already loaded compatible
kernel plugin. The UI exists to run/control the workload and show status; the
report files are the primary measurement and diagnostic records.

## Counter correctness application

Main control is in [`pmu-test/main.c`](../../examples/pmu-test/main.c), event
collection in [`event_coverage.c`](../../examples/pmu-test/event_coverage.c),
and controlled instruction bodies in
[`event_footprint.S`](../../examples/pmu-test/event_footprint.S).

The test creates workers on cores 0 and 1 before loading the user module and
a worker on core 2 afterwards. Workers use native semaphores for readiness,
completion, and lifetime control. Each worker remains alive after recording so
the coordinator can use its specific thread ID.

The primary assertions cover:

- ALL selection of software increment in slot 0 and instruction rename in
  slot 1, followed by ALL reset and start.
- Exactly 64 software increments on each worker and the main thread.
- Nonzero worker cycles and instruction activity under the test workload.
- Specific-thread reads matching the worker's recorded software count.
- ALL stop preventing further software increments from accumulating.
- ALL writes producing the same known value on every parked worker.
- A specific-thread write affecting the intended target.
- SELF reset producing zero while stopped.

`testNonRazor()` additionally checks the timer against native system time over
a roughly 100 ms delay, the reported frequency of 1 MHz, and 48-bit masking.
Its comparison allows a small timing tolerance because the calls have distinct
boundaries.

It sets all six software counters near wrap, executes 32 increments while
migrating among cores, and expects `16` from each counter. It tests writing the
cycle counter across ALL, cycle wraparound, reset of all seven counter values,
and broadcasts during temporary-thread churn.

Lifecycle testing closes process PMU access while the user module remains
loaded, checks a guarded call returns `NOT_INITIALIZED`, checks the timer still
advances, reopens, and verifies a known software increment. It finally stops
all targets. This is not an unload/relink test.

The report is `ux0:data/retail-perf-test/results.txt`, overwritten at launch.
The app runs once, then remains on a result screen until Circle exits. Restart
for another run. The desktop analyzer does not consume this report format.

## Event survey and targeted tests

The survey iterates all 56 accepted selectors on all three user cores, using
groups of up to six event counters. It measures a baseline and mixed workloads
with 512 and 1024 iterations. Software increment slots receive exact expected
markers. Remaining unused slots are also set to software increment.

Survey output distinguishes:

| Label | Meaning |
| --- | --- |
| `API_FAIL` | An API operation failed; test correctness failure |
| `RESPONDS` | The chosen workload produced a nonzero observation |
| `ZERO_IN_WORKLOAD` | No observation under that workload; not hardware-absence proof |
| `PASS` in a targeted delta test | Observed incremental count matches its expected tolerance |
| `REVIEW` | Numerical semantics need investigation; not automatically an API failure |

Targeted tests compare 4096 versus 8192 iterations for loads, stores, unaligned
loads, ISB, DSB, DMB variants, STREX success/failure, VFP, and NEON operations.
The expected delta is the added iteration count times operations per iteration.
The harness allows `expected / 20 + 64` tolerance. Several targets use shared
assembly bodies so the generated instruction count can be checked directly.

Extra pressure workloads test PLD/write stalls, DMB stalls, and warm versus
large-set cache/TLB behavior. These require only ordinary user instructions;
there is no MMIO or optional engine setup. Keep the distinction between an
API/software correctness failure and an event-semantics review.

## Function-profiler workloads

Definitions and data setup are in
[`profiler/workloads.c`](../../examples/profiler/workloads.c); collection and
CSV output are in [`profiler/main.c`](../../examples/profiler/main.c).

| Capture name | Work per call | Deliberate implementation detail |
| --- | --- | --- |
| `integer_chain` | 65,536 dependent integer updates | Register dependency and retained result |
| `branch_predictable` | 32,768 input loads and branch decisions | Input sorted into two condition regions |
| `branch_random` | 32,768 input loads and branch decisions | Randomized input; same explicit branch body |
| `cache_hot` | 32,768 volatile loads from a 4 KiB set | Small working set |
| `cache_cold` | 32,768 volatile loads distributed over an 8 MiB set | Large set, no privileged cache flush |
| `scalar_float` | 32 passes over 4096 float elements | Scalar multiply/add; auto-vectorization disabled |
| `neon_float` | Same float output work | Explicit four-lane NEON |
| `memcpy_512KiB` | One 512 KiB libc copy | Output checked; unused-work elimination prevented |

Functions are `noinline`; relevant sinks and inline assembly preserve intended
work. `workloadsCheck()` compares the float outputs within `0.00001f` and
checks the copied bytes. Setup and checks happen outside measurement.

There are two separate six-event bank passes:

| Slot | Bank 0 | Bank 1 |
| --- | --- | --- |
| 0 | Rename `68` | VFP rename `73` |
| 1 | D-cache miss `03` | NEON rename `74` |
| 2 | Data micro-TLB miss `05` | I-cache miss `01` |
| 3 | Branch misprediction `10` | D-cache stall `61` |
| 4 | Data reads `06` | ISB `90` |
| 5 | Data writes `07` | DSB `91` |

Each bank also records dedicated cycles and wall microseconds. Codes are hex.
Each repetition first collects one eight-call empty baseline, then measures
eight calls of each function after one warm-up call. Function order rotates
by repetition. Seven per-metric adjusted values produce independent medians.
The [measurement guide](measurement.md#baselines-batches-and-repetitions)
defines the exact formula.

The main thread is pinned to the selected user core. CPU clock is read at both
endpoints and never forced. The loading frame finishes rendering before
collection; there are no redraws inside the measured loops. CSV saving occurs
after collection and does not contribute to isolated function measurements.

On-screen summary ranks bank 0 cycle medians. Other views show subsets of
bank events; the CSV preserves all six events in each bank and all raw data.
Cross reruns, Triangle advances the core and reruns, Left/Right selects view,
Up/Down selects a workload, and Circle exits. CSV saving failure is shown even
if measurement succeeded. Save the report before another run overwrites it.

## Stress topology and worker lifetime

The stress application consists of
[`main.c`](../../examples/stress/main.c),
[`queue.h`](../../examples/stress/queue.h), and the
[`pipeline`](../../examples/stress/pipeline.c).

There are three persistent workers pinned to cores 0, 1, and 2. Two exist
before user-module loading; the third is created afterwards. Worker priority
is `0xA0`; the core-0 coordinator uses `0x100` while doing independent scalar
verification. The churn controller is on core 1 and creates temporary workers
round-robin across user cores.

Workers support four modes: probe, wrap/migration, mixed work, and exit. A go
semaphore dispatches a mode; a done semaphore and an acquire/release completion
flag publish the result. Completion flags do not replace the semaphore waits.
The main app retains workers throughout its runs and exits the process on
final teardown.

## Work pipeline and exact oracles

Each epoch completes 192 verified jobs:

- Each worker first receives 24 private jobs, guaranteeing all three cores do
  useful work before queue competition.
- The remaining 120 jobs enter a shared input queue.
- All 192 results enter a shared output queue.
- A job seed is derived from its epoch and job ID, so the coordinator can
  compute an independent expected result.

Each worker has a private 2 MiB arena and fixed tile storage. A job:

1. Generates 1024 input words, a byte tile, and scattered arena writes.
2. Runs 16 rounds of integer NEON arithmetic on the input words.
3. RLE-encodes the byte tile into bounded pair storage.
4. Validates the encoded stream against the tile and hashes transformed words
   plus decoded bytes.

The coordinator's `referenceChecksum()` uses a separate scalar calculation
without relying on the queue, SIMD output, or encoded stream. It checks job ID
range, uniqueness, seed, worker core, and checksum. The arena size is a
working-set choice; it does not mean every job processes the entire 2 MiB.

Each job measures four stage cycle spans and emits exactly four software
increments in counter 0. Job accounting and checksum validation are the exact
correctness oracles. Microarchitectural event magnitudes are observations.

## Queue invariants

Both queues are bounded MPMC rings with capacity 32, a power of two. Each slot
contains an atomic sequence and one value-type `Message`. Producers/consumers
reserve positions using relaxed compare/exchange, but publish slot ownership
with release stores and observe it through acquire loads.

Changing sequence publication to relaxed ordering can expose uninitialized or
reused messages. Changing capacity requires preserving the mask and sequence
logic. The default `sizeofQueue=648` report reflects the current Vita layout;
do not treat it as a portable host ABI constant.

The producer also drains the outgoing queue. Blocking indefinitely on input
submission while workers are blocked on a full output queue creates a
backpressure deadlock. The current loop pumps both directions, and workers
check the stop flag while trying to publish results.

Guaranteed private jobs avoid a prior harness starvation condition where the
coordinator's verification work left the core-0 worker with no shared jobs.
Do not remove the guarantee and interpret a zero-job worker as a PMU failure.

## PMU phases in every epoch

The control phase selects a bank, resets, and performs exact probe markers:
initial software value plus `64 + 17 * core`. It compares parked self snapshots
with specific-thread reads of all seven counters. It then checks specific
writes, switches all six slots to software increment, sets values near wrap,
and moves workers across all cores while emitting increments. All six wrapped
software results must equal 16, and cycle values must show wrap activity.

It stops and resets again, then verifies all seven values are zero. Invalid
input assertions cover an unaccepted event, event-counter index 6, read index
32, null get pointer, ALL get, invalid write index, invalid software mask,
and a successful zero mask.

Every fourth epoch, including epoch zero, a lifecycle phase parks workers,
closes process access, checks all six disabled-state guards, verifies the timer
continues and its frequency is 1 MHz, tests valid closed-state selection, then
reopens and verifies one software increment. The user module remains loaded.

## Live reads, broadcasts, and churn

During queued work, the coordinator periodically reads each worker's cycle
counter and changes slot 5 across ALL. Twelve transient threads per epoch
select software increments, reset/start, emit one marker, stop/read, then end
and are deleted after a wait. Cumulative churn counts advance by 12 each epoch.

Persistent workers configure event slots 1 through 5 by `epoch % 3`:

| Bank | Slot 1 | Slot 2 | Slot 3 | Slot 4 | Slot 5 before live changes |
| --- | --- | --- | --- | --- | --- |
| 0 | `68` | `03` | `05` | `10` | `06` |
| 1 | `73` | `74` | `61` | `07` | `04` |
| 2 | `63` | `64` | `65` | `70` | `72` |

Slot 0 is always software increment for mixed work. Live slot-5 broadcasts
choose `06` on even epochs and `07` on odd epochs. Therefore slot 5 has mixed
meaning and must not receive a fixed-event aggregate in analysis. The analyzer
retains its raw values and summarizes only stable slots 1 through 4.

Only the live churn broadcast path specially records `0x80028021` and mapped
invalid-argument errors as vanished-thread observations. Other control phases
and persistent-worker assertions still require success. Do not globally ignore
those errors or claim that a failing broadcast updated every thread.

Workers stop their own counters and publish snapshots before the coordinator
compares them. ALL start is not repeated after dispatch. This prevents an
already-finished worker from being restarted while another is still busy.

## Stress reporting and controls

The app starts a 32-epoch run on launch. Cross requests 32 epochs, Triangle
256, and Square uses `UINT32_MAX` as the practical continuous-run limit.
Circle requests cancellation after the current epoch; when idle it exits.
A failed run blocks further runs until relaunch.

Work/completion waits have watchdogs, including a ten-second work-phase
deadline. A failure is logged with its stage or integrity/accounting details.
The final RESULT contains completed epochs, jobs in those epochs, and created
transient-thread count; a failed partial epoch can have additional thread
activity outside the completed-job total.

Reports go to `ux0:data/libperf-stress/results.txt`. Launch truncates the file;
subsequent runs within that application session append. Each log line is
flushed and the file is closed between runs, permitting a complete idle-state
download. The [format guide](capture-formats.md#stress-log-format) specifies
records and aggregation.

## Established validation and its limits

Retail 3.65 testing covered the nine APIs, existing/future workers, all three
user cores, wrapping counters, migration, partial broadcasts, and repeated
close/reopen. The extended stress workload completed 256 epochs: 49,152
verified jobs, 3,072 transient threads, and 64 close/reopen phases.

The event survey observed 44 responsive selectors among 56 accepted ones.
Known-count software and several targeted instruction tests passed. DMB
targets remain unverified and zero in tested workloads; responses do not
establish every event's exact architectural meaning.

Host queue testing exercised 100,000 jobs with four concurrent consumers under
address/undefined-behavior sanitizers, checking missing, duplicate, and corrupt
messages. Host checks also independently recomputed the function CSV medians
and stress accounting.

These results describe tested artifacts and workloads, not blanket support for
all firmware or all Cortex-A9 event behavior. A complete application PASS
report does not prove a complete device event stream or absence of every crash.
If server events are lost or reconnect, retain that limitation separately from
the verified application report.
