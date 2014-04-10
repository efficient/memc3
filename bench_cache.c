#include "memcached.h"
#include <assert.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <pthread.h>
#include <math.h>

#include "bench_common.h"
#include "bench_config.h"
#include "bench_util.h"

// -----------------------------------------------------------------------------
// Bin: copied from memcached.c


/** exported globals **/
struct stats stats;
struct settings settings;
time_t process_started;     /* when the process was started */

struct slab_rebalance slab_rebal;
volatile int slab_rebalance_signal;

volatile rel_time_t current_time;


void append_stat(const char *name, ADD_STAT add_stats, conn *c,
                 const char *fmt, ...) {
    char val_str[STAT_VAL_LEN];
    int vlen;
    va_list ap;

    assert(name);
    assert(add_stats);
    assert(c);
    assert(fmt);

    va_start(ap, fmt);
    vlen = vsnprintf(val_str, sizeof(val_str) - 1, fmt, ap);
    va_end(ap);

    add_stats(name, strlen(name), val_str, vlen, c);
}


static void stats_init(void) {
    stats.curr_items = stats.total_items = stats.curr_conns = stats.total_conns = stats.conn_structs = 0;
    stats.get_cmds = stats.set_cmds = stats.get_hits = stats.get_misses = stats.evictions = stats.reclaimed = 0;
    stats.touch_cmds = stats.touch_misses = stats.touch_hits = stats.rejected_conns = 0;
    stats.curr_bytes = stats.listen_disabled_num = 0;
    stats.hash_power_level = stats.hash_bytes = stats.hash_is_expanding = 0;
    stats.expired_unfetched = stats.evicted_unfetched = 0;
    stats.slabs_moved = 0;
    stats.accepting_conns = true; /* assuming we start in this state. */
    stats.slab_reassign_running = false;

    /* make the time we started always be 2 seconds before we really
       did, so time(0) - time.started is never zero.  if so, things
       like 'settings.oldest_live' which act as booleans as well as
       values are now false in boolean context... */
    process_started = time(0) - 2;
    stats_prefix_init();
}

static void settings_init(void) {
    //settings.use_cas = true;
    settings.use_cas = false;
    settings.access = 0700;
    settings.port = 11211;
    settings.udpport = 11211;
    /* By default this string should be NULL for getaddrinfo() */
    settings.inter = NULL;
    settings.maxbytes = 64 * 1024 * 1024; /* default is 64MB */
    settings.maxconns = 1024;         /* to limit connections-related memory to about 5MB */
    settings.verbose = 0;
    settings.oldest_live = 0;
    settings.evict_to_free = 1;       /* push old items out of cache when memory runs out */
    settings.socketpath = NULL;       /* by default, not using a unix socket */
    settings.factor = 1.25;
    //settings.chunk_size = 48;         /* space for a modest key and value */
    settings.chunk_size = 64;         /* space for a modest key and value */
    settings.num_threads = 4;         /* N workers */
    settings.num_threads_per_udp = 0;
    settings.prefix_delimiter = ':';
    settings.detail_enabled = 0;
    settings.reqs_per_event = 20;
    settings.backlog = 1024;
    settings.binding_protocol = negotiating_prot;
    settings.item_size_max = 1024 * 1024; /* The famous 1MB upper limit. */
    settings.maxconns_fast = false;
    settings.hashpower_init = 0;
    settings.slab_reassign = false;
    settings.slab_automove = false;
}

conn *conn_new(const int sfd, enum conn_states init_state,
                const int event_flags,
                const int read_buffer_size, enum network_transport transport,
                struct event_base *base) 
{
    return NULL;
}

void do_accept_new_conns(const bool do_accept) 
{
}

cache_t* cache_create(const char* name, size_t bufsize, size_t align,
                      cache_constructor_t* constructor,
                      cache_destructor_t* destructor)
{
    return NULL;
}
// end of copied stuff from memcached
// -----------------------------------------------------------------------------


size_t key_len;
size_t val_len;
size_t total_ops;
pthread_mutex_t mymutex;

typedef struct {
    size_t tid;
    query *queries;
    size_t num_ops;
    size_t num_puts;
    size_t num_gets;
    size_t num_miss;
    size_t num_hits;
    double tput;
} thread_param;


static void print_settings() {
    printf("memcached settings:\n");
    printf("\tmaxbytes = %" PRIu64"\n", settings.maxbytes);
    printf("\tnum_threads = %d\n", settings.num_threads);
    printf("\n");
}

static void print_stats() {
    printf("stats:\n");
    printf("\thash_power_level = %d\n", stats.hash_power_level);
    printf("\thash_bytes = %" PRIu64"\n", stats.hash_bytes);
    printf("\tcurr_items = %d\n", stats.curr_items);
    printf("\tevictions = %" PRIu64"\n", stats.evictions);
    printf("\n");
}

