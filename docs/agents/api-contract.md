# Public API contract

## Scope and source map

The application interface is declared in
[`include/libperf.h`](../../include/libperf.h) and implemented in
[`user/src/user.c`](../../user/src/user.c). Kernel-side control and process
broadcasts are in [`src/kernel.c`](../../src/kernel.c). The user export table is
[`user/exports.yml`](../../user/exports.yml). The ABI identifiers are listed
below for application linkage.

Nine application functions are implemented. There are no public trace-buffer,
Razor marker, capture-trigger, activity-monitor, or overflow-interrupt APIs.
The kernel open/close functions are integration and test facilities, not extra
functions in the public header.

## Types and constants

| Item | Value or meaning |
| --- | --- |
| `SceUID` | Signed application thread/module identifier |
| `SceUInt32` | Counter value, software mask, event-counter index |
| `SceUInt8` | Event selector argument; conversion to this type happens before validation |
| `SceUInt64` | Timebase storage; only the low 48 bits are returned |
| `SCE_PERF_ARM_PMON_THREAD_ID_SELF` | `0`, supplied through the included VitaSDK header |
| `SCE_PERF_ARM_PMON_THREAD_ID_ALL` | `(SceUID)0x10027`, declared by this project |
| `-1` | Additional supported process-ALL alias for applicable operations |
| `SCE_PERF_ARM_PMON_PMCNT_NUM` | `6` |
| `SCE_PERF_ARM_PMON_COUNTER_0` through `_5` | `0` through `5` |
| `SCE_PERF_ARM_PMON_COUNTER_MASK_0` through `_5` | `1U << counter` |
| `SCE_PERF_ARM_PMON_COUNTER_MASK_ALL` | `0x3F` |
| `SCE_PERF_ARM_PMON_CYCLE_COUNTER` | `31U` |
| `SCE_PERF_ARM_PMON_COUNTER_CYCLE` | Equivalent cycle-counter ID `31` |
| `SCE_PERF_ERROR_INVALID_ARGUMENT` | Signed `0x80580000` |
| `SCE_PERF_ERROR_NOT_INITIALIZED` | Signed `0x80580005` |

Do not introduce a second local definition of SDK event names or thread SELF
unless needed for a deliberately supported SDK compatibility layer. Use the
project header so all clients agree on ALL, counter masks, and error constants.

## Thread targeting

| Operation | SELF | Specific live application thread | ALL (`0x10027` or `-1`) |
| --- | --- | --- | --- |
| Reset | Yes | Yes | Yes |
| Select event | Yes | Yes | Yes |
| Start / stop | Yes | Yes | Yes |
| Set counter | Yes | Yes | Yes |
| Get counter | Yes | Yes | Rejected |
| Software increment | Caller only; no thread argument | No | No |
| Timebase / frequency | Global timer; no thread argument | Not applicable | Not applicable |

Specific IDs are application PUIDs, converted to kernel GUIDs in the kernel
control path. Do not pass a kernel GUID obtained from internal enumeration into
the application API. Native thread operations provide the access checks and
saved-context handling; the library is not an API for profiling arbitrary
unrelated processes.

ALL enumerates the calling process's existing threads. It has no persistent
event policy for threads created later. A broadcast may fail after earlier
targets have changed. Reset itself writes seven counters sequentially in the
native path, so it is not an atomic multi-counter snapshot either.

## Return values and validation order

For `int` APIs, accept a nonnegative result as success and treat negative values
as errors. Most successful control calls return zero. Get writes its result
through `*value`; its return value is a status, not the counter's contents.
Do not consume the output after an error.

Start, stop, reset, get, set, and software increment first inspect the caller's
PMU user-access state through `sceKernelGetPMUSERENR()`. If it is disabled, they
return `SCE_PERF_ERROR_NOT_INITIALIZED` before validating the other arguments.
For example, a closed-PMU call with an invalid software mask returns the
closed-state error first.

