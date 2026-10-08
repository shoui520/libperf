# Using libperf in an application

This is the entry point for agents integrating libperf into an application or
analyzing its profiling results. The [developer guide](guide.md) provides the
application setup and usage walkthrough.

The documents describe interfaces, runtime behavior, measurement methods, and
capture formats. Use existing compatible headers, stubs, and modules when
available. Reading these documents does not add library maintenance, binary
readbacks, device configuration changes, SDK installation, or hardware test
runs to the application's task.

## Read by task

| Application task | Documentation |
| --- | --- |
| Link, package, and load the user module | [Integration](agents/integration.md) |
| Call the profiling API and handle errors | [API contract](agents/api-contract.md) |
| Understand thread ownership and native PMU behavior | [Architecture](agents/architecture.md) |
| Instrument functions and interpret costs | [Measurement](agents/measurement.md) |
| Understand the bundled workloads and their results | [Examples and validation](agents/examples-and-validation.md) |
| Read captures or consume analyzer JSON | [Capture formats](agents/capture-formats.md) |

## Facts to retain across tasks

1. Six configurable counters use IDs `0..5`; the dedicated cycle counter uses
   ID `31`. Event code `0x11` and counter ID `31` are different concepts.
2. `SELF` is `0`. The process-wide ALL constant is `0x10027`; `-1` is also
   accepted for broadcasts. Counter reads reject both ALL forms.
3. PMU state belongs to threads and is carried by native scheduling machinery.
   A process broadcast enumerates existing thread GUIDs and acts sequentially.
   It is neither atomic nor a rule that applies future event selections.
4. The user module opens PMU access on start and closes it on stop. Applications
   that load it at runtime use generated weak imports and check module ID and
   start status before calling the API.
5. Closing PMU access while keeping the module loaded is different from
   unloading it. Calling an unlinked weak import can crash. Lifecycle tests
   retain the module and use the kernel open/close exports.
6. The native retail timebase is 48 bits and runs at `1` MHz. Frequency is
   reported in MHz, not Hz; it is independent of PMU enablement and CPU speed.
7. Six guarded counter APIs return `0x80580005` when the caller lacks PMU user
   access. Event selection uses the kernel path without that same guard.
8. Event acceptance does not prove event availability or numerical accuracy.
   DMB event/stall counts remain unverified in tested workloads. Optional
   Jazelle/PLE events are not a reason to touch unavailable hardware.
9. Function CSV metrics are baseline-subtracted, clamped-at-zero batch medians
   divided by calls. Banks are separate passes. Stress stage averages have
   different measurement boundaries and include overhead and contention.
10. Analyzer envelope status and recorded stress status serve different
    purposes. Successful parsing of a failed test is not a test pass.