static query *init_queries(char* filename, size_t *num_ops)
{
    FILE *input;
    
    input = fopen(filename, "rb");
    if (input == NULL) {
        perror("can not open file");
        perror(filename);
        exit(1);
    }
    

    if (fread(&key_len, sizeof(key_len), 1, input) == 0) {
        printf("cannot read from %s\n", filename);
        exit(-1);
    }

    if (fread(&val_len, sizeof(val_len), 1, input) == 0) {
        printf("cannot read from %s\n", filename);
        exit(-1);
    }

    if (fread(&total_ops, sizeof(total_ops), 1, input) == 0) {
        printf("cannot read from %s\n", filename);
        exit(-1);
    }

    printf("trace(%s):\n", filename);
    printf("\tkey_len = %zu\n", key_len);
    printf("\tval_len = %zu\n", val_len);
    printf("\ttotal_ops = %zu\n", total_ops);
    printf("\n");

    query *queries = malloc(sizeof(query) * total_ops);
    if (queries == NULL) {
        perror("not enough memory to init queries\n");
        exit(-1);
    }
    
    
    size_t num_read;

    num_read = fread(queries, sizeof(query), total_ops, input);

    if (num_read < total_ops) {
        printf("num_read: %zu\n", num_read);
        perror("can not read all queries\n");
        fclose(input);
        exit(-1);
    }

    fclose(input);
    *num_ops = total_ops;
    return queries;
}

static void put_nolock(char* key, size_t nkey) {
    int   flags = 0;
    rel_time_t exptime = 0;
    int nbytes = val_len + 2;

    enum store_item_type ret;

    //mutex_lock(&cache_lock);
    // LRU may be triggered here
    item* it = item_alloc(key, nkey, flags, exptime, nbytes);
    //mutex_unlock(&cache_lock);

    if (it == NULL) {
        return;
    }

    // make a fake value
    //before_write(it);
    memcpy(ITEM_data(it), key, val_len );
    //assert(memcmp(ITEM_data(it), ITEM_key(it), nkey) == 0);
    //after_write(it);

    // store the item 
    // store_item holds its bucket lock
    ret = store_item(it, 0, NULL);
    if (ret == NOT_STORED) {
        perror("can not store");
        exit(-1);
    }
    // update item in LRU policy
    //item_update(it);

    // release the reference
    item_remove(it); 

}

static void put(char* key, size_t nkey) {

    mutex_lock(&cache_lock); 

    put_nolock(key, nkey);

    mutex_unlock(&cache_lock);

}

// __attribute__ ((unused))
static item* get(char* key, size_t nkey)
{
    
    //char data[nkey+val_len+8];
    //char key_read[nkey];
    //char val_read[val_len];
    item* it = NULL;
    
    //unsigned short vs, ve;
//GetRetry:

    // item_get holds its bucket lock
    it = item_get(key, nkey);

    /* vs = ITEM_version(it); */
    /* if (vs & 1) { */
    /*     goto GetRetry; */
    /* } */


    if (it) {
        /* uint8_t         it_flags; */

        /* //memcpy(data, it->data, nkey+val_len+8); */
        /* memcpy(key_read, ITEM_key(it), nkey); */
        /* memcpy(val_read, ITEM_data(it), val_len); */
        /* it_flags = it->it_flags; */

        /* ve = ITEM_version(it); */
        /* if (vs != ve) { */
        /*     goto GetRetry; */
        /* } */

        /* if ((it_flags & ITEM_LINKED) == 0) */
        /*     return NULL; */

        /* // at least it's not corrupted data */
        /* //assert(memcmp(val_read, key_read, nkey) == 0); */

        /* if (!keycmp(key_read, key, nkey)) { */
        /*     goto GetRetry; */
        /* } */
        //assert(memcmp(val_read, key, nkey) == 0);

        // update item in LRU policy
        item_update(it);

        //release the reference
        item_remove(it);
    }
    return it;
}

static void* run(void* param)
{
    struct timeval tv_s, tv_e;

    thread_param* p = (thread_param*) param;
    query* queries = p->queries;


    gettimeofday(&tv_s, NULL);

    for (size_t i = 0 ; i < p->num_ops; i ++) {
        enum query_types type = queries[i].type;
        char * key = queries[i].hashed_key;
        if (type == query_put) {
            p->num_puts ++;     
            put(key, key_len);

        } else if (type == query_get) {
            p->num_gets ++;
            item* it = get(key, key_len);
            if (it == NULL) {
                // cache miss
                p->num_miss ++;
                put(key, key_len);
            }
            else {
                p->num_hits ++;
            }
        }

    }

    gettimeofday(&tv_e, NULL);
    double time = timeval_diff(&tv_s, &tv_e);

    p->tput = p->num_ops / time;

    pthread_mutex_lock (&mymutex);
    printf("[thread%"PRIu64" (on CPU %d) get %"PRIu64" items in %.2f sec]\n", 
           p->tid, sched_getcpu(), p->num_ops, time);
    printf("#put = %zu, #get = %zu\n", p->num_puts, p->num_gets);
    printf("#miss = %zu, #hits = %zu\n", p->num_miss, p->num_hits);
    printf("hitratio = %.4f\n",   (float) p->num_hits / p->num_gets);
    printf("tput = %.2f\n",  p->tput);
    printf("\n");
    pthread_mutex_unlock (&mymutex);

    pthread_exit(NULL);
}


