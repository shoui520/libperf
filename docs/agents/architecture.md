# Retail implementation architecture

## Layers

```text
Application / example
    generated weak ScePerf imports
            |
    libperf.suprx (ScePerf)
       | self register access, timer, validation
       | kernel syscall imports for native control
            |
    libperf.skprx (SceKernelPerf)
       | process initial state and user access
       | PUID-to-GUID translation / process ALL enumeration
            |
    native ProcessMgr + ThreadMgr PMU context management
            |
    Cortex-A9 PMU on the executing user core
```

This design uses the retail kernel's process/thread PMU machinery to carry
state across scheduling and migration. It does not map a separate TOOL
performance peripheral or implement a second scheduler context-switch hook.

Source ownership:

- [`src/kernel.c`](../../src/kernel.c): export resolution, signature-checked
  injections, process open/close, native counter control, ALL dispatch, cleanup.
- [`user/src/user.c`](../../user/src/user.c): public validation, SELF CP15
  access, specific-thread reads, native timer, user-module lifecycle.
- [`exports.yml`](../../exports.yml): seven syscall exports in `SceKernelPerf`.
- [`user/exports.yml`](../../user/exports.yml): nine `ScePerf` application exports.
- [`include/libperf.h`](../../include/libperf.h): project constants and API.

## Kernel initialization

`module_start()` resolves nine native functions and creates a `0x10000`-byte
heap named `RetailPerfThreadList`. It then injects a two-byte Thumb NOP at one
verified branch in each of the native initial-PMU-state setters. Only after
all steps succeed does it return `SCE_KERNEL_START_SUCCESS`.

| Native module | Library NID | Function NID | Local function pointer |
| --- | --- | --- | --- |
| `SceProcessmgr` | `746EC971` | `61B9B6FA` | `sceKernelSetInitialPMCR` |
| `SceProcessmgr` | `746EC971` | `B1C3EFCA` | `sceKernelSetInitialPMUSERENR` |
| `SceProcessmgr` | `746EC971` | `6599E5D9` | `setProcessUserEnable` |
| `SceKernelThreadMgr` | `E2C40624` | `1AAFA818` | `sceKernelPMonSetControlRegister` |
| `SceKernelThreadMgr` | `E2C40624` | `5053B005` | `sceKernelPMonSetUserEnableRegister` |
| `SceKernelThreadMgr` | `7F8593BA` | `7F831213` | `sceKernelPMonThreadSetEnableCounter` |
| `SceKernelThreadMgr` | `7F8593BA` | `1D2A6815` | `sceKernelPMonThreadClearEnableCounter` |
| `SceKernelThreadMgr` | `7F8593BA` | `7B3368F1` | `sceKernelPMonThreadSetCounter` |
| `SceKernelThreadMgr` | `7F8593BA` | `FFB9CD24` | `sceKernelPMonThreadSetEvent` |

Numbers in this table are hexadecimal and refer to the current 3.65 CEX path.
Resolution uses `module_get_export_func()` for `KERNEL_PID`; a failed lookup or
null pointer takes the shared failure path. Keep module/library distinctions
intact: two native ThreadMgr libraries are involved.

The kernel module does not open PMU access for an arbitrary application at boot.
The application's user module opens it in that application's process context.

## Signature-checked initial-state gates

`patchInitialStateGate()` strips the Thumb bit from the resolved function
address and checks these bytes before writing anything:

| Offset from native function | Required bytes | Purpose |
| --- | --- | --- |
| `0x00` | `70 B5` | Expected function prologue |
| `0x0E` | `E4 20` | Expected immediate for the debug-switch check |
| `0x16` | Function-specific four-byte call encoding | Expected call site |
| `0x1A` | `90 B1` | Expected conditional branch being removed |

For initial PMUSERENR the call encoding is `03 F0 42 E8`. For initial PMCR it
is `02 F0 F2 EF`. The injection replaces only the two bytes at `0x1A` with
`00 BF`, retaining the call and surrounding validation. This removes the gate
following the `0xE4` debug-switch test in the known functions.

These are partial code signatures, not a universal firmware classifier. Do
not relax a mismatch or search blindly for a similar branch to claim support
for another firmware. Resolve and analyze that firmware's actual semantics.

Injection IDs start at `-1`. `releaseResources()` releases injections in
reverse order, resets their IDs, then deletes the thread-list heap and resets
its ID. Kernel start failure always uses this cleanup. Kernel module stop uses
the same resource release, but is not a process-wide PMU-close operation.

## Opening a process

`sceKernelPerfArmPmonOpen()` temporarily raises the calling thread's native
permission using `ksceKernelSetPermission(0x80)`. On success it performs these
operations, with process ID argument `0` denoting the current process:

1. Set initial PMCR to `0x11` for subsequently created threads.
2. Set current process/thread control state with native PMCR value `0x11`.
3. Set initial PMUSERENR to `1` for subsequently created threads.
4. Set existing native user-enable register state to `1`.
5. Set process user-enable policy to `1`.

`0x11` supplies the chosen PMCR baseline, including global enable and event
export; it does not request the divide-by-64 mode. Per-counter enablement is
still controlled by start/stop. Opening is not a request to choose six events,
reset every existing counter value, or persist an ALL event-selection policy.

Failure in the setter sequence invokes `disableProcessPmon()` as best-effort
rollback. The old permission is restored after the operation. A setter failure
takes precedence over permission-restoration failure; otherwise a restoration
error is returned. If the initial permission change fails, the function returns
immediately without executing the setter sequence.

