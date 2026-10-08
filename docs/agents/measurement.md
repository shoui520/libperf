# Measurement design and interpretation

## Decide the question first

Choose a unit of work and the metric that answers the question. Examples are
cycles per decoded block, branch mispredictions per search, cache misses per
tile, or elapsed microseconds per frame. Validate output equivalence before
comparing alternate implementations.

libperf supplies counters, not an automatic statistical or call-stack profiler.
Function names and boundaries come from your instrumentation. The bundled
function app is an inclusive boundary profiler; it does not sample PCs,
attribute arbitrary instructions, or reconstruct stack traces.

## Three distinct kinds of time

| Quantity | How it is obtained | What it includes |
| --- | --- | --- |
| Counted CPU cycles | Counter `31` | Execution counted for the target thread, including in-boundary measurement effects |
| Elapsed wall time | Difference of timebase values | Waiting, descheduling, and work between the timestamp reads |
| Host transfer/analysis time | Desktop timestamps | File transport and analysis; never the Vita function's CPU cost |

The current PMU configuration has no public user-only versus kernel-only filter.
Do not present measured thread cycles as exclusively user-mode instruction
costs. Timer reads and native calls can contribute to a counted interval.

Cycles divided by wall microseconds need not equal the CPU clock. The intervals
have different boundaries, and descheduling contributes to wall time. A clock
read before and after a run establishes endpoints, not a continuous clock
trace. Matching endpoints do not prove the frequency never changed.

## Widths and unit conversions

Event and cycle counters are 32-bit unsigned. For two snapshots:

```c
SceUInt32 delta = (SceUInt32)(after - before);
```

This recovers a modulo delta when the true increment is less than `2^32`.
It cannot distinguish one whole wrap from additional wraps. At a continuous
333 million cycles per second, the dedicated cycle counter wraps after roughly
12.9 seconds of counted cycles. Event counters have event-dependent rates and
may wrap sooner. Use short enough intervals and accumulate verified short
deltas in a 64-bit application total when longer measurements are needed.

Timebase values use 48 bits even though stored in `SceUInt64`:

```c
SceUInt64 ticks = (after - before) & UINT64_C(0xFFFFFFFFFFFF);
double us = (double)ticks / scePerfGetTimebaseFrequency();
```

Frequency is in MHz. The current returned value is `1`, so ticks are
microseconds. A one-microsecond wall timer does not have enough resolution to
make a single tiny call's duration meaningful; batching helps.

## Configure before collecting

A stable isolated measurement follows this order:

1. Load and validate the user module, allocate input/output data, and validate
   the workload's correctness.
2. Stop the target's counters and select the event bank.
3. Warm up intentionally outside the measured interval.
4. Reset all counter values.
5. Start counting.
6. Read the first timer value, execute the measured calls, read the second value.
7. Stop counting and read the counters.
8. Preserve the sample, including errors and collection metadata.

Starting does not clear values. Resetting does not stop a running target.
Selecting an event does not clear existing counts. Avoid changing event
selectors mid-interval unless deliberately testing that behavior.

If the workload fails while counting, still attempt to stop before returning
or restarting another measurement, while preserving the original failure.
Do not log, redraw, transfer files, or allocate memory inside an isolated
function interval unless that activity is intentionally part of the workload.

## Baselines, batches, and repetitions

The function-profiler app uses two banks, seven repetitions per bank, and eight
calls per measured batch. Each repetition has an empty-call batch through the
same measurement path. Each real function gets one warm-up call before its
batch; the function order rotates with repetition.

For metric `m`, bank `b`, function `f`, and repetition `r`:

```text
adjusted[b,f,r,m] = max(0, raw[b,f,r,m] - baseline[b,r,m])
reported[b,f,m]   = median_r(adjusted[b,f,r,m])
per_call[b,f,m]   = reported[b,f,m] / calls_per_batch
```

The subtraction occurs before taking the median. Do not compute
`median(raw) - median(baseline)` and assume it is equivalent. Clamping happens
per sample and per metric. Counter/timer medians are independent and can come
from different repetitions; the reported row is not one actual simultaneous
sample.

The baseline reduces harness cost but does not promise perfect subtraction.
Calling an empty function and calling a real function can exercise different
cache, branch, and scheduling state. Large baseline contributions or zero
adjusted values deserve scrutiny.

The CSV preserves batches and their call counts. Divide by calls only once.
The desktop JSON `raw_batches`, `adjusted_batches`, and `baseline_batches`
still contain batch values; fields explicitly named `*_per_call` are normalized.

## Sample variation and ranking

For adjusted cycles per call `x[r]`, the analyzer reports:

```text
minimum = min(x)
maximum = max(x)
span_pct = 100 * (maximum - minimum) / median(x)
```

When the median is zero, span percentage is null. A span is an observed range,
not a confidence interval or an error bound on future runs. Seven repetitions
are useful for this demonstration, not a universal statistically sufficient
sample size.

The default ranking is descending cycles per call within a bank. `--sort wall`
changes the ranking and relative-cost bar to wall time, but the displayed
equal-call share remains a cycle share. The share definition is:

```text
equal_call_cycle_share_pct[f] = 100 * cycles_per_call[f] / sum_f(cycles_per_call[f])
```

