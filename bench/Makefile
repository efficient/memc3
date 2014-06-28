all: bench_client

bench_client: bench_client.c
	gcc -std=gnu99 -O2 -lmemcached -o bench_client bench_client.c