static void usage() 
{
    printf("bench_cache\n");
    printf("\t-m #: max memory to use for items in megabytes\n");
    printf("\t-t #: number of threads\n");
    printf("\t-l load trace: /path/to/ycsbtrace\n");
    printf("\t-r run trace: /path/to/ycsbtrace\n");
    printf("\t-h  : show usage\n");

}

int main(int argc, char** argv) 
{

    char *loadfile = NULL;
    char *runfile = NULL;

    /* init settings */
    settings_init();
    settings.hashpower_init = 27;
    settings.num_threads = 1;

    char ch;
    while ((ch = getopt(argc, argv, "m:l:r:t:hv")) != -1) {
        switch (ch) {
        case 'l':
            loadfile = optarg;
            break;
        case 'r':
            runfile = optarg;
            break;
        case 'm':
            settings.maxbytes = ((size_t)atoi(optarg)) * 1024 * 1024;
            break;
        case 't':
            settings.num_threads = atoi(optarg);
            if (settings.num_threads <= 0) {
                fprintf(stderr, "Number of threads must be greater than 0\n");
                return 1;
            }
            /* There're other problems when you get above 64 threads.
             * In the future we should portably detect # of cores for the
             * default.
             */
            if (settings.num_threads > 64) {
                fprintf(stderr, "WARNING: Setting a high number of worker"
                        "threads is not recommended.\n"
                        " Set this value to the number of cores in"
                        " your machine or less.\n");
            }
            break;
        case 'v':
            settings.verbose++;
            break;
        case 'h': 
        default:
            usage();
            exit(-1);
        }
    }
#ifdef TEST_CUCKOO
    settings.hashpower_init = settings.hashpower_init - 2;
#endif

    print_settings();

    if (loadfile == NULL || runfile == NULL) {
        usage();
        exit(-1);
    }
        
    /* initialize other stuff */
    stats_init();
#ifdef TEST_ORIGINAL
    assoc_init(settings.hashpower_init);
#endif
#ifdef TEST_CUCKOO
    assoc2_init(settings.hashpower_init);
#endif
    slabs_init(settings.maxbytes, settings.factor, true);
    /* start up worker threads if MT mode */
    thread_init(settings.num_threads, NULL);
    print_bench_settings();
    
    /*
     * phase1: warm the cache:
     */
    printf("[Loading trace...]\n");
    size_t num_keys;
    query* queries;
    queries = init_queries(loadfile, &num_keys);
    for (size_t i = 0; i <  num_keys; i ++) {
        char * key = queries[i].hashed_key;
        put_nolock(key, key_len);
    }
    free(queries);
    printf("[Loading trace done]\n");

    /*
     * phase2: exec the queries
     */
    printf("[Running trace...]\n");

    thread_param tp[settings.num_threads];
    pthread_t threads[settings.num_threads];
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_mutex_init(&mymutex, NULL);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

    queries = init_queries(runfile, &total_ops);
    for (int t = 0; t < settings.num_threads; t++) {
        tp[t].queries = queries + t * total_ops / settings.num_threads;
        tp[t].num_ops = total_ops / settings.num_threads;
        tp[t].tid     = t;
        tp[t].num_puts = 0;
        tp[t].num_gets = 0;
        tp[t].num_miss = 0;
        tp[t].num_hits = 0;
        tp[t].tput = 0.0;
    }

    size_t n = get_cpunum();
    for (int t = 0; t < settings.num_threads; t++) {

#ifdef __linux__
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        if (2 * t < n)
            CPU_SET(2 * t, &cpuset);
        else
            CPU_SET(2 * t - n + 1, &cpuset);
        pthread_attr_setaffinity_np(&attr, sizeof(cpu_set_t), &cpuset);            
#endif

        int rc;
        rc = pthread_create(&threads[t], &attr, run, (void *) &tp[t]);
        if (rc) {
            perror("error: pthread_create\n");
            exit(-1);
        }
    }


    double total_tput = 0.0;
    size_t total_hits = 0;
    size_t total_gets  = 0;

    for (int t = 0; t < settings.num_threads; t++) {
        void *status;
        int rc = pthread_join(threads[t], &status);
        if (rc) {
            perror("error, pthread_join\n");
            exit(-1);
        }

        total_tput += tp[t].tput;
        total_hits += tp[t].num_hits;
        total_gets += tp[t].num_gets;
    }


    printf("total_tput = %.2f MOPS\n", total_tput / MILLION);
    printf("total_hitratio = %.4f\n", (float) total_hits / total_gets);

    pthread_attr_destroy(&attr);

#ifdef TEST_ORIGINAL
    assoc_post_bench();
#endif

#ifdef TEST_CUCKOO
    assoc2_post_bench();
#endif


    print_stats();


    return 0;

}