Select validates the counter and event, then calls the kernel. It intentionally
has no matching user-access guard. Selecting a valid event while PMU access is
closed can succeed; it does not enable measurement.

Native error `0x80024501` is translated to `SCE_PERF_ERROR_INVALID_ARGUMENT` by
start, stop, reset, select, and get. The set wrapper forwards its kernel return
without this translation. Other native errors may pass through unchanged.

## Reset

```c
int scePerfArmPmonReset(SceUID threadId);
```

Resets all six event counter values and the dedicated cycle counter for each
target. It does not select new events or provide an independent measurement
session. The SELF path reads PMCR, sets its event/cycle reset bits (`0x6`), and
writes it back. Specific/ALL paths set each counter value to zero through native
ThreadMgr helpers.

Stop a target before resetting when you require stable zeroes. A running target
can accumulate new counts immediately, including between sequential native
counter writes. Do not interpret a reset as an implicit stop.

## Select event

```c
int scePerfArmPmonSelectEvent(SceUID threadId, SceUInt32 counter,
			    SceUInt8 eventCode);
```

`counter` must be `0..5`; `31` is invalid for event selection. `eventCode` must
be in the explicit allowlist below. Selection always uses the native kernel
path, including SELF. It does not reset the counter value or start the counter.
Changing a running counter's event can combine counts with different meanings.

The event argument is eight bits. Passing a wider integer can truncate before
the wrapper sees it; callers should validate externally supplied integers
before conversion instead of relying on the wrapper to detect lost high bits.

### Accepted event codes

All codes in this table are hexadecimal. These are the 56 accepted selectors;
the short names follow the event survey and are identifiers, not claims of
exact source-level instruction semantics.

| Code | Event name | Code | Event name |
| --- | --- | --- | --- |
| `00` | Software increment | `01` | I-cache miss |
| `02` | Instruction micro-TLB miss | `03` | D-cache miss |
| `04` | D-cache access | `05` | Data micro-TLB miss |
| `06` | Data read | `07` | Data write |
| `09` | Exception taken | `0A` | Exception return |
| `0B` | Context-ID write | `0C` | Software change of PC |
| `0D` | Immediate branch | `0F` | Unaligned access |
| `10` | Branch misprediction | `11` | Cycle count in an event slot |
| `12` | Predictable branch | `40` | Jazelle bytecode |
| `41` | Jazelle software | `42` | Jazelle backward branch |
| `50` | Coherent linefill miss | `51` | Coherent linefill hit |
| `60` | I-cache-dependent stall | `61` | D-cache-dependent stall |
| `62` | Main-TLB-dependent stall | `63` | STREX passed |
| `64` | STREX failed | `65` | Data eviction |
| `66` | Issue without dispatch | `67` | Empty issue stage |
| `68` | Instruction rename | `6E` | Predictable function return |
| `70` | Main execution pipe | `71` | Second execution pipe |
| `72` | Load/store pipe | `73` | VFP rename |
| `74` | NEON rename | `80` | PLD-dependent stall |
| `81` | Write-dependent stall | `82` | Instruction main-TLB stall |
| `83` | Data main-TLB stall | `84` | Instruction micro-TLB stall |
| `85` | Data micro-TLB stall | `86` | DMB-dependent stall |
| `8A` | Integer clock activity | `8B` | Data-engine clock activity |
| `90` | ISB | `91` | DSB |
| `92` | DMB | `93` | External interrupt |
| `A0` | PLE line request completed | `A1` | PLE channel skipped |
| `A2` | PLE FIFO flush | `A3` | PLE request completed |
| `A4` | PLE FIFO overflow | `A5` | PLE request programmed |

The header imports many event constants from VitaSDK. Numeric selectors are
useful for accepted codes that an SDK version does not name. Selection accepts
only the codes in the table.

