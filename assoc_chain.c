/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 * Hash table
 *
 * The hash function used here is by Bob Jenkins, 1996:
 *    <http://burtleburtle.net/bob/hash/doobs.html>
 *       "By Bob Jenkins, 1996.  bob_jenkins@burtleburtle.net.
 *       You may use this code any way you wish, private, educational,
 *       or commercial.  It's free."
 *
 * The rest of the file is licensed under the BSD license.  See LICENSE.
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

#ifdef MEMC3_ASSOC_CHAIN

static pthread_cond_t maintenance_cond = PTHREAD_COND_INITIALIZER;


/* how many powers of 2's worth of buckets we use */
//static unsigned int hashpower = HASHPOWER_DEFAULT;

#define hashsize(n) ((unsigned int)1<<(n))
#define hashmask(n) (hashsize(n)-1)

/* Main hash table. This is where we look except during expansion. */
static item** primary_hashtable = 0;

/*
 * Previous hash table. During expansion, we look here for keys that haven't
 * been moved over to the primary yet.
 */
static item** old_hashtable = 0;

/* Number of items in the hash table. */
static unsigned int hash_items = 0;

/* Flag: Are we in the middle of expanding now? */
static bool expanding = false;

/*
 * During expansion we migrate values with bucket granularity; this is how
 * far we've gotten so far. Ranges from 0 .. hashsize(hashpower - 1) - 1.
 */
static unsigned int expand_bucket = 0;

static size_t total_steps = 0;
#ifdef COUNT_LARGEST_BUCKET
int* bucket_size;
int largest_bucket = 0;
#endif

void assoc_init(const int hashtable_init) {
    hashpower = HASHPOWER_DEFAULT;
    if (hashtable_init) {
        hashpower = hashtable_init;
    }
    // added by Bin: comment original calloc, use our impl in bench_util
    //primary_hashtable = calloc(hashsize(hashpower), sizeof(void *));
    primary_hashtable = alloc(hashsize(hashpower) * sizeof(void *));
    if (! primary_hashtable) {
        fprintf(stderr, "Failed to init hashtable.\n");
        exit(EXIT_FAILURE);
    }
#ifdef COUNT_LARGEST_BUCKET
    bucket_size = calloc(hashsize(hashpower), sizeof(int));
    if (! bucket_size) {
        fprintf(stderr, "Failed to init bucket_size.\n");
        exit(EXIT_FAILURE);
    }
    memset(bucket_size, 0, hashsize(hashpower));
#endif


    STATS_LOCK();
    stats.hash_power_level = hashpower;
    stats.hash_bytes = hashsize(hashpower) * sizeof(void *);
    STATS_UNLOCK();
}


item *assoc_find(const char *key, const size_t nkey, const uint32_t hv) {
    item *it;
    unsigned int oldbucket;

    if (expanding &&
        (oldbucket = (hv & hashmask(hashpower - 1))) >= expand_bucket)
    {
        it = old_hashtable[oldbucket];
    } else {
        it = primary_hashtable[hv & hashmask(hashpower)];
    }

    item *ret = NULL;
    int depth = 0;
    while (it) {
        if ((nkey == it->nkey) && (keycmp(key, ITEM_key(it), nkey) )) {
            ret = it;
            break;
        }
        it = it->h_next;
        ++depth;
    }

    total_steps += depth;

    MEMCACHED_ASSOC_FIND(key, nkey, depth);
    return ret;
}

/* returns the address of the item pointer before the key.  if *item == 0,
   the item wasn't found */

static item** _hashitem_before (const char *key, const size_t nkey, const uint32_t hv) {
    item **pos;
    unsigned int oldbucket;

    if (expanding &&
        (oldbucket = (hv & hashmask(hashpower - 1))) >= expand_bucket)
    {
        pos = &old_hashtable[oldbucket];
    } else {
        pos = &primary_hashtable[hv & hashmask(hashpower)];
    }

    while (*pos && ((nkey != (*pos)->nkey) || memcmp(key, ITEM_key(*pos), nkey))) {
        pos = &(*pos)->h_next;
    }
    return pos;
}

