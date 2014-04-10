#ifndef __PERF_COUNT__
#define __PERF_COUNT__

#include <inttypes.h>

enum PERF_COUNT_TYPE 
{
	PERF_COUNT_TYPE_HW_CPU_CYCLES,
	PERF_COUNT_TYPE_HW_INSTRUCTIONS,
	PERF_COUNT_TYPE_HW_CACHE_REFERENCES,
	PERF_COUNT_TYPE_HW_CACHE_MISSES,
	PERF_COUNT_TYPE_HW_BRANCH_INSTRUCTIONS,
	PERF_COUNT_TYPE_HW_BRANCH_MISSES,
	PERF_COUNT_TYPE_HW_BUS_CYCLES,
	PERF_COUNT_TYPE_HW_CACHE_L1D_READ_ACCESS,
	PERF_COUNT_TYPE_HW_CACHE_L1D_READ_MISS,
	PERF_COUNT_TYPE_HW_CACHE_L1D_PREFETCH_ACCESS,		// not working?
	PERF_COUNT_TYPE_HW_CACHE_L1D_PREFETCH_MISS,			// not working?
	PERF_COUNT_TYPE_HW_CACHE_LL_READ_ACCESS,
	PERF_COUNT_TYPE_HW_CACHE_LL_READ_MISS,
	PERF_COUNT_TYPE_HW_CACHE_LL_PREFETCH_ACCESS,		// not working?
	PERF_COUNT_TYPE_HW_CACHE_LL_PREFETCH_MISS,			// not working?
	PERF_COUNT_TYPE_HW_CACHE_DTLB_READ_ACCESS,
	PERF_COUNT_TYPE_HW_CACHE_DTLB_READ_MISS,
	PERF_COUNT_TYPE_HW_CACHE_DTLB_PREFETCH_ACCESS,		// not working?
	PERF_COUNT_TYPE_HW_CACHE_DTLB_PREFETCH_MISS,		// not working?
	PERF_COUNT_TYPE_SW_CPU_CLOCK,
	PERF_COUNT_TYPE_SW_TASK_CLOCK,
	PERF_COUNT_TYPE_SW_PAGE_FAULTS,
	PERF_COUNT_TYPE_SW_CONTEXT_SWITCHES,
	PERF_COUNT_TYPE_SW_CPU_MIGRATIONS,
	PERF_COUNT_TYPE_SW_PAGE_FAULTS_MIN,
	PERF_COUNT_TYPE_SW_PAGE_FAULTS_MAJ,
	PERF_COUNT_TYPE_SW_ALIGNMENT_FAULTS,
	PERF_COUNT_TYPE_SW_EMULATION_FAULTS,
};

typedef void *perf_count_t;

#define PERF_COUNT_INVALID ((uint64_t)-1)

// system_wide would require CAP_SYS_ADMIN
perf_count_t perf_count_init(const enum PERF_COUNT_TYPE *perf_count_types, int num_events, int system_wide);
void perf_count_free(perf_count_t perf_count);

void perf_count_start(perf_count_t perf_count);
void perf_count_stop(perf_count_t perf_count);
void perf_count_reset(perf_count_t perf_count);

uint64_t perf_count_get_by_type(perf_count_t perf_count, enum PERF_COUNT_TYPE type);
uint64_t perf_count_get_by_index(perf_count_t perf_count, int index);

#endif
