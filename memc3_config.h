#ifndef __MEMC3_CONFIG__
#define __MEMC3_CONFIG__

/*
 * size of the key in bytes
 */
#define NKEY 16
#define NVAL 32

//#define TEST_ORIGINAL 1  
#define TEST_CUCKOO 1


/*
 * make sure one and only one option above is enabled
 */
#if (TEST_CUCKOO + TEST_ORIGINAL != 1)
#error "you must specify one and only one hashtable"
#endif


//#define TEST_LRU 1
#define TEST_CLOCK 1
#if (TEST_LRU + TEST_CLOCK != 1)
#error "you must specify one and only one eviction policy"
#endif

/*
 * count the perf 
 */
//#define DO_PERF_COUNTING


/*
 * enable huge table to reduce TLB misses
 */
//#define ENABLE_HUGEPAGE

/*
 * enable key comparison by casting bits into ints
 */
#define ENABLE_INT_KEYCMP

/*
 * disable locking
 */
//#define NO_LOCKING 1

/*
 * enable global locking
 */
//#define ENABLE_GLOBAL_LOCK 1

/*
 * enable bucket locking
 */
//#define ENABLE_FG_LOCK    1

/*
 * enable optimistic locking
 */
#define ENABLE_OPT_LOCK    1


#if (ENABLE_OPT_LOCK + ENABLE_FG_LOCK + ENABLE_GLOBAL_LOCK + NO_LOCKING != 1)
#error "you must specify one and only locking policy"
#endif


/*
 * enable tag 
 */
#define ENABLE_TAG

/*
 * enable parallel cuckoo
 */
#define PAR_CUCKOO_WIDTH 1

//#define PRINT_LF
//#define COUNT_LARGEST_BUCKET

#endif
