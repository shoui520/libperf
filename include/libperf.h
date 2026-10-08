#ifndef _PSP2_LIBPERF_H_
#define _PSP2_LIBPERF_H_

#include <psp2/perf.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SCE_PERF_ARM_PMON_THREAD_ID_ALL ((SceUID)0x10027)
#define SCE_PERF_ARM_PMON_COUNTER_CYCLE 31
#define SCE_PERF_ARM_PMON_CYCLE_COUNTER 31U
#define SCE_PERF_ARM_PMON_PMCNT_NUM 6U
#define SCE_PERF_ARM_PMON_COUNTER_MASK_ALL 0x3FU
#define SCE_PERF_ERROR_INVALID_ARGUMENT ((int)0x80580000)
#define SCE_PERF_ERROR_NOT_INITIALIZED ((int)0x80580005)

#define SCE_PERF_ARM_PMON_COUNTER_0 0U
#define SCE_PERF_ARM_PMON_COUNTER_MASK_0 (1U << 0)

#define SCE_PERF_ARM_PMON_COUNTER_1 1U
#define SCE_PERF_ARM_PMON_COUNTER_MASK_1 (1U << 1)

#define SCE_PERF_ARM_PMON_COUNTER_2 2U
#define SCE_PERF_ARM_PMON_COUNTER_MASK_2 (1U << 2)

#define SCE_PERF_ARM_PMON_COUNTER_3 3U
#define SCE_PERF_ARM_PMON_COUNTER_MASK_3 (1U << 3)

#define SCE_PERF_ARM_PMON_COUNTER_4 4U
#define SCE_PERF_ARM_PMON_COUNTER_MASK_4 (1U << 4)

#define SCE_PERF_ARM_PMON_COUNTER_5 5U
#define SCE_PERF_ARM_PMON_COUNTER_MASK_5 (1U << 5)

int scePerfArmPmonReset(SceUID threadId);
int scePerfArmPmonSelectEvent(SceUID threadId, SceUInt32 counter, SceUInt8 eventCode);
int scePerfArmPmonStart(SceUID threadId);
int scePerfArmPmonStop(SceUID threadId);
int scePerfArmPmonGetCounterValue(SceUID threadId, SceUInt32 counter, SceUInt32 *value);
int scePerfArmPmonSetCounterValue(SceUID threadId, SceUInt32 counter, SceUInt32 value);
int scePerfArmPmonSoftwareIncrement(SceUInt32 mask);

SceUInt64 scePerfGetTimebaseValue(void);
SceUInt32 scePerfGetTimebaseFrequency(void);

#ifdef __cplusplus
}
#endif

#endif