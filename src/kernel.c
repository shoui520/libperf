#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/sysmem/uid_puid.h>
#include <psp2kern/kernel/sysroot.h>
#include <psp2kern/kernel/threadmgr.h>
#include <psp2kern/kernel/sysmem/heap.h>
#include <psp2kern/kernel/sysclib.h>
#include <stdint.h>

#include <taihen.h>

#include "libperf.h"

enum
{
    PMON_ACTION_RESET,
    PMON_ACTION_SET_EVENT,
    PMON_ACTION_START,
    PMON_ACTION_STOP,
    PMON_ACTION_SET_COUNTER
};

static SceUID injectIds[2] = {-1, -1};
static SceUID threadListHeap = -1;
static int (*setProcessUserEnable)(SceUID pid, SceUInt32 value);

static int (*sceKernelSetInitialPMCR)(SceUID pid, SceUInt32 initialPMCR);
static int (*sceKernelSetInitialPMUSERENR)(SceUID pid, SceUInt32 PMUSERENR);
static int (*sceKernelPMonSetControlRegister)(SceUID pid, SceUInt32 PMCR);
static int (*sceKernelPMonSetUserEnableRegister)(SceUID pid, SceUInt32 PMUSERENR);
static int (*sceKernelPMonThreadSetEnableCounter)(SceUID thid, SceUInt32 PMCNTENSET);
static int (*sceKernelPMonThreadClearEnableCounter)(SceUID thid, SceUInt32 PMCNTENCLR);
static int (*sceKernelPMonThreadSetCounter)(SceUID thid, SceUInt32 counter, SceUInt32 value);
static int (*sceKernelPMonThreadSetEvent)(SceUID thid, SceUInt32 counter, SceUInt32 event);

int module_get_export_func(SceUID pid, const char *modname, uint32_t libnid, uint32_t funcnid, uintptr_t *func);

static int armPmonResetAllCounters(SceUID threadId)
{
    int ret = 0;
    for (int i = 0; i < 6; i++)
    {
        if ((ret = sceKernelPMonThreadSetCounter(threadId, i, 0)) < 0)
        {
            return ret;
        }
    }

    return sceKernelPMonThreadSetCounter(threadId, 0x1F, 0);
}

/* Native ThreadMgr takes byte capacity and returns global thread IDs. */
static int armPmonExecAllThread(SceUInt32 action, SceUInt32 arg0, SceUInt32 arg1)
{
	SceUID pid = ksceKernelGetProcessId();
	SceUID *ids;
	int count, copied, total, ret = 0;

	count = ksceKernelGetThreadIdList(pid, NULL, 0, NULL);
	if (count < 0)
		return count;
	if (count == 0)
		return 0;
	/* Bound memory and retry if threads were created during enumeration. */
	for (int attempt = 0; attempt < 4; attempt++) {
		if (count > 65520)
			return (int)0x8002710B;
		int capacity = count + 16;
		ids = ksceKernelAllocHeapMemory(threadListHeap, capacity * sizeof(*ids));
		if (!ids)
			return (int)0x8002710B;
		copied = 0;
		total = ksceKernelGetThreadIdList(pid, ids, capacity * sizeof(*ids), &copied);
		if (total < 0 || copied < 0 || copied > capacity) {
			ksceKernelFreeHeapMemory(threadListHeap, ids);
			return total < 0 ? total : (int)0x80020005;
		}
		if (total > capacity) {
			ksceKernelFreeHeapMemory(threadListHeap, ids);
			count = total;
			continue;
		}
		for (int i = 0; i < copied; i++) {
			switch (action) {
			case PMON_ACTION_RESET:
				ret = armPmonResetAllCounters(ids[i]);
				break;
			case PMON_ACTION_SET_EVENT:
				ret = sceKernelPMonThreadSetEvent(ids[i], arg0, arg1);
				break;
			case PMON_ACTION_START:
				ret = sceKernelPMonThreadSetEnableCounter(ids[i], 0x8000003F);
				break;
			case PMON_ACTION_STOP:
				ret = sceKernelPMonThreadClearEnableCounter(ids[i], 0x8000003F);
				break;
			case PMON_ACTION_SET_COUNTER:
				ret = sceKernelPMonThreadSetCounter(ids[i], arg0, arg1);
				break;
			default:
				ret = (int)0x80020005;
			}
			if (ret != 0)
				break;
		}
		ksceKernelFreeHeapMemory(threadListHeap, ids);
		return ret;
	}
	return (int)0x80020005;
}

