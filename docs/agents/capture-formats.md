# Capture formats and desktop analysis

## Supported producers and entry point

[`tools/analyze.py`](../../tools/analyze.py) accepts function-profiler CSVs and
three-core stress logs. It prints a static terminal report by default or emits
JSON with `--json`. It is a standard-library Python 3.8+ program, not an
interactive application or an automatic device downloader.

The PMU correctness/event-survey application's text report is a different
diagnostic format and is not accepted by this analyzer. Do not infer format
from filename extension alone; the analyzer detects the function header and
otherwise attempts stress parsing.

## CLI contract

```text
analyze.py captures... [--json | --tui] [--bank BANK] [--sort {cycles,wall}]
```

| Argument | Behavior |
| --- | --- |
| One or more capture paths | Parse independently, preserve input order |
| `-` | Read one capture from stdin; allowed at most once |
| `--tui` | Explicitly request the default printed terminal report |
| `--json` | Emit all analysis data and samples as schema version 1 |
| `--bank N` | Show only that function bank in terminal output; validate its presence in each function capture |
| `--sort cycles` | Rank functions and scale bars by cycles per call |
| `--sort wall` | Rank functions and scale bars by wall microseconds per call |

`--bank` and nondefault `--sort` are terminal presentation options and are
rejected with `--json`. Stress summaries are not bank-filtered by `--bank`.
JSON always includes all parsed banks; it does not reproduce terminal sorting
or truncate samples to save space.

```sh
python3 tools/analyze.py results.csv --tui --bank 0
python3 tools/analyze.py results.txt --json > analysis.json
python3 tools/analyze.py core0.csv core1.csv results.txt --json
python3 tools/analyze.py - --json < results.csv
```

Files are read as UTF-8 with optional BOM handling. An stdin BOM is also
removed. Function input must begin with its header; leading arbitrary text
is not a supported preamble. Stress input is split at line-start `START `
records and can contain several runs in one file.

## Function CSV format

The first line contains collection metadata:

```text
# CPU Function Profiler,core=0,clock_before_mhz=333,clock_after_mhz=333,repeats=7,calls_per_batch=8,timebase_mhz=1
```

Fields are integers. The required keys are `core`, `clock_before_mhz`,
`clock_after_mhz`, `repeats`, `calls_per_batch`, and `timebase_mhz`. Core must be
0, 1, or 2. Repeats, call count, and clock values must be positive. Duplicate
metadata keys are errors. The analyzer retains additional integer metadata.

The exact CSV column order is:

```csv
kind,bank,function,run,calls,cycles,us,event0,event1,event2,event3,event4,event5
```

Each bank has a comment defining its six slot names and hexadecimal codes:

```text
# bank0,event0=rename(0x68),event1=dcache_miss(0x03),event2=dtlb_miss(0x05),event3=branch_miss(0x10),event4=loads(0x06),event5=stores(0x07)
```

Slots must be ordered `event0` through `event5`, names must match the parser's
word-character pattern, and codes must contain one or two hex digits. Duplicate
bank definitions are errors. Other comment lines beginning `#` are skipped
by CSV parsing. Retain the metadata and bank comments in transferred files.

The record kinds are:

| Kind | Function | Run | Meaning |
| --- | --- | --- | --- |
| `baseline` | `empty` | `0..repeats-1` | Raw empty-call batch for a bank/repetition |
| `raw` | Workload name | `0..repeats-1` | Raw measured workload batch |
| `median_adjusted` | Workload name | `-1` | Reported median after matched subtraction and zero clamping |

All rows use the metadata's call count. `cycles`, `us`, and all six events are
nonnegative integer **batch** values. The current producer's timer is 1 MHz,
so its elapsed ticks are microseconds; a future producer with different timer
frequency must still honor the `us` column's unit or explicitly version the
format and reader.

A record key is `(kind, bank, function, run)`; duplicates are rejected. Every
function must have all repetitions and a reported median in every defined
bank. Each bank must have every baseline. With `B` banks, `F` functions, and
`R` repetitions, the number of data rows is:

```text
B * (R + F * (R + 1))
```

The bundled producer uses `B=2`, `F=8`, `R=7`: 14 baseline rows, 112 raw rows,
and 16 reported rows, totaling 142. The parser derives counts from metadata
and function names rather than hardcoding eight workloads.

## Function validation and derived values

For each metric, match a raw row to the baseline in the same bank and run,
subtract with a floor of zero, then take the median over repetitions. The
reported `median_adjusted` batch value must exactly equal that recomputed
median. A discrepancy is a capture-read error, not a warning to ignore.

The analyzer divides the median by calls to form per-call fields and retains
raw/adjusted batches. It also derives the min/max adjusted cycle samples,
span percentage, and the equal-call cycle share. Zero median/total denominators
produce JSON null instead of NaN or infinity.

Three named comparisons are generated when both functions exist and the
reference cost is nonzero: random/predictable branches, large/hot working set,
and scalar/NEON float. The ratio is the first function's cycles per call divided
by the reference's cycles per call, within the same bank. Additional function
names are otherwise accepted without inventing comparison semantics.