// commented by Bin
/* grows the hashtable to the next power of 2. */
/* static void assoc_expand(void) { */
/*     old_hashtable = primary_hashtable; */

/*     // added by Bin: comment original calloc, use our impl in bench_util */
/*     //primary_hashtable = calloc(hashsize(hashpower + 1), sizeof(void *)); */
/*     primary_hashtable = alloc(hashsize(hashpower + 1) * sizeof(void *)); */
/*     if (primary_hashtable) { */
/*         if (settings.verbose > 1) */
/*             fprintf(stderr, "Hash table expansion starting\n"); */
/*         hashpower++; */
/*         expanding = true; */
/*         expand_bucket = 0; */
/*         STATS_LOCK(); */
/*         stats.hash_power_level = hashpower; */
/*         stats.hash_bytes += hashsize(hashpower) * sizeof(void *); */
/*         stats.hash_is_expanding = 1; */
/*         STATS_UNLOCK(); */
/*         pthread_cond_signal(&maintenance_cond); */
/*     } else { */
/*         primary_hashtable = old_hashtable; */
/*         /\* Bad news, but we can keep running. *\/ */
/*     } */
/* } */

/* Note: this isn't an assoc_update.  The key must not already exist to call this */
int assoc_insert(item *it, const uint32_t hv) {
    unsigned int oldbucket;

//    assert(assoc_find(ITEM_key(it), it->nkey) == 0);  /* shouldn't have duplicately named things defined */
    // commented by Bin
    /* if (assoc_find(ITEM_key(it), it->nkey, hv) != 0) { */
    /*     printf("see duplicate keys"); */
    /* } */


    if (expanding &&
        (oldbucket = (hv & hashmask(hashpower - 1))) >= expand_bucket)
    {
        it->h_next = old_hashtable[oldbucket];
        old_hashtable[oldbucket] = it;
    } else {
        it->h_next = primary_hashtable[hv & hashmask(hashpower)];
        primary_hashtable[hv & hashmask(hashpower)] = it;
#ifdef COUNT_LARGEST_BUCKET
        bucket_size[hv & hashmask(hashpower)] ++;
        if (bucket_size[hv & hashmask(hashpower)] > largest_bucket) {
            largest_bucket = bucket_size[hv & hashmask(hashpower)];
        }
#endif
    }

    hash_items++;
    // added by Bin:
    /* if ((hash_items) && (hash_items % 1000000 == 0)) { */
    /*     printf("%u Million items inserted!\n", hash_items / 1000000); */
    /* }  */

    if (! expanding && hash_items > (hashsize(hashpower) * 3) / 2) {
        // commented by Bin
        //assoc_expand();
        // added by Bin
        /* perror("can not insert!\n"); */
        /* exit(1); */
        printf("hash table is full (hashpower = %d, hash_items = %u,), need to increase hashpower\n",
               hashpower, hash_items);
        return 0;
    }

    MEMCACHED_ASSOC_INSERT(ITEM_key(it), it->nkey, hash_items);
    return 1;
}

void assoc_delete(const char *key, const size_t nkey, const uint32_t hv) {
    item **before = _hashitem_before(key, nkey, hv);

    if (*before) {
        item *nxt;
        hash_items--;
        // added by Bin:
        //printf("one key removed, %u left\n", hash_items);

        /* The DTrace probe cannot be triggered as the last instruction
         * due to possible tail-optimization by the compiler
         */
        MEMCACHED_ASSOC_DELETE(key, nkey, hash_items);
        nxt = (*before)->h_next;
        (*before)->h_next = 0;   /* probably pointless, but whatever. */
        *before = nxt;
        return;
    }
    /* Note:  we never actually get here.  the callers don't delete things
       they can't find. */
    assert(*before != 0);
}


static volatile int do_run_maintenance_thread = 1;

#define DEFAULT_HASH_BULK_MOVE 1
int hash_bulk_move = DEFAULT_HASH_BULK_MOVE;

