/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
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
#include <time.h>
#include <assert.h>

/* Forward Declarations */
static void item_link_q(item *it);
static void item_unlink_q(item *it);

/*
 * We only reposition items in the LRU queue if they haven't been repositioned
 * in this many seconds. That saves us from churning on frequently-accessed
 * items.
 */
#define ITEM_UPDATE_INTERVAL 60

#define LARGEST_ID POWER_LARGEST
typedef struct {
    uint64_t evicted;
    uint64_t evicted_nonzero;
    rel_time_t evicted_time;
    uint64_t reclaimed;
    uint64_t outofmemory;
    uint64_t tailrepairs;
    uint64_t expired_unfetched;
    uint64_t evicted_unfetched;
} itemstats_t;

#ifdef MEMC3_CACHE_LRU
static item *heads[LARGEST_ID];
static item *tails[LARGEST_ID];
static unsigned int sizes[LARGEST_ID];
#endif
static itemstats_t itemstats[LARGEST_ID];


void item_stats_reset(void) {
    mutex_lock(&cache_lock);
    memset(itemstats, 0, sizeof(itemstats));
    mutex_unlock(&cache_lock);
}


/* Get the next CAS id for a new item. */
uint64_t get_cas_id(void) {
    static uint64_t cas_id = 0;
    return ++cas_id;
}

/* Enable this for reference-count debugging. */
#if 0
# define DEBUG_REFCNT(it,op) \
                fprintf(stderr, "item %p refcnt(%d) %d %c%c\n", \
                        (void*)it, op, it->refcount,              \
                        (it->it_flags & ITEM_LINKED) ? 'L' : ' ', \
                        (it->it_flags & ITEM_SLABBED) ? 'S' : ' ')
#else
# define DEBUG_REFCNT(it,op) while(0)
#endif


/**
 * Generates the variable-sized part of the header for an object.
 *
 * key     - The key
 * nkey    - The length of the key
 * flags   - key flags
 * nbytes  - Number of bytes to hold value and addition CRLF terminator
 * suffix  - Buffer for the "VALUE" line suffix (flags, size).
 * nsuffix - The length of the suffix is stored here.
 *
 * Returns the total size of the header.
 */
static size_t item_make_header(const uint8_t nkey, const int flags, const int nbytes,
                     char *suffix, uint8_t *nsuffix) {
    /* suffix is defined at 40 chars elsewhere.. */
    *nsuffix = (uint8_t) snprintf(suffix, 40, " %d %d\r\n", flags, nbytes - 2);
    return sizeof(item) + nkey + *nsuffix + nbytes;
}

/*@null@*/
item *do_item_alloc(char *key, const size_t nkey, const int flags, const rel_time_t exptime, const int nbytes) {
    uint8_t nsuffix;
    item *it = NULL;
    char suffix[40];
    size_t ntotal = item_make_header(nkey + 1, flags, nbytes, suffix, &nsuffix);
    if (settings.use_cas) {
        ntotal += sizeof(uint64_t);
    }

    unsigned int id = slabs_clsid(ntotal);
    if (id == 0)
        return 0;

    mutex_lock(&cache_lock);
    if ((it = slabs_alloc(ntotal, id)) == NULL) {
        item *search = NULL;
#ifdef MEMC3_CACHE_LRU
        search = tails[id];
#endif

#ifdef MEMC3_CACHE_CLOCK
        search = slabs_cache_evict(id);
#endif

        if ((search == NULL) || ((search->it_flags & ITEM_LINKED) != 1)) {
            itemstats[id].outofmemory++;
            return NULL;
        }
            
        it = search;
        itemstats[id].evicted++;
        itemstats[id].evicted_time = current_time - it->time;
        if (it->exptime != 0)
            itemstats[id].evicted_nonzero++;
        if ((it->it_flags & ITEM_FETCHED) == 0) {
            STATS_LOCK();
            stats.evicted_unfetched++;
            STATS_UNLOCK();
            itemstats[id].evicted_unfetched++;
        }
        STATS_LOCK();
        stats.evictions++;
        STATS_UNLOCK();
        slabs_adjust_mem_requested(it->slabs_clsid, ITEM_ntotal(it), ntotal);

        uint32_t old_hv = hash(ITEM_key(it), it->nkey, 0);

#ifdef MEMC3_LOCK_OPT
        TagType tag = _tag_hash(old_hv);
        size_t i1   = _index_hash(old_hv);
        size_t i2   = _alt_index(i1, tag);
        size_t lock = _lock_index(i1, i2, tag);        
        incr_keyver(lock);
#endif

        do_item_unlink_nolock(it, old_hv);
        /* Initialize the item block: */
        it->slabs_clsid = 0;

#ifdef MEMC3_LOCK_OPT
        incr_keyver(lock);
#endif
    }

    assert(it->slabs_clsid == 0);
#ifdef MEMC3_CACHE_LRU
    assert(it != heads[id]);
#endif
    
    /* Item initialization can happen outside of the lock; the item's already
     * been removed from the slab LRU.
     */
    //it->refcount = 1;     /* the caller will have a reference */
    mutex_unlock(&cache_lock);
#ifdef MEMC3_CACHE_LRU
    it->next = it->prev = 0;
#endif
#ifdef MEMC3_ASSOC_CHAIN
    it->h_next = 0;
#endif
    it->slabs_clsid = id;

    DEBUG_REFCNT(it, '*');

    it->it_flags = settings.use_cas ? ITEM_CAS : 0;
    it->nkey = nkey;
    it->nbytes = nbytes;
    memcpy(ITEM_key(it), key, nkey);
    it->exptime = exptime;
    memcpy(ITEM_suffix(it), suffix, (size_t)nsuffix);
    it->nsuffix = nsuffix;

    assert((it->it_flags & ITEM_LINKED) == 0);
    assert((it->slabs_clsid > 0));
    return it;
}

