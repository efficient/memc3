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


#ifdef TEST_ORIGINAL
#define ASSOC_INIT       assoc_init
#define ASSOC_DESTROY    assoc_destroy
#define ASSOC_INSERT     assoc_insert
#define ASSOC_FIND       assoc_find
#define ASSOC_PRE_BENCH  assoc_pre_bench
#define ASSOC_POST_BENCH assoc_post_bench
#endif

#ifdef TEST_CUCKOO
#define ASSOC_INIT       assoc2_init
#define ASSOC_DESTROY    assoc2_destroy
#define ASSOC_INSERT     assoc2_insert
#define ASSOC_FIND       assoc2_find
#define ASSOC_PRE_BENCH  assoc2_pre_bench
#define ASSOC_POST_BENCH assoc2_post_bench
#endif



/* Lock for cache operations (item_*, assoc_*) */
pthread_mutex_t cache_lock;

pthread_mutex_t *item_locks;
/* size of the item lock hash table */
static uint32_t item_lock_count;
/* size - 1 for lookup masking */
static uint32_t item_lock_mask;

void item_lock(uint32_t hv) {
    mutex_lock(&item_locks[hv & item_lock_mask]);
}

void item_unlock(uint32_t hv) {
    pthread_mutex_unlock(&item_locks[hv & item_lock_mask]);
}

void STATS_LOCK() {}

void STATS_UNLOCK() {}

/** exported globals **/
struct stats stats;
struct settings settings;


pthread_mutex_t mymutex;

typedef struct {
    char it[sizeof(item)];
    char key[NKEY];
} item_mem;

typedef struct {
    char hashed_key[NKEY];
    uint32_t hv;
} item_disk;

static uint32_t myhashpower = 0;
static int num_threads = 1;
static size_t num_queries = 100*MILLION; //100*MILLION;
static double duration = 10.0;
static double writeratio = 0.00;
static double posratio = 1.00;
static hash_query *queries = NULL;
static size_t nkeys_lookup = 100*MILLION;//100*MILLION;
static size_t nkeys_insert = 130*MILLION;//130*MILLION;
static size_t coverage;
static bool verbose = false;
static int number = 1;


static int ht_insert(item *it, const uint32_t hv) {

#if defined(ENABLE_GLOBAL_LOCK)
    // insert is protected by a global lock
    mutex_lock(&cache_lock);
#endif

#if defined(TEST_ORIGINAL) && defined(ENABLE_FG_LOCK)
    item_lock(hv);
#endif
    
    int ret = ASSOC_INSERT(it, hv);

#if defined(TEST_ORIGINAL) && defined(ENABLE_FG_LOCK)
    item_unlock(hv);
#endif

#if defined(ENABLE_GLOBAL_LOCK)
    mutex_unlock(&cache_lock);
#endif

    return ret;
}

static item *ht_lookup(const char *key, const uint32_t hv) {

#if defined(ENABLE_GLOBAL_LOCK)
    mutex_lock(&cache_lock);
#endif

#if defined(TEST_ORIGINAL) && defined(ENABLE_FG_LOCK)
    item_lock(hv);
#endif

    item *it = ASSOC_FIND(key, NKEY, hv);

#if defined(TEST_ORIGINAL) && defined(ENABLE_FG_LOCK)
    item_unlock(hv);
#endif

#if defined(ENABLE_GLOBAL_LOCK)
    mutex_unlock(&cache_lock);
#endif
    return it;
}


static void print_stats() {
    printf("stats:\n");
    printf("\thash_power_level=%d\n", stats.hash_power_level);
    printf("\thash_bytes=%" PRIu64"\n", stats.hash_bytes);
    printf("\tcurr_items=%d\n", stats.curr_items);
    printf("\n");
}

static void init(unsigned int hashpower) {
    srand( time(NULL) );
    pthread_mutex_init(&cache_lock, NULL);

    memset(&stats, 0, sizeof(struct stats));
    memset(&settings, 0, sizeof(struct settings));
    
    settings.verbose = 2;

    ASSOC_INIT(hashpower);

    item_lock_count = ((unsigned long int)1 << (13));
    item_lock_mask  = item_lock_count - 1;

    item_locks = calloc(item_lock_count, sizeof(pthread_mutex_t));
    if (! item_locks) {
        perror("Can't allocate item locks");
        exit(1);
    }
    for (int i = 0; i < item_lock_count; i++) {
        pthread_mutex_init(&item_locks[i], NULL);
    }
}


