#ifndef _BENCH_COMMON_H_
#define _BENCH_COMMON_H_

#include "bench_config.h"

#define MILLION 1000000

enum query_types{
    query_put=0,
    query_get,
    query_del,
    query_NA,
};

typedef struct __attribute__((__packed__)) {
    char hashed_key[NKEY];
    char type;
} query;

typedef struct __attribute__((__packed__)) {
    char hashed_key[NKEY];
    uint32_t hv;
    char type;
    void *ptr;
} hash_query;

#define CONSOLE_PORT 40096

#define CMD_GO "go\0"

typedef struct __attribute__((__packed__)) {
    double total_tput;
    double total_time;
    size_t total_hits;
    size_t total_miss;
    size_t total_gets;
    size_t total_puts;
    size_t num_threads;
} result_t;

#endif
