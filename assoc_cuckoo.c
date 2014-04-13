/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 * Hash table implementation -- multi-reader/single-writer cuckoo hashing
 *
 */

#include "memcached.h"
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/signal.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>
#include <xmmintrin.h>
#include <immintrin.h>

#include "memc3_config.h"
#include "memc3_util.h"
#include "bit_util.h"


#if defined(MEMC3_LOCK_FINEGRAIN)

#define fg_lock_count ((unsigned long int)1 << (13))
#define fg_lock_mask (fg_lock_count - 1)
static pthread_spinlock_t fg_locks[fg_lock_count];

static void fg_lock(uint32_t i1, uint32_t i2) {
    uint32_t j1, j2;
    j1 = i1 & fg_lock_mask;
    j2 = i2 & fg_lock_mask;
    if (j1 < j2) {
        pthread_spin_lock(&fg_locks[j1]);
        pthread_spin_lock(&fg_locks[j2]);
    } else if (j1 > j2) {
        pthread_spin_lock(&fg_locks[j2]);
        pthread_spin_lock(&fg_locks[j1]);
    } else
        pthread_spin_lock(&fg_locks[j1]);
}

static void fg_unlock(uint32_t i1, uint32_t i2) {
    uint32_t j1, j2;
    j1 = i1 & fg_lock_mask;
    j2 = i2 & fg_lock_mask;
    if (j1 < j2) {
        pthread_spin_unlock(&fg_locks[j2]);
        pthread_spin_unlock(&fg_locks[j1]);
    } else if (j1 > j2) {
        pthread_spin_unlock(&fg_locks[j1]);
        pthread_spin_unlock(&fg_locks[j2]);
    } else
        pthread_spin_unlock(&fg_locks[j1]);
}
#endif

/* 
 * Number of items in the hash table. 
 */
static unsigned int hash_items = 0;

/* 
 * Number of cuckoo kickouts. 
 */
static unsigned int num_moves  = 0;


/*
 * The maximum number of cuckoo operations per insert, 
 * we use 128 in the submission 
 * now change to 500
 */
#define MAX_CUCKOO_COUNT 500


/*
 * the structure of a bucket
 */
#define bucket_size 4

struct Bucket {
    TagType   tags[bucket_size];
    char      notused[4];
    ValueType vals[bucket_size];
}  __attribute__((__packed__));

static struct Bucket* buckets;

#define IS_SLOT_EMPTY(i,j) (buckets[i].tags[j] == 0)

//#define IS_TAG_EQUAL(i,j,tag) ((buckets[i].tags[j] & tagmask) == tag)
#define IS_TAG_EQUAL(bucket,j,tag) ((bucket.tags[j] & tagmask) == tag)

/*
 * Initialize the hash table
 */
void assoc2_init(const int hashtable_init) {
    hashpower = HASHPOWER_DEFAULT;
    if (hashtable_init) {
        hashpower = hashtable_init;
    }

    hashsize = (ub4) 1 << (hashpower);
    hashmask = hashsize - 1;

    /*
     * tagpower: number of bits per tag
     */
    tagpower = sizeof(TagType)*8;
    tagmask  = ((ub4) 1 << tagpower) - 1;

    buckets = alloc(hashsize * sizeof(struct Bucket));
    //buckets = malloc(hashsize * sizeof(struct Bucket));
    if (! buckets) {
        fprintf(stderr, "Failed to init hashtable.\n");
        exit(EXIT_FAILURE);
    }
    memset(buckets, 0, sizeof(struct Bucket) * hashsize);


#ifdef MEMC3_LOCK_OPT
    memset(keyver_array, 0, sizeof(keyver_array));
#endif

#ifdef MEMC3_LOCK_FINEGRAIN
    for (size_t i = 0; i < fg_lock_count; i++) {
        pthread_spin_init(&fg_locks[i], PTHREAD_PROCESS_PRIVATE);
    }
#endif

    STATS_LOCK();
    stats.hash_power_level = hashpower;
    stats.hash_bytes = hashsize * sizeof(struct Bucket);
    STATS_UNLOCK();
}

/*
 * Desotry all the buckets
 */
void assoc2_destroy() {
    dealloc(buckets);
}


/*
 * Try to read bucket i and check if the given tag is there
 */
