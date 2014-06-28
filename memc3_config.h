#ifndef __MEMC3_CONFIG__
#define __MEMC3_CONFIG__

//#define MEMC3_ASSOC_CHAIN 1  
#define MEMC3_ASSOC_CUCKOO 1

/*
 * make sure one and only one option above is enabled
 */
#if (MEMC3_ASSOC_CUCKOO + MEMC3_ASSOC_CHAIN != 1)
#error "you must specify one and only one hashtable"
#endif


//#define MEMC3_CACHE_LRU 1
#define MEMC3_CACHE_CLOCK 1
#if (MEMC3_CACHE_LRU + MEMC3_CACHE_CLOCK != 1)
#error "you must specify one and only one eviction policy"
#endif


/*
 * enable huge table to reduce TLB misses
 */
//#define MEMC3_ENABLE_HUGEPAGE

/*
 * enable key comparison by casting bits into ints
 */
#define MEMC3_ENABLE_INT_KEYCMP

/*
 * disable locking
 */
//#define MEMC3_LOCK_NONE 1

/*
 * enable global locking
 */
//#define MEMC3_LOCK_GLOBAL 1

/*
 * enable bucket locking
 */
//#define MEMC3_LOCK_FINEGRAIN    1

/*
 * enable optimistic locking
 */
#define MEMC3_LOCK_OPT    1


#if (MEMC3_LOCK_OPT + MEMC3_LOCK_FINEGRAIN + MEMC3_LOCK_GLOBAL + MEMC3_LOCK_NONE != 1)
#error "you must specify one and only locking policy"
#endif


/*
 * enable tag 
 */
#define MEMC3_ENABLE_TAG

/*
 * enable parallel cuckoo
 */
#define MEMC3_ASSOC_CUCKOO_WIDTH 1

#endif
