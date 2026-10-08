#ifndef PROFILER_WORKLOADS_H
#define PROFILER_WORKLOADS_H

#include <stdint.h>

typedef void (*WorkFunction)(void);
typedef struct {
	const char *name;
	const char *job;
	WorkFunction run;
} Work;

extern const Work workloads[];
extern const unsigned workloadCount;
int workloadsInit(void);
int workloadsCheck(void);
void workloadsFree(void);
void emptyWork(void);
#endif