static __attribute__ ((unused))
item *try_read(const char *key, const size_t nkey, TagType tag, size_t i) {

#ifdef MEMC3_ENABLE_TAG
    volatile uint32_t tmp = *((uint32_t *) &(buckets[i]));
#endif

    for (size_t j = 0; j < bucket_size; j ++) {
#ifdef MEMC3_ENABLE_TAG
        //if (IS_TAG_EQUAL(buckets[i], j, tag))
        uint8_t ch = ((uint8_t*) &tmp)[j];
        if (ch == tag)
#endif
        {
            /* volatile __m128i p, q; */
            /* p = _mm_loadu_si128((__m128i const *) &buckets[i].vals[0]); */
            /* q = _mm_loadu_si128((__m128i const *) &buckets[i].vals[2]); */
            /* item *vals[4]; */

            /* _mm_storeu_si128((__m128i *) vals, p); */
            /* _mm_storeu_si128((__m128i *) (vals + 2), q); */
            /* item *it = vals[j]; */

            item *it = buckets[i].vals[j];
            
#ifndef MEMC3_ENABLE_TAG
            if (it == NULL)
                return NULL;
#endif
            char* it_key = (char*) ITEM_key(it);
            if (keycmp(key, it_key, nkey)) {
                return it;
            }
        }
    }
    return NULL;
}


/*
 * The interface to find a key in this hash table
 */
item *assoc2_find(const char *key, const size_t nkey, const uint32_t hv) {
    TagType tag = _tag_hash(hv);
    size_t i1   = _index_hash(hv);
    size_t i2   = _alt_index(i1, tag);

    item *result = NULL;


#ifdef MEMC3_LOCK_OPT
    size_t lock = _lock_index(i1, i2, tag);
    uint32_t vs, ve;
TryRead:
    vs = read_keyver(lock);
#endif

#ifdef MEMC3_LOCK_FINEGRAIN
    fg_lock(i1, i2);
#endif

    //

    //_mm_prefetch(&buckets[i2], _MM_HINT_NTA);

    /* item *r1, *r2; */
    /* r1 = try_read(key, nkey, tag, i1); */
    /* r2 = try_read(key, nkey, tag, i2); */
    /* if (r1) result = r1; */
    /* else result = r2; */


#ifdef MEMC3_ENABLE_TAG
    volatile uint32_t tags1, tags2;
    tags1 = *((uint32_t *) &(buckets[i1]));
    tags2 = *((uint32_t *) &(buckets[i2]));
#endif

    for (size_t j = 0; j < 4; j ++) {
#ifdef MEMC3_ENABLE_TAG

        uint8_t ch = ((uint8_t*) &tags1)[j];
        if (ch == tag)
#endif
        {

            item *it = buckets[i1].vals[j];
            
//#ifndef MEMC3_ENABLE_TAG
            if (it == NULL)
                continue;
//#endif
            char* it_key = (char*) ITEM_key(it);
            if (keycmp(key, it_key, nkey)) {
                result = it;
                break;
            }
        }
    }

    if (!result) 
    {
		//tags2 = *((uint32_t *) &(buckets[i2]));
        for (size_t j = 0; j < 4; j ++) {
#ifdef MEMC3_ENABLE_TAG

            uint8_t ch = ((uint8_t*) &tags2)[j];
            if (ch == tag)
#endif
            {

                item *it = buckets[i2].vals[j];
            
//#ifndef MEMC3_ENABLE_TAG
                if (it == NULL)
                    continue;
//#endif
                char* it_key = (char*) ITEM_key(it);
                if (keycmp(key, it_key, nkey)) {
                    result = it;
                    break;
                }
            }
        }
    }




    //result = try_read(key, nkey, tag, i1);
    //if (!result)
    //{
    //   result = try_read(key, nkey, tag, i2);
    //}

#ifdef MEMC3_LOCK_OPT
    ve = read_keyver(lock);

    if (vs & 1 || vs != ve)
        goto TryRead;
#endif

#ifdef MEMC3_LOCK_FINEGRAIN
    fg_unlock(i1, i2);
#endif

    return result;
}


/* 
 * Make bucket  from[idx] slot[whichslot] available to insert a new item
 * return idx on success, -1 otherwise
 * @param from:   the array of bucket index
 * @param whichslot: the slot available
 * @param  depth: the current cuckoo depth
 */

size_t    cp_buckets[MAX_CUCKOO_COUNT][MEMC3_ASSOC_CUCKOO_WIDTH];
size_t    cp_slots[MAX_CUCKOO_COUNT][MEMC3_ASSOC_CUCKOO_WIDTH];
ValueType cp_vals[MAX_CUCKOO_COUNT][MEMC3_ASSOC_CUCKOO_WIDTH];
int       kick_count = 0;