static hash_query *init_queries(size_t num_need, char* filename)
{
    static int batch = 0;
    batch ++;

    
    FILE *fd;
    size_t count;


    item_disk* buffer_disk = calloc(num_need, sizeof(item_disk));
    if (!buffer_disk) {
        perror("not enough memory to init queries\n");
        exit(-1);
    }

    fd = fopen(filename, "rb");
    if (fd) {
        size_t   num_have = 0;

        printf("read trace file %s ...\n", filename);
        count = fread(&num_have, sizeof(size_t), 1, fd);
        if (count < 1) {
            perror("fread err\n");
            exit(-1);
        }
        
        if (num_have < num_need) {
            perror("sorry, not enough trace\n");
            exit(-1);
        }
        else {
            printf("%zu records stored in this file ...\n", num_have);
            printf("%zu records needed ...\n", num_need);
        }

        count = fread(buffer_disk, sizeof(item_disk), num_need, fd);
        if (count < num_need) {
            perror("fread err\n");
            exit(-1);
        }
        fclose(fd);
    }

    else {
        printf("can not open file %s, let us make it\n", filename);

        for (size_t i = 0; i < num_need; i++) {
            sprintf(buffer_disk[i].hashed_key, "key-%d-%"PRIu64"0", batch, (uint64_t) i);
            buffer_disk[i].hv = hash(buffer_disk[i].hashed_key, NKEY, 0);
        }
        
        
        fd = fopen(filename, "wb");
        if (fd) {
            count = fwrite(&num_need, sizeof(size_t), 1, fd);
            if (count < 1) {
                perror("fread err\n");
                exit(-1);
            }
            count = fwrite(buffer_disk, sizeof(item_disk), num_need, fd);
            if (count < num_need) {
                perror("write err\n");
                exit(-1);
            }
            printf("dump %zu items to file %s\n", num_need, filename);
            fclose(fd);
        } else {
            perror("can not dump\n");
            exit(-1);
        }
    }
    
    item_mem* buffer = calloc(num_need, sizeof(item_mem));
    if (!buffer) {
        perror("not enough memory to init queries\n");
        exit(-1);
    }
    hash_query *queries = calloc(num_need, sizeof(hash_query));
    if (!queries) {
        perror("not enough memory to init queries\n");
        exit(-1);
    }
    memset(queries, 0, sizeof(hash_query) * num_need);

    for (size_t i = 0; i < num_need; i++) {
        
        item* it =  (item*) buffer[i].it;
        it->it_flags = settings.use_cas ? ITEM_CAS : 0;
        it->nkey = NKEY;
#ifdef TEST_LRU
        it->next = it->prev = 0;
#endif
#ifdef TEST_ORIGINAL
        it->h_next = 0;
#endif
        char* it_key = ITEM_key(it);
        memcpy(it_key, buffer_disk[i].hashed_key, NKEY);

        memcpy(queries[i].hashed_key, buffer_disk[i].hashed_key, NKEY);
        queries[i].ptr = (void*) it;
        queries[i].hv = buffer_disk[i].hv;
    }
        
    free(buffer_disk);
    return queries;
}

static size_t insert_items(size_t num, hash_query *queries)
{
    size_t total = 0;
    for (size_t i = 0; i < num; i ++) {
        int ret = ht_insert((item*) queries[i].ptr, queries[i].hv);
        if (ret == 0)
            break;
        total += 1;
    }
    printf("insert_items done: %zu items of %zu inserted\n", total, num);

    print_stats();
    return total;

}

typedef struct {
    int    tid;
    double time;
    double tput;
    size_t gets;
    size_t puts;
    size_t hits;
    int cpu;
} thread_param;

static void* exec_queries(void *a) //size_t num, hash_query *queries)
{
    struct timeval tv_s, tv_e;

    thread_param *tp = (thread_param*) a;
    tp->time = 0; 
    tp->hits = 0;
    tp->gets = 0;
    tp->puts = 0;
    tp->cpu =  sched_getcpu();

    if (verbose) {
        pthread_mutex_lock (&mymutex);
        printf("[thread%d (on CPU %d) started]\n", tp->tid, sched_getcpu());
        pthread_mutex_unlock (&mymutex);
    }
    size_t nq = num_queries / num_threads;
    //printf("nq = %"PRIu64" , num_threads = %d\n", nq, num_threads);
    hash_query *q = queries + nq * tp->tid;

    size_t k = 0;
    size_t left = nq;
    bool done = false;
    while (!done && tp->time < duration && left > 0) {
        size_t step;
        if (left >= 1000000)
            step = 1000000;
        else
            step = left;
    
        gettimeofday(&tv_s, NULL);
        for (size_t j = 0; j < step; j++, k ++) {
            if (q[k].type == query_get) {
                tp->gets ++;
                item* it = ht_lookup(q[k].hashed_key,  q[k].hv);
                tp->hits += (it != NULL);
                
            }
            else if (q[k].type == query_put) {
                tp->puts ++;
                int ret = ht_insert((item*) q[k].ptr, q[k].hv);
                if (ret == 0) {
                    done = true;
                    break;
                }
            } else {
                perror("unknown query\n");
                exit(0);
            }
        }
        gettimeofday(&tv_e, NULL);
        tp->time += timeval_diff(&tv_s, &tv_e);
        left = left - step;
     }

    tp->tput = (float) k / tp->time;

    pthread_exit(NULL);
}


