# General PMU CSV format, version 1

This format describes measurements produced by any application using libperf.
It is an interchange format: writers and other tools can implement it without
using the bundled analyzer. libperf itself does not write files or name regions.

## A small complete capture

```csv
# libperf.csv,version=1
region,thread,core,calls,pmu_calls,wall_us,cycles,event.rename.0x68,event.dcache_miss.0x03
update,main,0,100,100,3200,950000,420000,3100
upload,worker,1,20,18,1400,300000,120000,900
update,main,0,100,100,3300,980000,430000,3200
```

Each row contains **interval totals** for a named region, measured on one thread
with one event configuration. It can represent one call or a batch of calls.
The two update rows aggregate to 9,650 cycles per measured call. Upload has
90% PMU coverage: its cycle average is 300,000 / 18; its wall average is
1,400 / 20. Counts and durations must correspond to the calls they describe.

```sh
python3 tools/analyze.py capture.csv
python3 tools/analyze.py capture.csv --sort wall
python3 tools/analyze.py capture.csv --json > analysis.json
```

The terminal output is a printed report, with no interactive controls. JSON
retains all rows, extra columns, event definitions and normalized summaries.

## Encoding and columns

Use UTF-8, optionally with a BOM, comma-separated fields and standard CSV
quoting. Header order does not matter. Names are case-sensitive. The first
line should be `# libperf.csv,version=1`; readers can also accept unmarked CSVs.
Additional leading `#` lines are opaque comments. Do not insert comments
among data records. Duplicate headers, missing fields and malformed numbers
are errors. Quote commas, quotes and newlines inside text fields normally.

Only `region` and at least one measurement column are required. Numbers are
nonnegative decimal integers; times are integer microseconds. Empty measurement
cells mean **unavailable**, whereas `0` means a measured zero. Do not write NaN,
floating-point values, sentinel negatives or separators inside numbers.

| Column | Meaning |
| --- | --- |
| `region` | Application-defined name of the measured function or region |
| `process`, `thread` | Optional stable identities, represented as text |
| `session` | Optional capture generation; change when restarting counters |
| `core` | Optional core identity; leave empty if unknown or migration occurred |
| `bank` | Optional event configuration identity, represented as text |
| `timestamp_us` | Optional interval endpoint relative to a consistent capture epoch |
| `calls` | Number of completed invocations in the row; positive, default 1 |
| `pmu_calls` | Invocations with valid PMU measurements; 0 through `calls`, default `calls` |
| `wall_us` | Sum of elapsed wall time across **all** calls |
| `cycles` | Sum of cycle deltas across **PMU-covered** calls |
| `exclusive_wall_us` | Wall time after subtracting measured nested regions |
| `exclusive_cycles` | Cycle deltas after subtracting measured nested regions |
| `max_us` | Largest inclusive duration of a single call, not a sum |
| `event.NAME.0xNN` | Sum of PMU event deltas; name plus explicit 8-bit selector |
| `exclusive_event.NAME.0xNN` | Corresponding event sum after subtracting children |

Event names are arbitrary. `event.NAME` is accepted when the selector is unknown;
readers must not infer a selector from a friendly name. Any number of event
columns can be represented, although the hardware has a limited number of
simultaneously programmable counters. Blank cells can indicate an event was
not measured in that row. Different selectors must use different columns;
use `bank` when collecting different configurations in separate passes.
Unknown columns are retained as text and are not silently treated as counters.
Use extra columns or comments to record clock readings, units of work, build
identifiers or collection conditions for other consumers.

Always supply `pmu_calls` if coverage can be partial. With zero coverage,
PMU cells must be empty or zero; summaries treat both as unavailable. With
partial coverage, every nonempty PMU column must cover those same `pmu_calls`.
If different events cover different subsets of invocations, write separate
rows/banks with accurate denominators instead of mixing them into one row.
A missing cell does not contribute to that metric's denominator.

## Measurement and aggregation semantics

Read before and after a region on the **same thread**; compute unsigned
32-bit counter differences before adding them into wider totals. Detect
counter resets, configuration changes and failed reads and omit those PMU
measurements. Logging an invalid delta as zero hides failures. Read wall time
independently so unsuccessful PMU calls can still contribute wall measurements.

Interval totals may exceed 32 bits. The analyzer never wraps or differences
them in its default mode. Grouping uses region, process, thread, session and
bank. Core identities are retained in each group. Input files are never merged.

A metric's mean is its summed total divided by the corresponding measured
calls, not a mean of per-row averages. Wall totals use `calls`; PMU totals use
`pmu_calls`. `max_us` aggregates by maximum and has no per-call mean. There is
no automatic baseline subtraction, median calculation, event multiplex scaling
or inference of CPU frequency. These operations require additional evidence.

