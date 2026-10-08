# libperf agent documentation

This is the entry point for agents working on libperf. Read the operating rules
below, then follow the topic links for the task at hand. The human-facing
[developer guide](guide.md) gives the shortest application integration path.

These documents describe the current implementation. Check the linked source
before changing behavior; update the affected documentation when an interface,
measurement boundary, build output, or capture format changes. Separate
implemented behavior, observed hardware results, and unverified assumptions.

## Operating rules

- Keep CPU profiling available on retail/CEX. The validated platform is 3.65
  with taiHEN; other firmware versions need explicit compatibility work.
- Preserve all nine public counter/timebase functions and their exported NIDs.
  Razor functionality and interrupt-driven sampling are outside the project
  scope unless the user explicitly changes that scope.
- Use native process/thread PMU context handling. Do not introduce TOOL-only
  Perfmon MMIO, optional engines, privileged cache manipulation, or raw per-core
  register broadcasts as a shortcut for thread-scoped behavior.
- Keep supplied firmware, disassembly checkouts, and other read-only research
  inputs unchanged. Analyze them into a private workspace instead.
- Keep private captures, device configuration, identities, credentials, host
  paths, logs, temporary checks, and agent workflow scripts in `agent/`. Add
  `/agent/` to `.git/info/exclude`. Public documentation and the shipping
  `tools/analyze.py` are deliberate repository features.
- Public files must not contain personal host paths, device addresses,
  credentials, configuration dumps, proprietary document text, or firmware
  binaries. Use generic commands and repository-relative source links.
- Preserve upstream commit ancestry, attribution, and the MIT license. Do not
  rewrite upstream history to make the fork appear newly authored. Follow the
  user's current commit identity and publishing instructions; do not infer
  permission to commit or push from a request to edit files.
- Confine searches to plausible project/input directories. Do not run
  filesystem-wide `find`, `rg`, or process searches.
- Follow `.clang-format` for new C code. Use tabs, Linux function braces, and
  camelCase/PascalCase local function names. Keep public API names and SDK names
  intact. Avoid unrelated formatting of unchanged upstream code.
- Read the actual code before making changes. Tests must exercise a meaningful
  contract or failure mode, not merely repeat implementation details.

## Read by task

| Task | Read first | Main source |
| --- | --- | --- |
| Build, package, or load libperf | [Integration](agents/integration.md) | Root/user/example CMake files |
| Change an API or fix a return value | [API contract](agents/api-contract.md) | `include/libperf.h`, `user/src/user.c` |
| Work on kernel state or ALL broadcasts | [Architecture](agents/architecture.md) | `src/kernel.c` |
| Add function profiling or interpret numbers | [Measurement](agents/measurement.md) | `examples/profiler/` |
| Modify or extend tests | [Examples and validation](agents/examples-and-validation.md) | `examples/pmu-test/`, `examples/stress/` |
| Change analyzer output or implement a reader | [Capture formats](agents/capture-formats.md) | `tools/analyze.py` |

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
   access. Event selection uses the kernel path without that same guard;
   do not impose a fictional uniform closed-state contract.
8. Event acceptance does not prove event availability or numerical accuracy.
   DMB event/stall counts remain unverified in tested workloads. Optional
   Jazelle/PLE events are not a reason to touch unavailable hardware.
9. Function CSV metrics are baseline-subtracted, clamped-at-zero batch medians
   divided by calls. Banks are separate passes. Stress stage averages have
   different measurement boundaries and include overhead and contention.
10. Analyzer envelope status and recorded stress status serve different
    purposes. Successful parsing of a failed test is not a test pass.

## Validation expectations

For documentation, verify links, source names, output names, formulas, and
compilable C examples. For behavior changes, choose the relevant host checks
and hardware tests described in [examples and validation](agents/examples-and-validation.md).
Host success cannot establish real PMU counting. Preserve raw hardware results
and record firmware, artifact identity, scope, exact errors, and incomplete
evidence in the private workspace.

Do not deploy or reboot merely to finish a documentation task. Device work must
follow the user's authorized scope and the installed device tooling's workflow.