static int cp_search(size_t depth_start, size_t *cp_index) {

    int depth = depth_start;
    while ((kick_count < MAX_CUCKOO_COUNT) && 
           (depth >= 0) && 
           (depth < MAX_CUCKOO_COUNT - 1)) {

        size_t *from  = &(cp_buckets[depth][0]);
        size_t *to    = &(cp_buckets[depth + 1][0]);
        
        /*
         * Check if any slot is already free
         */
        for (size_t idx = 0; idx < MEMC3_ASSOC_CUCKOO_WIDTH; idx ++) {
            size_t i = from[idx];
            size_t j;
            for (j = 0; j < bucket_size; j ++) {
                if (IS_SLOT_EMPTY(i, j)) {
                    cp_slots[depth][idx] = j;
                    //cp_vals[depth][idx]  = buckets[i].vals[j];
                    *cp_index   = idx;
                    return depth;
                }
            }

            j          = rand() % bucket_size; // pick the victim item
            cp_slots[depth][idx] = j;
            cp_vals[depth][idx]  = buckets[i].vals[j];
            to[idx]    = _alt_index(i, buckets[i].tags[j]);
        }

        kick_count += MEMC3_ASSOC_CUCKOO_WIDTH;
        depth ++;
    }

    printf("%u max cuckoo achieved, abort\n", kick_count);
    return -1;
}

static int cp_backmove(size_t depth_start, size_t idx) {
    
    int depth = depth_start;
    while (depth > 0) {
        size_t i1 = cp_buckets[depth - 1][idx];
        size_t i2 = cp_buckets[depth][idx];
        size_t j1 = cp_slots[depth - 1][idx];
        size_t j2 = cp_slots[depth][idx];

        /*
         * We plan to kick out j1, but let's check if it is still there;
         * there's a small chance we've gotten scooped by a later cuckoo.
         * If that happened, just... try again.
         */
        if (buckets[i1].vals[j1] != cp_vals[depth - 1][idx]) {
            /* try again */
            return depth;
        }

        assert(IS_SLOT_EMPTY(i2,j2));

#ifdef MEMC3_LOCK_OPT
        size_t lock   = _lock_index(i1, i2, 0);
        incr_keyver(lock);
#endif

#ifdef MEMC3_LOCK_FINEGRAIN
        fg_lock(i1, i2);
#endif
    

        buckets[i2].tags[j2] = buckets[i1].tags[j1];
        buckets[i2].vals[j2] = buckets[i1].vals[j1];

        buckets[i1].tags[j1] = 0;
        buckets[i1].vals[j1] = 0;

#ifdef PRINT_LF
        num_moves ++;
#endif


#ifdef MEMC3_LOCK_OPT
        incr_keyver(lock);
#endif

#ifdef MEMC3_LOCK_FINEGRAIN
        fg_unlock(i1, i2);
#endif
        depth --;
    }

    return depth;

}

static int cuckoo(int depth) {
    int cur;
    size_t idx;

    kick_count = 0;    
    while (1) {

        cur = cp_search(depth, &idx);
        if (cur < 0)
            return -1;
        assert(idx >= 0);
        cur = cp_backmove(cur, idx);
        if (cur == 0) {
            return idx;
        }

        depth = cur - 1;
    }

    return -1;
}

/*
 * Try to add an item to bucket i, 
 * return true on success and false on failure
 */
static bool try_add(item* it, TagType tag, size_t i, size_t lock) {
#ifdef PRINT_LF
    static double next_lf = 0.0;
#endif

    for (size_t j = 0; j < bucket_size; j ++) {
        if (IS_SLOT_EMPTY(i, j)) {

#ifdef MEMC3_LOCK_OPT
            incr_keyver(lock);
#endif

#ifdef MEMC3_LOCK_FINEGRAIN
            fg_lock(i, i);
#endif

            buckets[i].tags[j] = tag;
            buckets[i].vals[j] = it;
            
            /* atomic add for hash_item */
            //__sync_fetch_and_add(&hash_items, 1);
            hash_items ++;

#ifdef PRINT_LF
            if ((hash_items & 0x0fff) == 1) {
                double lf = (double) hash_items / bucket_size / hashsize;
                if (lf > next_lf) {
                    struct timeval tv; 
                    gettimeofday(&tv, NULL);
                    double tvd_now = (double)tv.tv_sec + (double)tv.tv_usec/1000000;
                    
                    printf("loadfactor=%.4f\tmoves=%u\thash_items=%u\ttime=%.8f\n", lf, num_moves, hash_items, tvd_now);
                    next_lf += 0.05;
                }
            }
#endif

#ifdef MEMC3_LOCK_OPT
            incr_keyver(lock);
#endif

#ifdef MEMC3_LOCK_FINEGRAIN
            fg_unlock(i, i);
#endif

            return true;
        }
    }
    return false;
}



