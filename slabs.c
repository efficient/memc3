/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 * Slabs memory allocation, based on powers-of-N. Slabs are up to 1MB in size
 * and are divided into chunks. The chunk sizes start off at the size of the
 * "item" structure plus space for a small key and value. They increase by
 * a multiplier factor from there, up to half the maximum slab size. The last
 * slab size is always 1MB, since that's the maximum item size allowed by the
 * memcached protocol.
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

/* powers-of-N allocation structures */

typedef struct {
    unsigned int size;      /* sizes of items */
    unsigned int perslab;   /* how many items per slab */

    void *slots;           /* list of item ptrs */
    unsigned int sl_curr;   /* total free items in list */

    void *end_page_ptr;         /* pointer to next free item at end of page, or 0 */
    unsigned int end_page_free; /* number of items remaining at end of last alloced page */

    unsigned int slabs;     /* how many slabs were allocated for this class */

    void **slab_list;       /* array of slab pointers */
    unsigned int list_size; /* size of prev array */

    unsigned int killing;  /* index+1 of dying slab, or zero if none */
    size_t requested; /* The number of requested bytes */

#ifdef MEMC3_CACHE_CLOCK
    /* added by Bin: the bitmap used for clock */
    void *bitmap;  
    unsigned int bitmap_len;
    unsigned int clock;
    unsigned int clock_max;
#endif

} slabclass_t;

static slabclass_t slabclass[MAX_NUMBER_OF_SLAB_CLASSES];
static size_t mem_limit = 0;
static size_t mem_malloced = 0;
static int power_largest;

static void *mem_base = NULL;
static void *mem_current = NULL;
static size_t mem_avail = 0;

/**
 * Access to the slab allocator is protected by this lock
 */
static pthread_mutex_t slabs_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 * Forward Declarations
 */
static int do_slabs_newslab(const unsigned int id);
static void *memory_allocate(size_t size);

#ifndef DONT_PREALLOC_SLABS
/* Preallocate as many slab pages as possible (called from slabs_init)
   on start-up, so users don't get confused out-of-memory errors when
   they do have free (in-slab) space, but no space to make new slabs.
   if maxslabs is 18 (POWER_LARGEST - POWER_SMALLEST + 1), then all
   slab types can be made.  if max memory is less than 18 MB, only the
   smaller ones will be made.  */
static void slabs_preallocate (const unsigned int maxslabs);
#endif

/*
 * Figures out which slab class (chunk size) is required to store an item of
 * a given size.
 *
 * Given object size, return id to use when allocating/freeing memory for object
 * 0 means error: can't store such a large object
 */

unsigned int slabs_clsid(const size_t size) {
    int res = POWER_SMALLEST;

    if (size == 0)
        return 0;
    while (size > slabclass[res].size)
        if (res++ == power_largest)     /* won't fit in the biggest slab */
            return 0;
    return res;
}

/**
 * Determines the chunk sizes and initializes the slab class descriptors
 * accordingly.
 */
void slabs_init(const size_t limit, const double factor, const bool prealloc) {
    int i = POWER_SMALLEST - 1;
    unsigned int size = sizeof(item) + settings.chunk_size;

    mem_limit = limit;

    if (prealloc) {
        /* Allocate everything in a big chunk with malloc */
        //mem_base = malloc(mem_limit);
        mem_base = alloc(mem_limit);
        if (mem_base != NULL) {
            mem_current = mem_base;
            mem_avail = mem_limit;
        } else {
            fprintf(stderr, "Warning: Failed to allocate requested memory in"
                    " one large chunk.\nWill allocate in smaller chunks\n");
        }
    }

    memset(slabclass, 0, sizeof(slabclass));

    while (++i < POWER_LARGEST && size <= settings.item_size_max / factor) {
        /* Make sure items are always n-byte aligned */
        if (size % CHUNK_ALIGN_BYTES)
            size += CHUNK_ALIGN_BYTES - (size % CHUNK_ALIGN_BYTES);

        slabclass[i].size = size;
        slabclass[i].perslab = settings.item_size_max / slabclass[i].size;
        size *= factor;
        if (settings.verbose > 1) {
            fprintf(stderr, "slab class %3d: chunk size %9u perslab %7u\n",
                    i, slabclass[i].size, slabclass[i].perslab);
        }
    }

    power_largest = i;
    slabclass[power_largest].size = settings.item_size_max;
    slabclass[power_largest].perslab = 1;
    if (settings.verbose > 1) {
        fprintf(stderr, "slab class %3d: chunk size %9u perslab %7u\n",
                i, slabclass[i].size, slabclass[i].perslab);
    }

    /* for the test suite:  faking of how much we've already malloc'd */
    {
        char *t_initial_malloc = getenv("T_MEMD_INITIAL_MALLOC");
        if (t_initial_malloc) {
            mem_malloced = (size_t)atol(t_initial_malloc);
        }

    }

#ifndef DONT_PREALLOC_SLABS
    {
        char *pre_alloc = getenv("T_MEMD_SLABS_ALLOC");

        if (pre_alloc == NULL || atoi(pre_alloc) != 0) {
            slabs_preallocate(power_largest);
        }
    }
#endif
}

