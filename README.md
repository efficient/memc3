MemC3
=====

MemC3 is an in-memory key-value cache, built upon Memcached. By adapting memory-efficient, multi-reader concurrent cuckoo hashing and CLOCK replacement algorithm, it scale better, run faster, and use less memory. For details about this algorithm and citations, please refer to [our paper in NSDI 2013][1].

   [1]: http://www.cs.cmu.edu/~dga/papers/memc3-nsdi2013.pdf "MemC3: Compact and Concurrent Memcache with Dumber Caching and Smarter Hashing"

Authors
======= 

Bin Fan (binfan@cs.cmu.edu), David G. Andersen (dga@cs.cmu.edu), and Michael Kaminsky (michael.e.kaminsky@intel.com)

Building
==========

    $ autoreconf -fis
    $ ./configure
    $ make