/* Best effort: attempt every disable even if one setter fails. */
static int disableProcessPmon(void)
{
	int first = 0, result;
#define DISABLE(call)                     \
	do {                              \
		result = (call);          \
		if (result < 0 && !first) \
			first = result;   \
	} while (0)
	DISABLE(setProcessUserEnable(0, 0));
	DISABLE(sceKernelSetInitialPMUSERENR(0, 0));
	DISABLE(sceKernelPMonSetUserEnableRegister(0, 0));
	DISABLE(sceKernelSetInitialPMCR(0, 0));
	DISABLE(sceKernelPMonSetControlRegister(0, 0));
#undef DISABLE
	return first;
}

int sceKernelPerfArmPmonOpen(void)
{
	int permission = ksceKernelSetPermission(0x80);
	int ret, restore;

	if (permission < 0)
		return permission;
	if ((ret = sceKernelSetInitialPMCR(0, 0x11)) < 0 ||
	    (ret = sceKernelPMonSetControlRegister(0, 0x11)) < 0 ||
	    (ret = sceKernelSetInitialPMUSERENR(0, 1)) < 0 ||
	    (ret = sceKernelPMonSetUserEnableRegister(0, 1)) < 0 ||
	    (ret = setProcessUserEnable(0, 1)) < 0)
		disableProcessPmon();
	restore = ksceKernelSetPermission(permission);
	return ret < 0 ? ret : (restore < 0 ? restore : ret);
}

int sceKernelPerfArmPmonClose(void)
{
	int permission = ksceKernelSetPermission(0x80);
	int ret, restore;

	if (permission < 0)
		return permission;
	ret = disableProcessPmon();
	restore = ksceKernelSetPermission(permission);
	return ret < 0 ? ret : (restore < 0 ? restore : ret);
}

int sceKernelPerfArmPmonStart(SceUID threadId)
{
	if ((threadId != 0 && threadId != SCE_KERNEL_THREAD_ID_PROCESS_ALL_ID && threadId != -1) &&
	    (threadId = kscePUIDtoGUID(0, threadId)) < 0) {
		return threadId;
	}

	if (threadId != SCE_KERNEL_THREAD_ID_PROCESS_ALL_ID && threadId != -1)
		return sceKernelPMonThreadSetEnableCounter(threadId, 0x8000003F);
	else
		return armPmonExecAllThread(PMON_ACTION_START, 0, 0);
}

int sceKernelPerfArmPmonStop(SceUID threadId)
{
	if ((threadId != 0 && threadId != SCE_KERNEL_THREAD_ID_PROCESS_ALL_ID && threadId != -1) &&
	    (threadId = kscePUIDtoGUID(0, threadId)) < 0) {
		return threadId;
	}

	if (threadId != SCE_KERNEL_THREAD_ID_PROCESS_ALL_ID && threadId != -1)
		return sceKernelPMonThreadClearEnableCounter(threadId, 0x8000003F);
	else
		return armPmonExecAllThread(PMON_ACTION_STOP, 0, 0);
}

int sceKernelPerfArmPmonReset(SceUID threadId)
{
	if ((threadId != 0 && threadId != SCE_KERNEL_THREAD_ID_PROCESS_ALL_ID && threadId != -1) &&
	    (threadId = kscePUIDtoGUID(0, threadId)) < 0) {
		return threadId;
	}

	if (threadId != SCE_KERNEL_THREAD_ID_PROCESS_ALL_ID && threadId != -1)
		return armPmonResetAllCounters(threadId);
	else
		return armPmonExecAllThread(PMON_ACTION_RESET, 0, 0);
}

int sceKernelPerfArmPmonSelectEvent(SceUID threadId, SceUInt32 counter, SceUInt8 eventCode)
{
	if ((threadId != 0 && threadId != SCE_KERNEL_THREAD_ID_PROCESS_ALL_ID && threadId != -1) &&
	    (threadId = kscePUIDtoGUID(0, threadId)) < 0) {
		return threadId;
	}

	if (threadId != SCE_KERNEL_THREAD_ID_PROCESS_ALL_ID && threadId != -1)
		return sceKernelPMonThreadSetEvent(threadId, counter, eventCode);
	else
		return armPmonExecAllThread(PMON_ACTION_SET_EVENT, counter, eventCode);
}