void item_free(item *it) {
    size_t ntotal = ITEM_ntotal(it);
    unsigned int clsid;
    assert((it->it_flags & ITEM_LINKED) == 0);
#ifdef MEMC3_CACHE_LRU
    assert(it != heads[it->slabs_clsid]);
    assert(it != tails[it->slabs_clsid]);
#endif
    //assert(it->refcount == 0);

    //before_write(it);
    /* so slab size changer can tell later if item is already free or not */
    clsid = it->slabs_clsid;
    it->slabs_clsid = 0;
    DEBUG_REFCNT(it, 'F');
    slabs_free(it, ntotal, clsid);
    //after_write(it);
}

/**
 * Returns true if an item will fit in the cache (its size does not exceed
 * the maximum for a cache entry.)
 */
bool item_size_ok(const size_t nkey, const int flags, const int nbytes) {
    char prefix[40];
    uint8_t nsuffix;

    size_t ntotal = item_make_header(nkey + 1, flags, nbytes,
                                     prefix, &nsuffix);
    if (settings.use_cas) {
        ntotal += sizeof(uint64_t);
    }

    return slabs_clsid(ntotal) != 0;
}

static void item_link_q(item *it) { /* item is the new head */
#ifdef MEMC3_CACHE_LRU
    item **head, **tail;
    assert(it->slabs_clsid < LARGEST_ID);
    assert((it->it_flags & ITEM_SLABBED) == 0);

    head = &heads[it->slabs_clsid];
    tail = &tails[it->slabs_clsid];
    assert(it != *head);
    assert((*head && *tail) || (*head == 0 && *tail == 0));
    it->prev = 0;
    it->next = *head;
    if (it->next) it->next->prev = it;
    *head = it;
    if (*tail == 0) *tail = it;
    sizes[it->slabs_clsid]++;
    return;
#endif
}

static void item_unlink_q(item *it) {
#ifdef MEMC3_CACHE_LRU
    item **head, **tail;
    assert(it->slabs_clsid < LARGEST_ID);
    head = &heads[it->slabs_clsid];
    tail = &tails[it->slabs_clsid];

    if (*head == it) {
        assert(it->prev == 0);
        *head = it->next;
    }
    if (*tail == it) {
        assert(it->next == 0);
        *tail = it->prev;
    }
    assert(it->next != it);
    assert(it->prev != it);

    if (it->next) it->next->prev = it->prev;
    if (it->prev) it->prev->next = it->next;
    sizes[it->slabs_clsid]--;
    return;
#endif
}

int do_item_link(item *it, const uint32_t hv) {
    MEMCACHED_ITEM_LINK(ITEM_key(it), it->nkey, it->nbytes);

    //mutex_lock(&cache_lock);
    do_item_link_nolock(it, hv);
    //mutex_unlock(&cache_lock);

    return 1;
}

int do_item_link_nolock(item *it, const uint32_t hv) {
    assert((it->it_flags & (ITEM_LINKED|ITEM_SLABBED)) == 0);
    //it->it_flags |= ITEM_LINKED;
    __sync_fetch_and_or(&it->it_flags, ITEM_LINKED);

    it->time = current_time;

    STATS_LOCK();
    stats.curr_bytes += ITEM_ntotal(it);
    stats.curr_items += 1;
    stats.total_items += 1;
    STATS_UNLOCK();

    /* Allocate a new CAS ID on link. */
    ITEM_set_cas(it, (settings.use_cas) ? get_cas_id() : 0);
    int ret;
#ifdef MEMC3_ASSOC_CHAIN
    ret = assoc_insert(it, hv);
#endif

#ifdef MEMC3_ASSOC_CUCKOO
    ret = assoc2_insert(it, hv);
#endif
    if (ret == 0) {
        assert(false);
    }

    item_link_q(it);
    //refcount_incr(&it->refcount);
    //after_write(it);
    assert((it->slabs_clsid > 0));
    return 1;
}