#ifndef DONT_PREALLOC_SLABS
static void slabs_preallocate (const unsigned int maxslabs) {
    int i;
    unsigned int prealloc = 0;

    /* pre-allocate a 1MB slab in every size class so people don't get
       confused by non-intuitive "SERVER_ERROR out of memory"
       messages.  this is the most common question on the mailing
       list.  if you really don't want this, you can rebuild without
       these three lines.  */

    for (i = POWER_SMALLEST; i <= POWER_LARGEST; i++) {
        if (++prealloc > maxslabs)
            return;
        do_slabs_newslab(i);
    }

}
#endif

static int grow_slab_list (const unsigned int id) {
    slabclass_t *p = &slabclass[id];
    if (p->slabs == p->list_size) {
        size_t new_size =  (p->list_size != 0) ? p->list_size * 2 : 16;
        void *new_list = realloc(p->slab_list, new_size * sizeof(void *));
        if (new_list == 0) return 0;
#ifdef MEMC3_CACHE_CLOCK
        // added by Bin:
        //size_t old_bitmap_len = p->bitmap_len;
        size_t new_bitmap_len = (new_size * p->perslab + 7) / 8;
        void *new_bitmap = realloc(p->bitmap, new_bitmap_len);
        if (new_bitmap == 0) return 0;
        
        //memset((char*) new_bitmap + old_bitmap_len, 0, new_bitmap_len - old_bitmap_len);
        p->bitmap_len = new_bitmap_len;
        p->bitmap = new_bitmap;
#endif
        p->list_size = new_size;
        p->slab_list = new_list;
    }
    return 1;
}

static int do_slabs_newslab(const unsigned int id) {
    slabclass_t *p = &slabclass[id];
    int len = settings.slab_reassign ? settings.item_size_max
        : p->size * p->perslab;
    char *ptr;

    if ((mem_limit && mem_malloced + len > mem_limit && p->slabs > 0) ||
        (grow_slab_list(id) == 0) ||
        ((ptr = memory_allocate((size_t)len)) == 0)) {

        MEMCACHED_SLABS_SLABCLASS_ALLOCATE_FAILED(id);
        return 0;
    }

    memset(ptr, 0, (size_t)len);
    p->end_page_ptr = ptr;
    p->end_page_free = p->perslab;

    p->slab_list[p->slabs++] = ptr;
#ifdef MEMC3_CACHE_CLOCK
    p->clock_max = p->slabs * p->perslab;
#endif
    mem_malloced += len;
    MEMCACHED_SLABS_SLABCLASS_ALLOCATE(id);

    return 1;
}