int sceKernelPerfArmPmonSetCounterValue(SceUID threadId, SceUInt32 counter, SceUInt32 value)
{
	if ((threadId != 0 && threadId != SCE_KERNEL_THREAD_ID_PROCESS_ALL_ID && threadId != -1) &&
	    (threadId = kscePUIDtoGUID(0, threadId)) < 0) {
		return threadId;
	}

	if (threadId != SCE_KERNEL_THREAD_ID_PROCESS_ALL_ID && threadId != -1)
		return sceKernelPMonThreadSetCounter(threadId, counter, value);
	else
		return armPmonExecAllThread(PMON_ACTION_SET_COUNTER, counter, value);
}

static void releaseResources(void)
{
	for (int i = 1; i >= 0; i--) {
		if (injectIds[i] >= 0) {
			taiInjectReleaseForKernel(injectIds[i]);
			injectIds[i] = -1;
		}
	}
	if (threadListHeap >= 0) {
		ksceKernelDeleteHeap(threadListHeap);
		threadListHeap = -1;
	}
}

/* Byte signatures are from the supplied 3.65 CEX ProcessMgr ELF.
 * Patch only CBZ after checkDipsw(0xE4), retaining the call and other checks.
 */
static int patchInitialStateGate(uintptr_t function, const uint8_t call[4])
{
	const uint8_t *body = (const uint8_t *)(function & ~(uintptr_t)1);
	static const uint8_t nop[2] = {0x00, 0xBF};
	if (body[0] != 0x70 || body[1] != 0xB5 || body[14] != 0xE4 || body[15] != 0x20 ||
	    memcmp(body + 0x16, call, 4) != 0 || body[0x1A] != 0x90 || body[0x1B] != 0xB1)
		return (int)0x80020005;
	return taiInjectAbsForKernel(KERNEL_PID, (void *)(body + 0x1A), nop, sizeof(nop));
}

#define RESOLVE(module, library, nid, target)                                \
	do {                                                                 \
		if (module_get_export_func(KERNEL_PID, module, library, nid, \
					   (uintptr_t *)&target) < 0 ||      \
		    !target)                                                 \
			goto fail;                                           \
	} while (0)

int module_start(SceSize argSize, void *argp)
{
	const uint8_t userCall[4] = {0x03, 0xF0, 0x42, 0xE8};
	const uint8_t controlCall[4] = {0x02, 0xF0, 0xF2, 0xEF};
	(void)argSize;
	(void)argp;
	RESOLVE("SceProcessmgr", 0x746EC971, 0x61B9B6FA, sceKernelSetInitialPMCR);
	RESOLVE("SceProcessmgr", 0x746EC971, 0xB1C3EFCA, sceKernelSetInitialPMUSERENR);
	RESOLVE("SceProcessmgr", 0x746EC971, 0x6599E5D9, setProcessUserEnable);
	RESOLVE("SceKernelThreadMgr", 0xE2C40624, 0x1AAFA818, sceKernelPMonSetControlRegister);
	RESOLVE("SceKernelThreadMgr", 0xE2C40624, 0x5053B005, sceKernelPMonSetUserEnableRegister);
	RESOLVE("SceKernelThreadMgr", 0x7F8593BA, 0x7F831213, sceKernelPMonThreadSetEnableCounter);
	RESOLVE("SceKernelThreadMgr", 0x7F8593BA, 0x1D2A6815,
		sceKernelPMonThreadClearEnableCounter);
	RESOLVE("SceKernelThreadMgr", 0x7F8593BA, 0x7B3368F1, sceKernelPMonThreadSetCounter);
	RESOLVE("SceKernelThreadMgr", 0x7F8593BA, 0xFFB9CD24, sceKernelPMonThreadSetEvent);
	threadListHeap = ksceKernelCreateHeap("RetailPerfThreadList", 0x10000, NULL);
	if (threadListHeap < 0)
		goto fail;
	injectIds[0] = patchInitialStateGate((uintptr_t)sceKernelSetInitialPMUSERENR, userCall);
	if (injectIds[0] < 0)
		goto fail;
	injectIds[1] = patchInitialStateGate((uintptr_t)sceKernelSetInitialPMCR, controlCall);
	if (injectIds[1] < 0)
		goto fail;
	return SCE_KERNEL_START_SUCCESS;
fail:
	releaseResources();
	return SCE_KERNEL_START_FAILED;
}

int module_stop(SceSize argSize, void *argp)
{
	(void)argSize;
	(void)argp;
	releaseResources();
	return SCE_KERNEL_STOP_SUCCESS;
}