## Stress log format

An application-session preamble can precede the first START. Each run begins:

```text
START epochs=32 cores=3 jobs/epoch=192 sizeofQueue=648
```

These are the exact four start fields; values are nonnegative integers and
`cores` must be 3. The application uses `4294967295` for its practical
continuous-run epoch limit. `sizeofQueue` describes producer ABI, not a host
allocation requirement.

Each epoch can have the following records. Numbers below illustrate the
grammar; a complete file needs all records required for its completed epochs.

```text
epoch=0 BEGIN probe/wrap/migration/control
epoch=0 lifecycle: workers parked, closing PMU
worker,0,0,jobs=24,sw=96,cycles=2000000,us=100000
stages,0,0,generate=500000,neon=650000,rle=130000,validate=285000
events,0,0,c1=1200000,c2=25000,c3=5000,c4=4500,c5=100000
epoch=0 PASS jobs=192 churn=12 live_reads=300 broadcasts=100 vanished=0
```

Worker/stages/events keys are `(epoch, core)`. Core is `0..2`. Worker has
`jobs`, `sw`, `cycles`, and `us`; stages has `generate`, `neon`, `rle`, and
`validate`; events has `c1..c5`. Fields within these records are strict, values
are nonnegative, and duplicate keys are errors.

Stage values are accumulated cycle spans for that worker in that epoch.
Worker cycles cover the larger work interval; worker wall time includes its
waits and descheduling. Counter 0 is represented as `sw` in the worker record.
`c5` is retained as an unattributed raw counter because of deliberate live
reprogramming.

Epoch PASS fields contain per-epoch jobs but **cumulative** churn, live reads,
broadcasts, and vanished-thread observations. Cumulative activity must not
decrease. Completed PASS epoch IDs must be contiguous starting at zero.

The final records are:

```text
RESULT PASS completed_epochs=32 jobs=6144 transient_threads=384
API reset calls=100
API select calls=100
API start calls=100
API stop calls=100
API get calls=100
API set calls=100
API software calls=100
API time calls=100
API frequency calls=100
Report: ux0:data/libperf-stress/results.txt
```

The API values above are illustrative, not expected totals. Valid RESULT
statuses are `PASS`, `FAIL`, and `CANCELLED`. There is one RESULT per run and
at most one API entry per recognized API. `FAIL...` diagnostic lines are
preserved. Unknown structured records are rejected rather than silently skipped.

## Stress validation boundaries

For every completed epoch, the reader requires all three workers' worker,
stage, and event records. Jobs must sum to `jobs/epoch`, software counts must
equal four times each worker's jobs, and cycles/wall time must be positive.
The epoch PASS must report the same jobs and `(epoch + 1) * 12` churn count.
Required lifecycle records must exist for completed epochs divisible by four.

A RESULT must agree with completed epoch and job totals. Its transient-thread
count cannot be smaller than `completed_epochs * 12`. PASS additionally
requires no FAIL diagnostic, the requested finite epoch total, exactly the
completed transient-thread total, all API footer names, and no unmatched or
extra worker/stage/event records.

The analyzer does not independently replay every PMU assertion or checksum.
For example, it does not verify the current build's minimum 24 jobs per core,
exact expected API totals, or every event's magnitude. Those are producer or
additional validation concerns. Do not market parser success as independent
proof of the complete workload's correctness.

Without a RESULT, a valid complete-record prefix is marked `INCOMPLETE`, and
only epochs with PASS records enter cost summaries. A truncated malformed line
can instead cause a parsing error. FAIL/CANCELLED runs may have partial data
from an unfinished epoch; those data are excluded from aggregate cost rows.
An incomplete API footer is warned about except that a PASS run missing it is
rejected.

## Stress aggregation

Per-core stage cost is a job-weighted mean across completed epochs:

```text
stage_cycles_per_job[core,stage] = sum_epoch(stage_cycles) / sum_epoch(jobs)
```

Do not average epoch means without weighting by jobs. The reader retains
per-core stage totals, worker cycle/wall totals, and each completed worker
sample. Cores with no completed jobs have null per-job values.

Stable event-bank aggregation groups samples by `epoch % 3`, adds counts from
all cores for slots 1 through 4, and divides by jobs represented in that bank.
The bank definitions come from the known stress producer, not embedded event
headers. A changed producer bank requires a corresponding reader update.

Lifecycle entries and the last completed epoch's cumulative activity are
retained separately. A failed partial epoch may have additional lifecycle or
churn activity that is not part of completed cost aggregation.

## JSON envelope

Successful analysis has this shape:

```json
{
  "schema": "libperf.analysis",
  "schema_version": 1,
  "status": "ok",
  "captures": [
    {"source": "results.csv", "runs": []}
  ]
}
```

`source` is the input basename or `stdin`, not a full host path. Different
directories can contain the same basename; use array order to associate them
with the supplied inputs. A function capture has one run. A stress capture
can have multiple runs. Envelope `ok` means parsing/analysis succeeded even
when a stress run says FAIL or INCOMPLETE.