/*@null@*/
static void *do_slabs_alloc(const size_t size, unsigned int id) {
    slabclass_t *p;
    void *ret = NULL;

    if (id < POWER_SMALLEST || id > power_largest) {
        MEMCACHED_SLABS_ALLOCATE_FAILED(size, 0);
        return NULL;
    }

    p = &slabclass[id];


    /* fail unless we have space at the end of a recently allocated page,
       we have something on our freelist, or we could allocate a new page */
    if (! (p->end_page_ptr != 0 || p->sl_curr != 0 ||
           do_slabs_newslab(id) != 0)) {
        /* We don't have more memory available */
        ret = NULL;
    } else if (p->sl_curr != 0) {
        /* return off our freelist */
#ifdef MEMC3_CACHE_LRU
        item *it = (item *)p->slots;
        p->slots = it->next;
        if (it->next) it->next->prev = 0;
        p->sl_curr--;
        ret = (void *)it;
#endif

#ifdef MEMC3_CACHE_CLOCK
        /* return off our freelist */
        slabbed_item* sl_it = (slabbed_item *)p->slots;
        p->slots = sl_it->next;
        if (sl_it->next) sl_it->next->prev = 0;
        p->sl_curr--;
        ret = (void *)sl_it;
#endif
    } else {
        /* if we recently allocated a whole page, return from that */
        assert(p->end_page_ptr != NULL);
        ret = p->end_page_ptr;
        if (--p->end_page_free != 0) {
            p->end_page_ptr = ((caddr_t)p->end_page_ptr) + p->size;
        } else {
            p->end_page_ptr = 0;
        }
        /* item *it = (item *)ret; */
        /* reset_version(it); */
    }

    if (ret) {
        p->requested += size;
        MEMCACHED_SLABS_ALLOCATE(size, id, p->size, ret);
    } else {
        MEMCACHED_SLABS_ALLOCATE_FAILED(size, id);
    }

    return ret;
}

static void do_slabs_free(void *ptr, const size_t size, unsigned int id) {
    slabclass_t *p;

    assert(((item *)ptr)->slabs_clsid == 0);
    assert(id >= POWER_SMALLEST && id <= power_largest);
    if (id < POWER_SMALLEST || id > power_largest)
        return;

    MEMCACHED_SLABS_FREE(size, id, ptr);
    p = &slabclass[id];

#ifdef USE_SYSTEM_MALLOC
    mem_malloced -= size;
    free(ptr);
    return;
#endif

#ifdef MEMC3_CACHE_LRU
    item *it = (item *)ptr;
    //it->it_flags |= ITEM_SLABBED;
    __sync_fetch_and_or(&it->it_flags, ITEM_SLABBED);
    it->prev = 0;
    it->next = p->slots;
    if (it->next) it->next->prev = it;
    p->slots = it;
#endif

#ifdef MEMC3_CACHE_CLOCK
    slabbed_item *sl_it = (slabbed_item *)ptr;
    //sl_it->it_flags |= ITEM_SLABBED;
    __sync_fetch_and_or(&sl_it->it_flags, ITEM_SLABBED);
    sl_it->prev = 0;
    sl_it->next = p->slots;
    if (sl_it->next) sl_it->next->prev = sl_it;
    p->slots = sl_it;
#endif

    p->sl_curr++;
    p->requested -= size;
    return;
}

bool get_stats(const char *stat_type, int nkey, ADD_STAT add_stats, void *c) {
    bool ret = true;

    // removed by Bin
    return ret;
}

/*@null@*/
static void do_slabs_stats(ADD_STAT add_stats, void *c) {
    int i, total;
    /* Get the per-thread stats which contain some interesting aggregates */
    struct thread_stats thread_stats;
    threadlocal_stats_aggregate(&thread_stats);

    total = 0;
    for(i = POWER_SMALLEST; i <= power_largest; i++) {
        slabclass_t *p = &slabclass[i];
        if (p->slabs != 0) {
            uint32_t perslab, slabs;
            slabs = p->slabs;
            perslab = p->perslab;

            char key_str[STAT_KEY_LEN];
            char val_str[STAT_VAL_LEN];
            int klen = 0, vlen = 0;

            APPEND_NUM_STAT(i, "chunk_size", "%u", p->size);
            APPEND_NUM_STAT(i, "chunks_per_page", "%u", perslab);
            APPEND_NUM_STAT(i, "total_pages", "%u", slabs);
            APPEND_NUM_STAT(i, "total_chunks", "%u", slabs * perslab);
            APPEND_NUM_STAT(i, "used_chunks", "%u",
                            slabs*perslab - p->sl_curr - p->end_page_free);
            APPEND_NUM_STAT(i, "free_chunks", "%u", p->sl_curr);
            APPEND_NUM_STAT(i, "free_chunks_end", "%u", p->end_page_free);
            APPEND_NUM_STAT(i, "mem_requested", "%llu",
                            (unsigned long long)p->requested);
            APPEND_NUM_STAT(i, "get_hits", "%llu",
                    (unsigned long long)thread_stats.slab_stats[i].get_hits);
            APPEND_NUM_STAT(i, "cmd_set", "%llu",
                    (unsigned long long)thread_stats.slab_stats[i].set_cmds);
            APPEND_NUM_STAT(i, "delete_hits", "%llu",
                    (unsigned long long)thread_stats.slab_stats[i].delete_hits);
            APPEND_NUM_STAT(i, "incr_hits", "%llu",
                    (unsigned long long)thread_stats.slab_stats[i].incr_hits);
            APPEND_NUM_STAT(i, "decr_hits", "%llu",
                    (unsigned long long)thread_stats.slab_stats[i].decr_hits);
            APPEND_NUM_STAT(i, "cas_hits", "%llu",
                    (unsigned long long)thread_stats.slab_stats[i].cas_hits);
            APPEND_NUM_STAT(i, "cas_badval", "%llu",
                    (unsigned long long)thread_stats.slab_stats[i].cas_badval);
            APPEND_NUM_STAT(i, "touch_hits", "%llu",
                    (unsigned long long)thread_stats.slab_stats[i].touch_hits);
            total++;
        }
    }

    /* add overall slab stats and append terminator */

    APPEND_STAT("active_slabs", "%d", total);
    APPEND_STAT("total_malloced", "%llu", (unsigned long long)mem_malloced);
    add_stats(NULL, 0, NULL, 0, c);
}