`08`, `0E`, and holes elsewhere are rejected. The interface does not provide
a conventional "instructions retired" selector borrowed from another processor.
An accepted optional-engine event
can remain zero. Do not touch optional hardware to make a survey row nonzero.

## Start and stop

```c
int scePerfArmPmonStart(SceUID threadId);
int scePerfArmPmonStop(SceUID threadId);
```

Start enables all six event counters and the cycle counter using mask
`0x8000003F`. Stop clears the same enable bits. Start does not reset accumulated
values; stop does not erase them. Repeated start/stop calls are not reference
counted, and the interface has no subset-enable argument.

SELF uses direct PM enable-set/enable-clear register writes. Other targets use
native ThreadMgr enable operations. Stop is the correct boundary before stable
counter reads. Repeating ALL start after some workers have finished can restart
their supposedly parked counters and invalidate snapshot comparisons.

## Get and set values

```c
int scePerfArmPmonGetCounterValue(SceUID threadId, SceUInt32 counter,
				SceUInt32 *value);
int scePerfArmPmonSetCounterValue(SceUID threadId, SceUInt32 counter,
				SceUInt32 value);
```

Valid counter IDs are `0..5` and `31`. Get rejects a null pointer and both ALL
IDs. A non-null pointer still needs to refer to writable application memory;
the null check is not a general memory-safety validator.

SELF accesses the appropriate event counter or PMCCNTR register. Specific-thread
get uses `sceKernelPMonThreadGetCounter()`. Specific/ALL set uses the kernel
control export. Values are 32-bit unsigned and wrap modulo `2^32`; there is no
overflow count, automatic widening, or overflow callback.

For a live target, reads are observations at the call boundary. They are not a
cross-thread synchronization primitive. Have the target stop and remain alive
to get repeatable self-versus-coordinator equality.

## Software increment

```c
int scePerfArmPmonSoftwareIncrement(SceUInt32 mask);
```

Bits `0..5` address the calling thread's six event counters. Valid masks are
`0x00..0x3F`; the zero mask is a successful no-op when PMU access is open. Any
bit outside that mask is rejected. There is no cycle-counter bit.

To count a marker, configure the addressed slot for event `0x00` and enable
counting. An increment against a stopped software counter does not accumulate.
No target argument exists, so the coordinator cannot inject a software marker
into another worker's counter. Emit the marker from that worker.

## Timebase and frequency

```c
SceUInt64 scePerfGetTimebaseValue(void);
SceUInt32 scePerfGetTimebaseFrequency(void);
```

Value is `sceKernelGetSystemTimeWide() & 0xFFFFFFFFFFFFULL`. Frequency returns
`1` in MHz. Neither function requires PMU user access; both remain usable while
PMU access is closed and the module remains loaded. Their return types contain
values, not negative status codes.

Use `(after - before) & 0xFFFFFFFFFFFFULL` for a single-wrap delta. Divide ticks
by MHz for microseconds and by `MHz * 1,000,000` for seconds. The timer wraps
after about 8.9 years at 1 MHz, but its width must still be preserved in formats
and arithmetic.

## ABI identifiers

`ScePerf` library NID is `0x447F047D`, version 1.0.

| Function | NID |
| --- | --- |
| `scePerfArmPmonReset` | `0x35151735` |
| `scePerfArmPmonSelectEvent` | `0x63CBEA8B` |
| `scePerfArmPmonStart` | `0xC9D969D5` |
| `scePerfArmPmonStop` | `0xD1A40F54` |
| `scePerfArmPmonGetCounterValue` | `0x6132A497` |
| `scePerfArmPmonSetCounterValue` | `0x12F6C708` |
| `scePerfArmPmonSoftwareIncrement` | `0x4264B4E7` |
| `scePerfGetTimebaseValue` | `0xBD9615E5` |
| `scePerfGetTimebaseFrequency` | `0x78EA4FFB` |

Application imports use these names, parameter widths, calling convention,
and export identifiers.