Inclusive measurements contain child work. Exclusive measurements require a
writer to track nesting and subtract valid child measurements in the same
counter configuration. Both can be recorded together; the terminal report
shows inclusive cycles/wall and lists exclusive values separately. Summing
inclusive regions double-counts nested work. Wall time can include preemption
and waiting; cycles are not a direct conversion to elapsed time. Rankings do
not express CPU utilization.

Terminal ranking uses cycles per measured call, falling back to wall time
when no group has cycle measurements. `--sort wall` selects wall ranking
explicitly. If `pmu_calls` is absent, the report warns that full coverage is
assumed for present counts; it cannot verify that assumption.

## Reading existing CSV layouts

The analyzer also accepts tab- or semicolon-separated captures and recognizes
these aliases without requiring a version marker:

| Role | Accepted alternate headers |
| --- | --- |
| `region` | `label`, `function`, `name`, `scope` |
| `thread` | `thread_id`, `tid` |
| `process` | `process_id`, `pid` |
| `core` | `cpu` |
| `wall_us` | `duration_us`, `us` |
| `exclusive_wall_us` | `exclusive_us` |
| `cycles` | `cycle_count`, `cycle_delta` |
| `timestamp_us` | `elapsed_us` |

`elapsed_us` is an endpoint timestamp, not a duration. If a producer uses it
for durations, explicitly map it using `--column wall_us=elapsed_us` and map
the timestamp role to its actual column, or rename that input header first.
An ambiguous alias set is rejected instead of guessing.

Map other columns explicitly and identify arbitrary event columns:

```sh
python3 tools/analyze.py measurements.csv \
  --column region=operation --column wall_us=duration \
  --column cycles=cycle_total --event loads=0x06 --event misses
```

`--column FIELD=HEADER` can be repeated for any role listed in the table
above, or `reset`. `--event HEADER[=0xCODE]` can be repeated for any numeric
counter. An `exclusive_` prefix marks exclusive events. Unspecified event
codes remain unknown. Bare `event0`, `event1`, etc. are accepted as unknown-code
counters; a slot number does not establish the programmed selector.
These options apply to general CSVs, not the bundled examples' strict formats.
They change interpretation, so they apply equally to terminal and JSON output.
They do not repair corrupt records or invent absent measurements.

## Explicit cumulative snapshot conversion

Raw PMU snapshots are a different representation from the version 1 interval
format. They can be imported explicitly:

```csv
region,thread,core,session,timestamp_us,cycles,event.loads.0x06,reset
warmup,main,0,run1,100,4294967200,10,1
update,main,0,run1,200,104,40,0
```

```sh
python3 tools/analyze.py snapshots.csv --counter-mode cumulative --counter-bits 32
```

The first row is a baseline; the second attributes the interval to `update`
and yields 200 cycles and 30 loads. The snapshot values remain in JSON along
with derived deltas. Timestamp differences supply interval wall time. Each
interval counts as one observation, not an independently known function call.
A snapshot label attributes a span; it does not prove all work in that span
occurred inside that function.

Cumulative import requires timestamps and rejects `calls`, `pmu_calls`, wall
and maximum-duration columns, which describe a different kind of aggregation.
It supports 32- or 64-bit counters, assumes at most one wrap between adjacent
snapshots, and cannot distinguish an unmarked reset from a wrap. Set `reset=1`
on a new baseline or change `session`; other reset values must be `0`.
Snapshots are paired separately by process, thread, session, bank and core.
Do not combine migration-spanning readings unless the producer can establish
a continuous counter context. Changing selectors requires a new bank/session.

## JSON representation

The existing `libperf.analysis` schema version 1 envelope contains one capture
per input file. General CSVs add a `kind: "trace"` run with:

- `counter_mode` and `counter_bits`: interpretation and optional snapshot width.
- `columns`, `events`, `comments`: resolved roles, selector/scope definitions and preamble.
- `samples`: every normalized record, unknown fields in `extra`, and optional
  `baseline` and original `snapshots` when cumulative conversion applies.
- `regions`: grouping identities, observed cores, calls, PMU coverage and metric summaries.
- `metrics`: each field has `total`, its measured `calls`, contributing `rows`
  and `per_call`; `max_us.total` is a maximum and its `per_call` is null.
- `warnings`: limitations of the measurement interpretation.

Missing metrics are omitted from summaries, not populated with fabricated
zeros. Counts remain integer values in JSON; means and percentages are floating
point. Consumers using limited-width numeric types should preserve large
integer counts when importing JSON. Existing `function` and `stress` runs
retain their previous structure and validation rules. Successful parsing exits
0; malformed data exits 2, with structured error output under `--json`.