static void usage(char* binname) 
{
    printf("%s\n", binname);
    printf("\t-l  : benchmark lookup\n");
    printf("\t-i  : benchmark insert\n");
    printf("\t-p #: myhashpower, benchmark create (to measure size)\n");
    printf("\t-t #: number of threads\n");
    printf("\t-c #: coverage\n");
    printf("\t-w #: percent of writes\n");
    printf("\t-r #: number of runs (for lookup)\n");
    printf("\t-h  : show usage\n");

}

static  __attribute__ ((unused))
int assign_core(int i)
{
    //size_t n = get_cpunum();
    size_t n = 6;
    int c = 0;
    if (i < n)
        c = 2 * i;
    else if (i < 2 * n)
        c = 2 * i - 2 * n + 1;
    else if (i < 3 * n)
        c = 2 * i - 2 * n ;

    /* if (2 * i < 24) { */
    /*     c = 2 * i; */
    /* } else { */
    /*     c = 2 * i - 24 + 1; */
    /* } */
    //c = 8;
    return c;
    //return i;
}

int main(int argc, char** argv) 
{

    int bench_lookup = 0;
    int bench_insert = 0;

    size_t num_runs = 1;
    coverage = nkeys_lookup;

    hash_query *pos_queries = NULL;
    hash_query *neg_queries = NULL;
    
    char ch;
    while ((ch = getopt(argc, argv, "ilp:c:t:d:w:hn:vr:b:")) != -1) {
        switch (ch) {
        case 'i': bench_insert = 1; break;
        case 'l': bench_lookup = 1; break;
        case 'p': myhashpower = atoi(optarg); break;
        case 't': num_threads = atoi(optarg); break;
        case 'd': duration = atof(optarg); break;
        case 'c': coverage = atoi(optarg); break;
        case 'w': writeratio = atof(optarg); break;
        case 'b': posratio   = atof(optarg); break;
        case 'r': num_runs = (int)atoi(optarg); break;
        case 'n': number = (int)atoi(optarg); break;
        case 'v': verbose = true; break;
        case 'h': usage(argv[0]); exit(0); break;
        default:
            usage(argv[0]);
            exit(-1);
        }
    }

    if (bench_insert + bench_lookup < 1)  {
        usage(argv[0]);
        exit(-1);
    }

/*     uint32_t tmp = nkeys_lookup; */
/*     while (tmp > 0) { */
/*         myhashpower ++; */
/*         tmp = tmp >> 1; */
/*     } */
/* #ifdef TEST_CUCKOO */
/*     myhashpower -= 2; */
/* #endif */

    
    size_t ninserted = 0;
    print_bench_settings();
                
    if (bench_insert) {

         printf("[bench_insert]\n");

#ifdef TEST_ORIGINAL
        myhashpower = 26;
        num_queries = 1 << (myhashpower + 1);
#endif

#ifdef TEST_CUCKOO
        myhashpower = 25;
        num_queries = 1 << (myhashpower + 2);
#endif

        char insert_trace[32];
        memset(insert_trace, 0, sizeof(insert_trace));
        sprintf(insert_trace,"trace1-%zu", nkeys_lookup);
        queries = init_queries(num_queries, insert_trace);

        myhashpower = 25;
        num_queries = 1 << (myhashpower + 2);

        init(myhashpower);
        print_stats();

        /* for (size_t k = 0; k < num_queries; k ++) { */
        /*     queries[k].type = query_put; */
        /* } */

        /* duration = 1000; */

        printf("num_queries = %.2f M\n", num_queries * 1.0 / MILLION);
        printf("num_threads = %d\n", num_threads);
        printf("hashpower    = %u\n", myhashpower);

        ASSOC_PRE_BENCH();
        TIME("insert items", insert_items(num_queries, queries), num_queries);
        ASSOC_POST_BENCH();
        exit(0);
        
    } else if (bench_lookup) {
        printf("[bench_lookup]\n");
#ifdef TEST_ORIGINAL
        //myhashpower = 26;
        //num_queries = 1 << (myhashpower + 1);
        // only for explore!!
        myhashpower = 27;
        num_queries = 100 * MILLION;
        nkeys_insert = 100 * MILLION;
#endif

#ifdef TEST_CUCKOO
        myhashpower = 25;
        num_queries = 100 * MILLION; //1 << (myhashpower + 2);
#endif


        init(myhashpower);

        char pos_trace[32];
        char neg_trace[32];
        memset(pos_trace, 0, sizeof(pos_trace));
        memset(neg_trace, 0, sizeof(neg_trace));
        sprintf(pos_trace,"trace1-130M"); //, nkeys_lookup);
        sprintf(neg_trace,"trace2-100M"); //, nkeys_insert);

        pos_queries = init_queries(nkeys_insert, pos_trace);
        neg_queries = init_queries(nkeys_lookup, neg_trace);

        print_stats();

        
        ASSOC_PRE_BENCH();
        TIME("insert items", ninserted = insert_items(nkeys_insert, pos_queries), ninserted);
        ASSOC_POST_BENCH();
        

        printf("ninserted = %"PRIu64"\n", ninserted);
        printf("writeratio = %.2f\n", writeratio);
        printf("posratio = %.2f\n", posratio);
        printf("coverage = %"PRIu64"\n", (uint64_t)coverage);
        printf("num_queries = %.2f M\n", num_queries * 1.0 / MILLION);
        printf("num_threads = %d\n", num_threads);
        printf("hashpower    = %u\n", myhashpower);
    }

    
    queries = malloc(sizeof(hash_query) * num_queries);
    if (!queries) 
	{
		perror("not enough memory for queries\n");
		exit(-1);
	}

	pthread_t threads[num_threads];
    thread_param tp[num_threads];
    pthread_attr_t attr;
    pthread_attr_init(&attr);

    // start  benchmarking
    for (int iter = 0; iter < num_runs; iter ++)
    {
        printf("\n[start iter = %d]\n", iter + 1);
        printf("[exec %.2f M queries]\n", num_queries * 1.0 / MILLION);
        printf("[generating workload...");
        size_t next_insert = 0;
        for (size_t k = 0; k < num_queries; k ++) {
            if (((float) rand() / (float) RAND_MAX) < writeratio) {
                queries[k] = neg_queries[next_insert++];
                queries[k].type = query_put;
            }
            else {
                // this is a get query

                if (((float) rand() / (float) RAND_MAX) < posratio) {
                    queries[k] = pos_queries[rand() % (ninserted)];
                }
                else {
                    queries[k] = neg_queries[rand() % (nkeys_lookup)];
                }

                queries[k].type = query_get;
            }
        }
        printf("done]\n");


        for (int i = 0; i < num_threads; i++) {
            tp[i].tid = i;
#ifdef __linux__

            int c = assign_core(i);
            cpu_set_t cpuset;
            CPU_ZERO(&cpuset);
            CPU_SET(c, &cpuset);
            pthread_attr_setaffinity_np(&attr, sizeof(cpu_set_t), &cpuset);
#endif
            int rc = pthread_create(&threads[i],  &attr, exec_queries, (void*) &(tp[i]));
            if (rc) {
                perror("error, pthread_create\n");
                exit(-1);
            }
        }
        double total_tput = 0.0;
        size_t total_puts = 0;
        for (int i = 0; i < num_threads; i++) {
            pthread_join(threads[i], NULL);
            total_tput += tp[i].tput;
            total_puts += tp[i].puts;

            if (verbose) {
                printf("[thread%d done on CPU %d: %.2f sec]\n", 
                       tp[i].tid, tp[i].cpu, tp[i].time);
                printf("\thitratio = %.2f, tput = %.2f MOPS\n",
                       1.0 * tp[i].hits / tp[i].gets, (float) tp[i].tput / MILLION);
                printf("\tgets = %.2f M puts = %.2f M\n",
                       (float) tp[i].gets / MILLION, (float) tp[i].puts / MILLION);
            }
        }

        printf("tput = %.2f MOPS\n", total_tput / MILLION);
        printf("total_puts = %.2f M\n", (float) total_puts / MILLION);
        ASSOC_POST_BENCH();
    }
    free(queries);
}
