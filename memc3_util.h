#ifndef __BENCH_UTIL__
#define __BENCH_UTIL__
#include "memc3_config.h"
#include <execinfo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* forward declaration */
void print_memc3_settings(void);

void *alloc(size_t size);
void dealloc(void *p);

double timeval_diff(struct timeval *start, struct timeval *end);
int get_cpunum(void);

/* Obtain a backtrace and print it to stdout. */
static inline void
print_trace (void)
{
    void *array[10];
    size_t size;
    char **strings;
    size_t i;

    size = backtrace (array, 10);
    strings = backtrace_symbols (array, size);

    printf ("Obtained %zd stack frames.\n", size);

    for (i = 0; i < size; i++)
        printf ("%s\n", strings[i]);

    free (strings);
}

static inline void
print_key(char* key, int nkey) {
    for (int i = 0; i < nkey; i ++) {
        printf("%x", *((char*) &key[i]));
    }
}

// added by Bin, needed by CLOCK
static inline uint8_t bv_getbit(void *bv, uint32_t index) {
    char* bitmap = (char*) bv;
    uint32_t byte_index = index >> 3;
    char  byte_mask  =  1 << (index & 7);
    return bitmap[byte_index] & byte_mask;
}

// added by Bin, needed by CLOCK
static inline void bv_setbit(void *bv, uint32_t index, uint8_t v) {
    char* bitmap = (char*) bv;
    uint32_t byte_index = index >> 3;
    char  byte_mask  =  (v & 1) << (index & 7);
    bitmap[byte_index] &= ~byte_mask;
    bitmap[byte_index] |= byte_mask;
}

#ifdef MEMC3_ENABLE_INT_KEYCMP

#define INT_KEYCMP_UNIT uint64_t

static uint64_t keycmp_mask[] = {0x0000000000000000ULL,
                                 0x00000000000000ffULL,
                                 0x000000000000ffffULL,
                                 0x0000000000ffffffULL,
                                 0x00000000ffffffffULL,
                                 0x000000ffffffffffULL,
                                 0x0000ffffffffffffULL,
                                 0x00ffffffffffffffULL};
static inline
bool keycmp(const char* key1, const char* key2, size_t len) {

    INT_KEYCMP_UNIT v_key1;
    INT_KEYCMP_UNIT v_key2;
    size_t k = 0;
    while ((len ) >= k + sizeof(INT_KEYCMP_UNIT)) {
        v_key1 = *(INT_KEYCMP_UNIT *) (key1 + k);
        v_key2 = *(INT_KEYCMP_UNIT *) (key2 + k);
        if (v_key1 != v_key2)
            return false;
        k += sizeof(INT_KEYCMP_UNIT);
    }
    /*
     * this code only works for little endian
     */
    if (len - k) {
        v_key1 = *(INT_KEYCMP_UNIT *) (key1 + k);
        v_key2 = *(INT_KEYCMP_UNIT *) (key2 + k);
        return ((v_key1 ^ v_key2) & keycmp_mask[len - k]) == 0;
    }
    return true;
}

#else
static inline
bool keycmp(const char* key1, const char* key2, size_t len) {
    return memcmp(key1, key2, len) == 0;
}
#endif


#ifdef DO_PERF_COUNTING
#include "perf_count.h"

static const char *perf_count_names[] =
{
	//"Cycle",
	//"Instr",
	//"CacheRef",
	//"CacheMiss",
	//"BranchInstr",
	//"BranchMiss",
	//"L1DReadAccess",
	//"L1DReadMiss",
	"LLReadAccess",
	"LLReadMiss",
	"DTLBReadAccess",
	"DTLBReadMiss",
	//"PageFault",
	//"CtxSwitch",
};

static const enum PERF_COUNT_TYPE perf_count_types[] =
{
	//PERF_COUNT_TYPE_HW_CPU_CYCLES,
	//PERF_COUNT_TYPE_HW_INSTRUCTIONS,
	//PERF_COUNT_TYPE_HW_CACHE_REFERENCES,
	//PERF_COUNT_TYPE_HW_CACHE_MISSES,
	//PERF_COUNT_TYPE_HW_BRANCH_INSTRUCTIONS,
	//PERF_COUNT_TYPE_HW_BRANCH_MISSES,
	//PERF_COUNT_TYPE_HW_CACHE_L1D_READ_ACCESS,
	//PERF_COUNT_TYPE_HW_CACHE_L1D_READ_MISS,
	PERF_COUNT_TYPE_HW_CACHE_LL_READ_ACCESS,
	PERF_COUNT_TYPE_HW_CACHE_LL_READ_MISS,
	PERF_COUNT_TYPE_HW_CACHE_DTLB_READ_ACCESS,
	PERF_COUNT_TYPE_HW_CACHE_DTLB_READ_MISS,
	//PERF_COUNT_TYPE_SW_PAGE_FAULTS,
	//PERF_COUNT_TYPE_SW_CONTEXT_SWITCHES,
};

static inline void print_perf_counts(perf_count_t perf_count, double tdiff)
{
	for (size_t i = 0; i < sizeof(perf_count_types) / sizeof(perf_count_types[0]); i++)
	{
		printf("%-14s: %.5f %.5f M/sec\n", perf_count_names[i],
               (float) perf_count_get_by_index(perf_count, (int)i),
               (float) perf_count_get_by_index(perf_count, (int)i) / tdiff / 1000000);
	}
	printf("\n");
}

#define TIME(label, statement, totalnum)                 \
    do { \
    printf("<%s>\n", label); \
    struct timeval tvs, tve; \
	perf_count_t perf_count = perf_count_init(perf_count_types, sizeof(perf_count_types) / sizeof(perf_count_types[0]), 0); \
    perf_count_reset(perf_count);                                     \
    perf_count_start(perf_count);                                     \
    gettimeofday(&tvs, NULL); \
    do { statement; } while(0);	\
    gettimeofday(&tve, NULL); \
    perf_count_stop(perf_count);                                  \
    double tvsd = (double)tvs.tv_sec + (double)tvs.tv_usec/1000000; \
    double tved = (double)tve.tv_sec + (double)tve.tv_usec/1000000; \
    double tdiff = tved - tvsd; \
    printf("\ttime: %.5f sec\n", tdiff); \
    if (totalnum) printf("\ttotal: %zu\n\ttput: %.5f\n", totalnum, totalnum/tdiff); \
    print_perf_counts(perf_count, tdiff);                                     \
    printf("\n"); \
    perf_count_free(perf_count); \
    } while (0)

#else

#define TIME(label, statement, totalnum)                 \
    do { \
    printf("<%s>\n", label); \
    struct timeval tvs, tve; \
    gettimeofday(&tvs, NULL); \
    do { statement; } while(0);	\
    gettimeofday(&tve, NULL); \
    double tvsd = (double)tvs.tv_sec + (double)tvs.tv_usec/1000000; \
    double tved = (double)tve.tv_sec + (double)tve.tv_usec/1000000; \
    printf("%s\n\ttime: %.5f sec\n", label, tved-tvsd); \
    if (totalnum) printf("\ttotal: %zu\n\ttput: %.5f\n", totalnum, totalnum/(tved-tvsd)); \
    printf("\n"); \
    } while (0)
#endif

#endif