Read/validation failure under `--json` emits:

```json
{
  "schema": "libperf.analysis",
  "schema_version": 1,
  "status": "error",
  "error": {"code": "capture_read_error", "message": "description"}
}
```

No partial successful captures are emitted in that error envelope. Error text
from a filesystem exception can contain the user-supplied path; treat generated
diagnostics as local evidence before sharing them publicly. CLI argument
errors are argparse errors on stderr, not capture-read JSON envelopes.

## Function-run JSON fields

| Field | Meaning |
| --- | --- |
| `kind` | `function` |
| `status` | `complete` after the complete capture passes validation |
| `metadata` | Header integer fields, including core, clocks, repeats, calls, timer frequency |
| `warnings` | Interpretation limitations, including bank separation and clock endpoints |
| `banks` | Bank objects ordered by bank ID |

Each bank object has:

- `bank`: integer ID.
- `events`: six objects with `slot` (`0..5`), `name`, and numeric event `code`.
- `baseline_batches`: one object per repetition with numeric `bank`, `run`,
  `calls`, `cycles`, `us`, and `event0..event5`.
- `functions`: function objects ordered by name, not terminal cost order.
- `comparisons`: objects with `function`, `reference`, and `cycle_cost_ratio`.

Each function object has:

| Field | Unit / interpretation |
| --- | --- |
| `name` | Producer's workload label |
| `cycles_per_call` | Median adjusted cycles / calls |
| `wall_us_per_call` | Median adjusted microseconds / calls |
| `cycle_min_per_call`, `cycle_max_per_call` | Adjusted cycle sample range / calls |
| `cycle_span_pct` | Range divided by median, percent; null at zero median |
| `events_per_call` | Six independently normalized medians, ordered by bank slot |
| `equal_call_cycle_share_pct` | Fraction of summed function cycles, percent; null at zero total |
| `raw_batches` | Numeric raw records, including bank/run/calls and batch metrics |
| `adjusted_batches` | Matching clamped batch metric objects in repetition order |

Adjusted objects contain only metric fields; repetition is their array index
and call count comes from metadata/raw batches. Raw objects retain the original
numeric identifiers. Strings `kind` and `function` are represented by the
containing objects rather than repeated inside each raw batch.

## Stress-run JSON fields

| Field | Meaning |
| --- | --- |
| `kind` | `stress` |
| `status` | Recorded `PASS`, `FAIL`, `CANCELLED`, or derived `INCOMPLETE` |
| `metadata` | START fields, including the literal `jobs/epoch` key |
| `result` | RESULT fields including status, or null when absent |
| `completed_epochs` | Count of contiguous epoch PASS records |
| `cores` | Three per-core summaries |
| `event_banks` | Three stable-event bank summaries |
| `worker_samples` | Completed samples ordered by core, then epoch |
| `api_calls` | Present API names mapped to integer wrapper-call counts |
| `last_epoch_activity` | Final completed PASS activity, or null |
| `lifecycle_epochs` | Recorded lifecycle epoch numbers, including any attempted partial epoch |
| `failures` | Original FAIL diagnostic lines |
| `warnings` | Scope, incomplete-footer, partial-epoch, and interpretation notes |

Each core has `core`, `jobs`, `worker_cycle_total`, `worker_wall_us_total`,
`stage_cycle_totals`, and `stage_cycles_per_job`. The two stage maps use
`generate`, `neon`, `rle`, and `validate`.

Each bank has `bank`, represented `jobs`, and four `events`. An event has `slot`
(`1..4`), `name`, numeric `code`, integer `total`, and normalized `per_job`.
The `slot` indexing differs from function bank JSON because stress slot 0 is
the separate software counter.

A worker sample has `epoch`, `core`, `jobs`, `sw`, `cycles`, `us`, `stage_cycles`,
`event_bank`, and raw `events` (`c1..c5`). API counts are attempts through the
instrumented wrappers, including deliberately failing validation calls; they
are not counts of successful native operations.

## Consumer and presentation rules

- Check schema/version and envelope status before using fields.
- Check each run's status separately, especially in automation.
- Preserve units, bank identity, normalization, and null values.
- Keep integer counters exact in consumers whose native number type loses
  integer precision above `2^53`; do not silently round future large totals.
- Never reinterpret rename counts as retired instructions or stress event
  totals as percentages of execution time.
- Keep uncertainty notes with exported or rendered metrics.
- Labels originate in files. Terminal rendering escapes control characters
  and truncates labels; JSON escapes strings but consumers must also escape
  them for HTML, terminal, or shell contexts.
- The terminal prints no cursor-control sequence, waits for no interaction,
  and falls back to representable characters for an ASCII output encoding.
- Exit `0` means analysis succeeded, exit `2` means a read/format/CLI error.
  A pipe closed by a downstream consumer is handled as normal termination.
- Version changes that alter a field's unit or meaning must be explicit.
  Do not silently turn medians into means or add fixed attribution to live
  counter 5 under the existing schema.