## Closing and rollback

`sceKernelPerfArmPmonClose()` uses the same temporary permission and restoration
pattern. `disableProcessPmon()` attempts every operation in this order:

1. Clear process user-enable policy.
2. Clear initial PMUSERENR.
3. Clear native user-enable register state.
4. Clear initial PMCR.
5. Clear native control-register state.

It retains the first negative error while still attempting the remaining
disables. Do not change rollback into a sequence that stops after its first
failure; that would leave more process access state behind.

There is no reference count or independent per-client lease in open/close.
Coordinate ownership inside the application. Park active users before closing
and reconfigure counters as needed after reopening. Native state changes and
future-thread defaults are not equivalent to a library-wide scoped profiler.

## User-module lifecycle

The user module's `module_start()` calls kernel open. It returns module-start
failure on a negative result. Its `module_stop()` calls kernel close and
reports stop failure if closing fails.

The public guarded APIs inspect `sceKernelGetPMUSERENR()` before accessing PMU
registers. This avoids attempting privileged CP15 operations when the caller
does not have user access. Selection follows the native kernel path and does
not share that guard. Timer functions use the native timer and are independent
of PMU access.

Weak-import linkage is separate from user-enable state. A module can remain
linked while its process PMU access is closed, allowing closed-state API tests.
An unloaded module can leave weak imports unlinked. Do not call those imports
to test the closed state.

## SELF register operations

The user implementation accesses the architectural PMU through CP15. Each
inline assembly block is `volatile` and uses a `memory` clobber. These are
compiler ordering constraints; they do not promise a new architectural barrier
sequence around every measurement.

| Register | CP15 encoding used | Operation |
| --- | --- | --- |
| PMCR | `p15,0,c9,c12,0` | Read/modify/write reset bits |
| PMCNTENSET | `p15,0,c9,c12,1` | Start mask `0x8000003F` |
| PMCNTENCLR | `p15,0,c9,c12,2` | Stop mask `0x8000003F` |
| PMSWINC | `p15,0,c9,c12,4` | Software increment mask |
| PMSELR | `p15,0,c9,c12,5` | Choose configurable counter for get/set |
| PMCCNTR | `p15,0,c9,c13,0` | Dedicated cycle-counter get/set |
| PMXEVCNTR | `p15,0,c9,c13,2` | Selected event-counter get/set |

Event selection itself uses the native kernel helper rather than a direct
PMXEVTYPER write in the user wrapper. Keep this distinction when reasoning
about a register-only emulator or self-versus-specific behavior.

## Specific-thread operations

Kernel start/stop/reset/select/set accept SELF, a specific PUID, or either ALL
form. For a specific ID they call `kscePUIDtoGUID(0, threadId)` before invoking
the resolved native ThreadMgr operation. Failed conversion returns directly.
SELF `0` is passed to the native operation without PUID conversion when used
in a kernel control path.

Specific get is handled by the user module's imported
`sceKernelPMonThreadGetCounter()`. The local kernel plugin does not export a
separate get syscall. This distinction matters when changing imports or mocks.

Native context handling is responsible for accessing running or saved target
state. Do not replace it with an assumption that the caller is executing on
the target's last core. Thread-scoped counters must survive migration.

## ALL enumeration and action dispatch

`armPmonExecAllThread()` uses `ksceKernelGetProcessId()` and
`ksceKernelGetThreadIdList()` for the calling process. Its contract is a
byte-sized output capacity with native GUID entries and a copied count.

The enumeration algorithm is:

1. Query the current count with a null buffer.
2. Return the native error if negative, or success if there are no threads.
3. Attempt enumeration up to four times.
4. Reject a requested count above `65520` with `0x8002710B`.
5. Allocate `(count + 16) * sizeof(SceUID)` bytes from the private kernel heap.
6. Ask the native function for IDs with **byte capacity**, not entry capacity.
7. Reject native enumeration failure or copied counts outside `0..capacity`.
8. If the reported total exceeds capacity, free the list, update the requested
   count, and retry.
9. Apply the requested action to each copied GUID sequentially.
10. Stop at the first nonzero action result, free the list, and return it.

Allocation failure and an oversized count return `0x8002710B`; malformed native
counts or exhausted retries return `0x80020005`. These are native/internal
errors, not extra public libperf error constants.

Actions map to reset-all-seven, select-one-event, enable-all-seven,
disable-all-seven, or set-one-counter. Enumeration GUIDs are already in the
native form; do not convert them as application PUIDs a second time.

Thread deletion can race the action loop after successful enumeration.
Earlier actions remain applied when a later target vanishes. An error such as
`0x80028021`, or an error mapped to `0x80580000` by a user wrapper, can therefore
represent a partial broadcast. General application code must not suppress all
invalid-argument errors as harmless churn.

## Constraints for architecture changes

- Keep the user-access guard ahead of direct CP15 operations.
- Keep native saved-thread state handling and explicit process targeting.
- Preserve byte-capacity enumeration, bounded retries, and all allocation frees.
- Keep open/close permission restoration and failure cleanup.
- Do not remove signature checks to broaden apparent firmware compatibility.
- Do not add MMIO access to a TOOL-only peripheral; retail timing already has
  a native, validated timer path.
- A binary build or emulator pass cannot establish scheduler behavior on real
  hardware. Validate context migration, parked snapshots, and existing/future
  thread cases after changing these paths.