void do_item_unlink(item *it, const uint32_t hv) {
    MEMCACHED_ITEM_UNLINK(ITEM_key(it), it->nkey, it->nbytes);
    mutex_lock(&cache_lock);
    do_item_unlink_nolock(it, hv);
    mutex_unlock(&cache_lock);
}

/* FIXME: Is it necessary to keep this copy/pasted code? */
void do_item_unlink_nolock(item *it, const uint32_t hv) {
    MEMCACHED_ITEM_UNLINK(ITEM_key(it), it->nkey, it->nbytes);
    if ((it->it_flags & ITEM_LINKED) != 0) {

        //it->it_flags &= ~ITEM_LINKED;
        __sync_fetch_and_and(&it->it_flags, ~ITEM_LINKED);
        
        STATS_LOCK();
        stats.curr_bytes -= ITEM_ntotal(it);
        stats.curr_items -= 1;
        STATS_UNLOCK();
        
#ifdef MEMC3_ASSOC_CHAIN
        assoc_delete(ITEM_key(it), it->nkey, hv);
#endif

#ifdef MEMC3_ASSOC_CUCKOO
        assoc2_delete(ITEM_key(it), it->nkey, hv);
#endif
        item_unlink_q(it);
        do_item_remove(it);
    }
}

void do_item_remove(item *it) {
    MEMCACHED_ITEM_REMOVE(ITEM_key(it), it->nkey, it->nbytes);
    
    //refcount_decr(&it->refcount);
    return;
    /* assert((it->it_flags & ITEM_SLABBED) == 0); */
    /* if (refcount_decr(&it->refcount) == 0) { */
    /*     item_free(it); */
    /* } */
}

void do_item_update(item *it) {
    MEMCACHED_ITEM_UPDATE(ITEM_key(it), it->nkey, it->nbytes);
    if (it->time < current_time - ITEM_UPDATE_INTERVAL) {

#ifdef MEMC3_CACHE_LRU
        mutex_lock(&cache_lock);
        //assert((it->it_flags & ITEM_SLABBED) == 0);
        if ((it->it_flags & ITEM_LINKED) != 0) {
            assert((it->slabs_clsid > 0));
            //before_write(it);
            item_unlink_q(it);
            it->time = current_time;
            item_link_q(it);
            //after_write(it);
        }
        mutex_unlock(&cache_lock);
#endif

#ifdef MEMC3_CACHE_CLOCK
        slabs_cache_update(it);
#endif

    }
}

int do_item_replace(item *it, item *new_it, const uint32_t hv) {
    MEMCACHED_ITEM_REPLACE(ITEM_key(it), it->nkey, it->nbytes,
                           ITEM_key(new_it), new_it->nkey, new_it->nbytes);
    assert((it->it_flags & ITEM_SLABBED) == 0);
    assert(false);
    do_item_unlink(it, hv);
    return do_item_link(new_it, hv);
}

/*@null@*/
char *do_item_cachedump(const unsigned int slabs_clsid, const unsigned int limit, unsigned int *bytes) {
    // removed by Bin
    return NULL;
}

void item_stats_evictions(uint64_t *evicted) {
    // removed by Bin
}

void do_item_stats(ADD_STAT add_stats, void *c) {
    // removed by Bin
}

/** dumps out a list of objects of each size, with granularity of 32 bytes */
/*@null@*/
void do_item_stats_sizes(ADD_STAT add_stats, void *c) {
    // removed by Bin
}

/** wrapper around assoc_find which does the lazy expiration logic */
item *do_item_get(const char *key, const size_t nkey, const uint32_t hv) {

#ifdef MEMC3_ASSOC_CHAIN
    //mutex_lock(&cache_lock);
    item *it = assoc_find(key, nkey, hv);
    //mutex_unlock(&cache_lock);
#endif

#ifdef MEMC3_ASSOC_CUCKOO
    item *it = assoc2_find(key, nkey, hv);
#endif

    if (it != NULL) {
        //refcount_incr(&it->refcount);
        //it->it_flags |= ITEM_FETCHED;
        __sync_fetch_and_or(&it->it_flags, ITEM_FETCHED);
        //assert(memcmp(ITEM_key(it), key, nkey) == 0);
    }

    return it;
}

item *do_item_touch(const char *key, size_t nkey, uint32_t exptime,
                    const uint32_t hv) {
    item *it = do_item_get(key, nkey, hv);
    if (it != NULL) {
        it->exptime = exptime;
    }
    return it;
}

/* expires items that are more recent than the oldest_live setting. */
void do_item_flush_expired(void) {
    // removed by Bin
}