static void *memory_allocate(size_t size) {
    void *ret;

    if (mem_base == NULL) {
        /* We are not using a preallocated large memory chunk */
        //ret = malloc(size);
        ret = alloc(size);
    } else {
        ret = mem_current;

        if (size > mem_avail) {
            return NULL;
        }

        /* mem_current pointer _must_ be aligned!!! */
        if (size % CHUNK_ALIGN_BYTES) {
            size += CHUNK_ALIGN_BYTES - (size % CHUNK_ALIGN_BYTES);
        }

        mem_current = ((char*)mem_current) + size;
        if (size < mem_avail) {
            mem_avail -= size;
        } else {
            mem_avail = 0;
        }
    }

    return ret;
}

void *slabs_alloc(size_t size, unsigned int id) {
    void *ret;

    pthread_mutex_lock(&slabs_lock);
    ret = do_slabs_alloc(size, id);
    pthread_mutex_unlock(&slabs_lock);
    return ret;
}

void slabs_free(void *ptr, size_t size, unsigned int id) {
    pthread_mutex_lock(&slabs_lock);
    do_slabs_free(ptr, size, id);
    pthread_mutex_unlock(&slabs_lock);
}

void slabs_stats(ADD_STAT add_stats, void *c) {
    pthread_mutex_lock(&slabs_lock);
    do_slabs_stats(add_stats, c);
    pthread_mutex_unlock(&slabs_lock);
}

void slabs_adjust_mem_requested(unsigned int id, size_t old, size_t ntotal)
{
    pthread_mutex_lock(&slabs_lock);
    slabclass_t *p;
    if (id < POWER_SMALLEST || id > power_largest) {
        fprintf(stderr, "Internal error! Invalid slab class\n");
        abort();
    }

    p = &slabclass[id];
    p->requested = p->requested - old + ntotal;
    pthread_mutex_unlock(&slabs_lock);
}

static pthread_cond_t maintenance_cond = PTHREAD_COND_INITIALIZER;
static volatile int do_run_slab_thread = 1;

#define DEFAULT_SLAB_BULK_CHECK 1
int slab_bulk_check = DEFAULT_SLAB_BULK_CHECK;


enum move_status {
    MOVE_PASS=0, MOVE_DONE, MOVE_BUSY
};

/* Slab rebalancer thread.
 * Does not use spinlocks since it is not timing sensitive. Burn less CPU and
 * go to sleep if locks are contended
 */
static void *slab_maintenance_thread(void *arg) {
    return NULL;
}

enum reassign_result_type slabs_reassign(int src, int dst) {
    enum reassign_result_type ret = REASSIGN_OK;
    return ret;
}

static pthread_t maintenance_tid;