This describes a synthetic mix with one call to each function. It is not frame
time, whole-process utilization, a flame graph, or the cost distribution of an
application with different call frequencies.

## Event banks

Six configurable counters can measure six chosen events alongside the cycle
counter. More selectors require separate passes. Each pass retains its own
cycles, wall time, baseline, and samples.

Do not merge bank 0's numerator with bank 1's denominator and label the result
a simultaneous hardware rate. Even identical inputs can encounter different
cache state and contention. If cross-pass ratios are useful, label them as
cross-pass estimates and preserve the separate observations.

A PMU event count is not automatically an instruction count or elapsed time.
Rename and speculative activity can differ from architectural retirement.
Stall categories can overlap and do not necessarily partition total cycles.
Adding all stall or pipeline event counts does not produce utilization.

## Inclusive and nested function boundaries

An outer function's count includes called helpers. If both parent and child
have inclusive measurements, adding their totals double-counts child work.
The library has no stack of nested counter configurations or automatic
exclusive-time calculation.

For nested boundaries under one fixed bank, start once and use short snapshot
deltas. Do not reset, stop, or select another bank in an inner helper while an
outer measurement relies on the same counters. Any exclusive accounting must
track nesting and subtract only compatible same-thread child intervals.

The stress pipeline measures four separate stage spans with cycle reads. Its
accumulated stage values include read-boundary overhead. Its worker-wide cycle
total also contains queue activity, markers, and other worker work, so it need
not equal the sum of stage spans.

## Thread synchronization

For exact self-versus-specific equality:

1. Configure parked live workers.
2. Start their counters before dispatch.
3. Let each worker stop its own counters and save all seven values.
4. Publish completion using proper release/acquire or native synchronization.
5. Keep workers alive and stopped while the coordinator reads them.
6. Compare corresponding counter IDs, then release the workers.

Do not use counter reads as a substitute for memory publication. Do not issue
another ALL start after dispatch: a worker that completed early could have
its counters restarted. Changing/resetting another target while it runs can
also invalidate its local interpretation.

ALL is a sequential snapshot broadcast. A vanished temporary target can stop
it partway through. For consistent application configuration, control thread
lifetime and park targets. For churn testing, retain and classify the error,
then independently verify persistent targets.

## Core affinity and workload fairness

User-core affinity masks are `0x10000`, `0x20000`, and `0x40000`, expressed
through `SCE_KERNEL_CPU_MASK_USER_0 << core`. Save and restore affinity when
temporarily changing it in a general application. Migration tests deliberately
move threads; isolated comparisons normally pin them.

Pinning reduces one source of variation but does not reserve cache, memory
bandwidth, or the core exclusively. It also does not guarantee a shared queue
distributes work evenly. The stress coordinator runs on core 0 and performs
scalar verification, so its worker can receive fewer shared jobs. Guaranteed
private work is the stress test's fairness mechanism; job proportions are not
core-utilization measurements.

Do not add concurrent workers' wall-time totals to estimate run duration:
their intervals overlap. A coordinator timestamp around the whole run is the
appropriate source for end-to-end elapsed time when needed.

## Controlled function comparisons

The profiler's branch pair performs equal input-load and decision counts while
changing predictability. Its float pair performs the same element-wise output
work with scalar/VFP and explicit NEON implementations, checked for numerical
agreement. These are more controlled comparisons than unrelated workloads.

The 4 KiB versus 8 MiB memory functions intentionally change working-set size
and access pattern. `cache_cold` means the large set; it does not imply a
privileged cache flush. Warm-up does not make the entire 8 MiB set L1-resident.
Do not infer cache geometry or exact bandwidth solely from the comparison.

Retain compiler optimization settings and inspect generated code when
comparing instruction counts. Prevent unused-work elimination without turning
the workload into a different artificial task. Auto-vectorization settings,
inline assembly, `noinline`, and volatile sinks are purposeful in the examples.

## Event validation limits

The retail survey accepts all 56 listed selectors and observed responses for
44 across its mixed, targeted, and pressure workloads. A response is evidence
that something counted, not proof that all count semantics are exact.

DMB count `0x92` and DMB stall `0x86` remained zero in tested Vita and
Cortex-A9 Linux workloads. Keep them unverified; do not substitute another
event or fabricate counts. Optional Jazelle/PLE events can also remain zero.

Cortex-A9 erratum 740661 affects NEON rename counting on r1/r2 revisions and
is fixed in r3p0. A stalled NEON instruction can be counted more than once.
Matching a particular Vita known-count loop does not establish its CPU
revision or universal accuracy. Do not derive an exact NEON instruction count
from `0x74` without the necessary revision and workload evidence.

## Comparing with another Cortex-A9 system

Match scope, compiled operations, and input sizes before comparing counts.
On Linux, use raw PMU events, pin to a CPU with PMU support, and record both
`time_enabled` and `time_running` to detect disabled or multiplexed groups.
Record whether the driver accepts privilege filters. A user-only Linux group
is not directly equivalent to a Vita interval including native call effects.

For known-count tests, compare identical assembly instruction bodies and
incremental deltas between iteration counts. For memory events, equivalent
allocated byte footprints do not ensure equivalent page mappings, TLB reach,
cache hierarchy, frequency, or contention. Prefer qualitative validation to
an invented expectation of identical cache-miss totals.
