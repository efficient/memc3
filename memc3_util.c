#include <stdio.h>
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <sys/shm.h>
#define __USE_GNU
#include <sched.h>

#include "memcached.h"

#include "memc3_util.h"

void print_memc3_settings()
{
    printf("memc3 settings:\n");
#ifdef MEMC3_ASSOC_CHAIN 
    printf("\thashtable = built-in hashtable\n");
#endif

#ifdef MEMC3_CACHE_LRU
    printf("\teviction = LRU\n");
#endif

#ifdef MEMC3_CACHE_CLOCK
    printf("\teviction = CLOCK\n");
#endif

#ifdef DO_PERF_COUNTING
    printf("\t+perf_counting\n");
#endif

#ifdef MEMC3_ENABLE_HUGEPAGE
    printf("\t+hugepage\n");
#endif

#ifdef MEMC3_ENABLE_INT_KEYCMP
    printf("\t+int_keycmp\n");
#endif

#ifdef MEMC3_ASSOC_CUCKOO
    printf("\t+cuckoo \n");
#ifdef MEMC3_ENABLE_TAG
    printf("\t+tag\n");
#endif
#if (MEMC3_ASSOC_CUCKOO_WIDTH > 1)
    printf("\t+%d-way cuckoo_path\n", MEMC3_ASSOC_CUCKOO_WIDTH);
#endif

#endif


#ifdef MEMC3_LOCK_GLOBAL
    printf("\t+global lock\n");
#endif
 
#ifdef MEMC3_LOCK_NONE
    printf("\t+no locking\n");
#endif

#ifdef MEMC3_LOCK_OPT
    printf("\t+opt lock\n");
#endif

#ifdef MEMC3_LOCK_FINEGRAIN
    printf("\t+bucket lock\n");
#endif


    printf("\n");
}

#define HUGEPAGE_SIZE 2097152

void *alloc(size_t size) 
{
#if defined(MEMC3_ENABLE_HUGEPAGE) && defined(__linux__)
    if (size % HUGEPAGE_SIZE != 0)
		size = (size / HUGEPAGE_SIZE + 1) * HUGEPAGE_SIZE;
	int shmid = shmget(IPC_PRIVATE, size, /*IPC_CREAT |*/ SHM_HUGETLB | SHM_R | SHM_W);
	if (shmid == -1) {
		perror("shmget failed");
        exit(EXIT_FAILURE);
	}
	void *p = shmat(shmid, NULL, 0);
	if (p == (void *)-1) {
		perror("Shared memory attach failed");
        exit(EXIT_FAILURE);
	}
	if (shmctl(shmid, IPC_RMID, NULL) == -1) {
		perror("shmctl failed");
	}
	return p;
#else
    void *p = malloc(size);
    if (NULL == p) {
        perror("malloc failed");
        assert(0);
    }
    return p;
#endif
}

void dealloc(void *p)
{
#ifdef MEMC3_ENABLE_HUGEPAGE
	if (shmdt(p)) {
		perror("");
		assert(0);
	}
#else
    free(p);
#endif
}

double timeval_diff(struct timeval *start, 
                    struct timeval *end)
{
    /* Calculate the second difference*/
    double r = end->tv_sec - start->tv_sec;

    /* Calculate the microsecond difference */
    if (end->tv_usec > start->tv_usec)
        r += (end->tv_usec - start->tv_usec)/1000000.0;
    else if (end->tv_usec < start->tv_usec)
        r -= (start->tv_usec - end->tv_usec)/1000000.0;

    return r;
}

int get_cpunum()
{
    int num = 0;

#ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    sched_getaffinity(0, sizeof(cpuset), &cpuset);
    for (int i = 0; i < 32; i++)
    {
        if (CPU_ISSET(i, &cpuset))
            num++;
    }
#endif
    return num;
}