static void *assoc_maintenance_thread(void *arg) {

    while (do_run_maintenance_thread) {
        int ii = 0;

        /* Lock the cache, and bulk move multiple buckets to the new
         * hash table. */
        mutex_lock(&cache_lock);

        for (ii = 0; ii < hash_bulk_move && expanding; ++ii) {
            item *it, *next;
            int bucket;

            for (it = old_hashtable[expand_bucket]; NULL != it; it = next) {
                next = it->h_next;

                bucket = hash(ITEM_key(it), it->nkey, 0) & hashmask(hashpower);
                it->h_next = primary_hashtable[bucket];
                primary_hashtable[bucket] = it;
            }

            old_hashtable[expand_bucket] = NULL;

            expand_bucket++;
            if (expand_bucket == hashsize(hashpower - 1)) {
                expanding = false;
                free(old_hashtable);
                STATS_LOCK();
                stats.hash_bytes -= hashsize(hashpower - 1) * sizeof(void *);
                stats.hash_is_expanding = 0;
                STATS_UNLOCK();
                if (settings.verbose > 1)
                    fprintf(stderr, "Hash table expansion done\n");
            }
        }

        if (!expanding) {
            // added by Bin:
            fprintf(stderr, "\nHash table expansion done\n");
            //assoc_pre_bench();
            //assoc_post_bench();

            /* We are done expanding.. just wait for next invocation */
            pthread_cond_wait(&maintenance_cond, &cache_lock);
        }

        pthread_mutex_unlock(&cache_lock);
    }
    return NULL;
}

static pthread_t maintenance_tid;

int start_assoc_maintenance_thread() {
    int ret;
    char *env = getenv("MEMCACHED_HASH_BULK_MOVE");
    if (env != NULL) {
        hash_bulk_move = atoi(env);
        if (hash_bulk_move == 0) {
            hash_bulk_move = DEFAULT_HASH_BULK_MOVE;
        }
    }
    if ((ret = pthread_create(&maintenance_tid, NULL,
                              assoc_maintenance_thread, NULL)) != 0) {
        fprintf(stderr, "Can't create thread: %s\n", strerror(ret));
        return -1;
    }
    return 0;
}

void stop_assoc_maintenance_thread() {
    mutex_lock(&cache_lock);
    do_run_maintenance_thread = 0;
    pthread_cond_signal(&maintenance_cond);
    pthread_mutex_unlock(&cache_lock);

    /* Wait for the maintenance thread to stop */
    pthread_join(maintenance_tid, NULL);
}


void assoc_destroy() {
    dealloc(primary_hashtable);
}


//added by Bin
void assoc_pre_bench() {
    total_steps = 0;
}

void assoc_post_bench() {
    size_t total_size = 0;
    printf("hashtable stats:\n");
    printf("\thashpower = %u\n", hashpower);
    printf("\thash_items = %u\n", hash_items);
    /* printf("\thashsize = %u\n", hashsize(hashpower)); */
    /* printf("\thashmask = %u\n", hashmask(hashpower)); */
    /* printf("\thashtable size = %zu KB\n",(hashsize(hashpower) + hash_items)  * sizeof(void *)/1024); */
    
    total_size = (hashsize(hashpower) + hash_items)  * sizeof(void *);
    printf("\ttotal_size= %zu KB\n",(total_size)/1024);
#ifdef COUNT_LARGEST_BUCKET
    printf("\tlargest_bucket = %d\n", largest_bucket);
#endif
    printf("\tup to %u items\n", hashsize(hashpower) * 3 / 2);
    printf("\ttotal steps = %zu\n", total_steps);

    printf("\n");
}


#else
/* the only reason to do this is to make compiler happ */
void assoc_init(const int hashpower_init) {}
item *assoc_find(const char *key, const size_t nkey, const uint32_t hv) {return NULL;}
int assoc_insert(item *item, const uint32_t hv) {return 1;}
void assoc_delete(const char *key, const size_t nkey, const uint32_t hv) {return;}
void do_assoc_move_next_bucket() {return;}
int start_assoc_maintenance_thread() {return 1;}
void stop_assoc_maintenance_thread() {return;}


// added to keep same API as other hashtable impl
void assoc_destroy() {return;}
void assoc_pre_bench() {return;}
void assoc_post_bench() {return;}
#endif
