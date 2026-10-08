#include <psp2/kernel/clib.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/pamgr.h>
#include <psp2/kernel/threadmgr/thread.h>

#include "libperf.h"

int sceKernelGetPMUSERENR(void);

int sceKernelPMonThreadGetCounter(SceUID thid, SceUInt32 counter, SceUInt32 *value);

int scePerfArmPmonStart(SceUID threadId)
{
	if (!sceKernelGetPMUSERENR())
		return SCE_PERF_ERROR_NOT_INITIALIZED;

	if (threadId == 0) {
		__asm__ volatile("mcr p15, #0, %0, c9, c12, #1" ::"r"(0x8000003F) : "memory");
		return 0;
	}

	int ret = sceKernelPerfArmPmonStart(threadId);
	if (ret == (int)0x80024501)
		ret = SCE_PERF_ERROR_INVALID_ARGUMENT;
	return ret;
}

int scePerfArmPmonStop(SceUID threadId)
{
	if (!sceKernelGetPMUSERENR())
		return SCE_PERF_ERROR_NOT_INITIALIZED;

	if (threadId == 0) {
		__asm__ volatile("mcr p15, #0, %0, c9, c12, #2" ::"r"(0x8000003F) : "memory");
		return 0;
	}

	int ret = sceKernelPerfArmPmonStop(threadId);
	if (ret == (int)0x80024501)
		ret = SCE_PERF_ERROR_INVALID_ARGUMENT;
	return ret;
}

int scePerfArmPmonReset(SceUID threadId)
{
	if (!sceKernelGetPMUSERENR())
		return SCE_PERF_ERROR_NOT_INITIALIZED;

	if (threadId == 0) {
		SceUInt32 PMCR;
		__asm__ volatile("mrc p15, #0, %0, c9, c12, #0" : "=r"(PMCR)::"memory");
		PMCR |= 0x6;
		__asm__ volatile("mcr p15, #0, %0, c9, c12, #0" ::"r"(PMCR) : "memory");
		return 0;
	}

	int ret = sceKernelPerfArmPmonReset(threadId);
	if (ret == (int)0x80024501)
		ret = SCE_PERF_ERROR_INVALID_ARGUMENT;
	return ret;
}

int scePerfArmPmonSelectEvent(SceUID threadId, SceUInt32 counter, SceUInt8 eventCode)
{
	if (counter > 5)
		return SCE_PERF_ERROR_INVALID_ARGUMENT;

	switch (eventCode) {
	case 0x00:
	case 0x01:
	case 0x02:
	case 0x03:
	case 0x04:
	case 0x05:
	case 0x06:
	case 0x07:
	case 0x09:
	case 0x0A:
	case 0x0B:
	case 0x0C:
	case 0x0D:
	case 0x0F:
	case 0x10:
	case 0x11:
	case 0x12:
	case 0x40:
	case 0x41:
	case 0x42:
	case 0x50:
	case 0x51:
	case 0x60:
	case 0x61:
	case 0x62:
	case 0x63:
	case 0x64:
	case 0x65:
	case 0x66:
	case 0x67:
	case 0x68:
	case 0x6E:
	case 0x70:
	case 0x71:
	case 0x72:
	case 0x73:
	case 0x74:
	case 0x80:
	case 0x81:
	case 0x82:
	case 0x83:
	case 0x84:
	case 0x85:
	case 0x86:
	case 0x8A:
	case 0x8B:
	case 0x90:
	case 0x91:
	case 0x92:
	case 0x93:
	case 0xA0:
	case 0xA1:
	case 0xA2:
	case 0xA3:
	case 0xA4:
	case 0xA5:
		break;
	default:
		return SCE_PERF_ERROR_INVALID_ARGUMENT;
		break;
	}

	int ret = sceKernelPerfArmPmonSelectEvent(threadId, counter, eventCode);
	if (ret == (int)0x80024501)
		ret = SCE_PERF_ERROR_INVALID_ARGUMENT;
	return ret;
}

int scePerfArmPmonSetCounterValue(SceUID threadId, SceUInt32 counter, SceUInt32 value)
{
	if (!sceKernelGetPMUSERENR())
		return SCE_PERF_ERROR_NOT_INITIALIZED;

	if (counter > 5 && counter != 0x1F) {
		return SCE_PERF_ERROR_INVALID_ARGUMENT;
	}

	if (threadId == 0) {
		if (counter != 0x1F) {
			__asm__ volatile("mcr p15, #0, %0, c9, c12, #5" ::"r"(counter) : "memory");
			__asm__ volatile("mcr p15, #0, %0, c9, c13, #2" ::"r"(value) : "memory");
		} else {
			__asm__ volatile("mcr p15, #0, %0, c9, c13, #0" ::"r"(value) : "memory");
		}
		return 0;
	}

	return sceKernelPerfArmPmonSetCounterValue(threadId, counter, value);
}

int scePerfArmPmonGetCounterValue(SceUID threadId, SceUInt32 counter, SceUInt32 *value)
{
	if (!sceKernelGetPMUSERENR())
		return SCE_PERF_ERROR_NOT_INITIALIZED;

	if (!value || threadId == -1 || threadId == 0x10027 || (counter > 5 && counter != 0x1F)) {
		return SCE_PERF_ERROR_INVALID_ARGUMENT;
	}

	if (threadId == 0) {
		if (counter != 0x1F) {
			__asm__ volatile("mcr p15, #0, %0, c9, c12, #5" ::"r"(counter) : "memory");
			__asm__ volatile("mrc p15, #0, %0, c9, c13, #2" : "=r"(*value)::"memory");
		} else {
			__asm__ volatile("mrc p15, #0, %0, c9, c13, #0" : "=r"(*value)::"memory");
		}
		return 0;
	}

	int ret = sceKernelPMonThreadGetCounter(threadId, counter, value);
	if (ret == (int)0x80024501)
		ret = SCE_PERF_ERROR_INVALID_ARGUMENT;
	return ret;
}

int scePerfArmPmonSoftwareIncrement(SceUInt32 mask)
{
	if (!sceKernelGetPMUSERENR())
		return SCE_PERF_ERROR_NOT_INITIALIZED;

	if (mask & ~0x3F)
		return SCE_PERF_ERROR_INVALID_ARGUMENT;

	__asm__ volatile("mcr p15, #0, %0, c9, c12, #4" ::"r"(mask) : "memory");
	return 0;
}

SceUInt64 scePerfGetTimebaseValue(void)
{
	/* CEX provides a global microsecond timebase without TOOL Perfmon access. */
	return (SceUInt64)sceKernelGetSystemTimeWide() & 0xFFFFFFFFFFFFULL;
}

SceUInt32 scePerfGetTimebaseFrequency(void)
{
	/* MHz: one tick per microsecond, independent of CPU frequency. */
	return 1;
}

int module_start(SceSize argSize, void *argp)
{
	(void)argSize;
	(void)argp;
	if (sceKernelPerfArmPmonOpen() < 0)
		return SCE_KERNEL_START_FAILED;
	return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize argSize, void *argp)
{
	(void)argSize;
	(void)argp;
	if (sceKernelPerfArmPmonClose() < 0)
		return SCE_KERNEL_STOP_FAIL;
	return SCE_KERNEL_STOP_SUCCESS;
}