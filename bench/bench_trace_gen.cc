#include <cstdlib>
#include <iostream>
#include <openssl/sha.h>
#include <cstdio>
#include <cstring>
#include <cstdint>

#include "bench_common.h"

using namespace std;

static void sha1(char hash[20], const char* buf, size_t count)
{
	SHA1(reinterpret_cast<const unsigned char*>(buf), count, reinterpret_cast<unsigned char*>(hash));
}


int main(int argc, char **argv) {

    if (argc <= 1) {
        cout << "usage: ./bench_trace_gen  output_filename < input_filename" 
             << endl;
        exit (1);
    }

    //size_t val_len = static_cast<size_t>(-1);
    //size_t val_len = atoi(argv[1]);
    size_t key_len = NKEY;
    size_t val_len = NVAL;
    size_t num_queries = 0;

    FILE *fp = fopen(argv[1], "w");

    fwrite(&key_len, sizeof(size_t), 1, fp);
    fwrite(&val_len, sizeof(size_t), 1, fp);

    const size_t tmp_size = 1048576;
	char* tmp = new char[tmp_size];
	
    while (fgets(tmp, tmp_size, stdin)) {
        char buf[20];
        char rawkey[1024];
        query q;
        if (sscanf(tmp, "\"operationcount\"=\"%zu\"", &num_queries)) {
            fwrite(&num_queries, sizeof(num_queries), 1, fp);
            continue;
        } else if (sscanf(tmp, "INSERT usertable %s [ field", rawkey)) {
            q.type = query_put;
            sha1(buf, rawkey, strlen(rawkey));
            memcpy(q.hashed_key, buf, key_len);
        } else if (sscanf(tmp, "UPDATE usertable %s [ field", rawkey)) { 
            q.type = query_put;
            sha1(buf, rawkey, strlen(rawkey));
            memcpy(q.hashed_key, buf, key_len);
        } 
        else if (sscanf(tmp, "READ usertable %s [", rawkey)) {
            q.type = query_get;
            sha1(buf, rawkey, strlen(rawkey));
            memcpy(q.hashed_key, buf, key_len);
        } else 
            continue;

        fwrite(&q, sizeof(q), 1, fp);
    }
    delete tmp;
    fclose(fp);
}