int start_slab_maintenance_thread(void) {
    int ret;
    slab_rebalance_signal = 0;
    slab_rebal.slab_start = NULL;
    char *env = getenv("MEMCACHED_SLAB_BULK_CHECK");
    if (env != NULL) {
        slab_bulk_check = atoi(env);
        if (slab_bulk_check == 0) {
            slab_bulk_check = DEFAULT_SLAB_BULK_CHECK;
        }
    }
    if ((ret = pthread_create(&maintenance_tid, NULL,
                              slab_maintenance_thread, NULL)) != 0) {
        fprintf(stderr, "Can't create thread: %s\n", strerror(ret));
        return -1;
    }
    return 0;
}

void stop_slab_maintenance_thread(void) {
    mutex_lock(&cache_lock);
    do_run_slab_thread = 0;
    pthread_cond_signal(&maintenance_cond);
    pthread_mutex_unlock(&cache_lock);

    /* Wait for the maintenance thread to stop */
    pthread_join(maintenance_tid, NULL);
}

#ifdef MEMC3_CACHE_CLOCK
static __attribute__((unused)) short firstzero[256] =                   \
                              {0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 4,\
                               0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 5,\
                               0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 4,\
                               0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 6,\
                               0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 4,\
                               0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 5,\
                               0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 4,\
                               0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 7,\
                               0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 4,\
                               0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 5,\
                               0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 4,\
                               0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 6,\
                               0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 4,\
                               0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 5,\
                               0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 4,\
                               0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 8};

item* slabs_cache_evict(unsigned int id) {
    slabclass_t *p;
    p = &slabclass[id];

    size_t max_steps = 10;
    bool foundzero = false;

    //size_t clock = rand() % p->clock_max;

    // slow search until we reach the end of a byte
    
    assert(p->clock < p->clock_max);

    while ((p->clock & 0x7) != 0) {
        if (bv_getbit(p->bitmap, p->clock)) {
            bv_setbit(p->bitmap, p->clock, 0);
        } else {
            bv_setbit(p->bitmap, p->clock, 1);
            foundzero = true;
            break;
        }
        p->clock ++;
        if (p->clock >= p->clock_max)
            break;
    }

    if (!foundzero) {
        if (p->clock + 64 >= p->clock_max)
            p->clock = 0;
        uint64_t *val64 = (uint64_t* ) ((char*) (p->bitmap) + p->clock / 8);

        for (size_t steps = 0; steps < max_steps; steps ++) {
            if (*val64 == (uint64_t) -1) {
                *val64 = 0;
                val64 ++;
                p->clock += 64;
                if (p->clock + 64 >= p->clock_max)
                    p->clock = 0;
                continue;
            }
            else {
                // only works for little endian
                uint32_t *val32 = (uint32_t*) val64;
                if (*val32 == (uint32_t) -1) {
                    *val32 = 0;
                    val32 ++;
                    p->clock += 32;
                }
                uint16_t *val16 = (uint16_t*) val32;
                if (*val16 == (uint16_t) -1) {
                    *val16 = 0;
                    val16 ++;
                    p->clock += 16;
                }
                uint8_t *val8 = (uint8_t*) val16;
                if (*val8 == (uint8_t) -1) {
                    *val8 = 0;
                    val8 ++;
                    p->clock += 8;
                }
                //k = *val8;
                //*val8 &= 1 << k;
                p->clock += firstzero[*val8];
                //printf("val8 %d\n", *val8);
                bv_setbit(p->bitmap, p->clock, 1);
                break;
            }
        }

    }

    p->clock = (p->clock > p->clock_max) ? 0 : p->clock;
    bv_setbit(p->bitmap, p->clock, 1);
    
    assert(p->clock < p->clock_max);
    //printf("pos%u\n", p->clock);
    //fflush(stdout);
    size_t index  = p->clock / p->perslab;
    size_t offset = p->clock % p->perslab;
    p->clock ++;
    return (item*) ((char*) (p->slab_list[index]) + p->size * offset);
    
}

void slabs_cache_update(item* it) {

    slabclass_t *p;
    unsigned int id;

    id = it->slabs_clsid;
    if (id > POWER_SMALLEST && id < power_largest) {

        p = &slabclass[id];
        size_t pos = ((char*) it - (char*) mem_base) / p->size;
        assert(pos < p->clock_max);
        bv_setbit(p->bitmap, pos, 1);
    }
}
#endif