/* Note: this isn't an assoc_update.  The key must not already exist to call this */
// need to be protected by cache_lock
int assoc2_insert(item *it, const uint32_t hv) {

    TagType tag = _tag_hash(hv);
    size_t i1   = _index_hash(hv);
    size_t i2   = _alt_index(i1, tag);
    size_t lock = _lock_index(i1, i2, tag);

    if (try_add(it, tag, i1, lock))
        return 1;

    if (try_add(it, tag, i2, lock))
        return 1;

    int idx;
    size_t depth = 0;
    for (idx = 0; idx < MEMC3_ASSOC_CUCKOO_WIDTH; idx ++) {
        if (idx< MEMC3_ASSOC_CUCKOO_WIDTH/2) 
            cp_buckets[depth][idx] = i1;
        else
            cp_buckets[depth][idx] = i2;
    }
    size_t j;
    idx = cuckoo(depth);
    if (idx >= 0) {
        i1 = cp_buckets[depth][idx];
        j = cp_slots[depth][idx];
        if (buckets[i1].vals[j] != 0)
            printf("ououou\n");
        if (try_add(it, tag, i1, lock))
            return 1;
        printf("mmm i1=%zu i=%d\n", i1, idx);
    }

    printf("hash table is full (hashpower = %d, hash_items = %u, load factor = %.2f), need to increase hashpower\n",
           hashpower, hash_items, 1.0 * hash_items / bucket_size / hashsize);
    return 0;
}    

static bool try_del(const char*key, const size_t nkey, TagType tag, size_t i, size_t lock) {
    for (size_t j = 0; j < bucket_size; j ++) {
#ifdef MEMC3_ENABLE_TAG
        //if (IS_TAG_EQUAL(i, j, tag))
        if (IS_TAG_EQUAL(buckets[i], j, tag))
#endif
        {
            item *it = buckets[i].vals[j];

#ifndef MEMC3_ENABLE_TAG
            if (it == NULL)
                return false;
#endif

            if (keycmp(key, ITEM_key(it), nkey)) {

#ifdef MEMC3_LOCK_OPT
                incr_keyver(lock);
#endif

#ifdef MEMC3_LOCK_FINEGRAIN
                fg_lock(i, i);
#endif

                buckets[i].tags[j] = 0;
                buckets[i].vals[j] = 0;
                hash_items --;

#ifdef MEMC3_LOCK_OPT
                incr_keyver(lock);
#endif

#ifdef MEMC3_LOCK_FINEGRAIN
                fg_unlock(i, i);
#endif

                return true;
            }
        }
    }
    return false;
}

// need to be protected by cache_lock
void assoc2_delete(const char *key, const size_t nkey, const uint32_t hv) {

    TagType tag = _tag_hash(hv);
    size_t   i1 = _index_hash(hv);
    size_t   i2 = _alt_index(i1, tag);
    size_t lock = _lock_index(i1, i2, tag);


    if (try_del(key, nkey, tag, i1, lock))
        return;

    if (try_del(key, nkey, tag, i2, lock))
        return;


    /* Note:  we never actually get here.  the callers don't delete things
       they can't find. */
    assert(false);
}


void assoc2_pre_bench() {
    num_moves = 0;
}

void assoc2_post_bench() {

    size_t total_size = 0;

    printf("hash_items = %u\n", hash_items);
    printf("index table size = %zu\n", hashsize);
    printf("hashtable size = %zu KB\n",hashsize*sizeof(struct Bucket)/1024);
    printf("hashtable load factor= %.5f\n", 1.0 * hash_items / bucket_size / hashsize);
    total_size += hashsize*sizeof(struct Bucket);
    printf("total_size = %zu KB\n", total_size / 1024);
#ifdef PRINT_LF
    printf("moves per insert = %.2f\n", (double) num_moves / hash_items);
#endif
    printf("\n");
}

