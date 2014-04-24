MemC3
=====

MemC3 is an in-memory key-value cache, derived from Memcached but improved with memory-efficient and concurrent data structures. MemC3 applies multi-reader concurrent cuckoo hashing  as its key-value index and CLOCK-replacement algorithm as cache eviction policy. As a result, MemC3 scales better, runs faster, and uses less memory. For details about the algorithms, performance evaluation and citations, please refer to [our paper in NSDI 2013][1].

   [1]: http://www.cs.cmu.edu/~dga/papers/memc3-nsdi2013.pdf "MemC3: Compact and Concurrent Memcache with Dumber Caching and Smarter Hashing"

Authors
======= 

MemC3 is developed by Bin Fan, David G. Andersen, and Michael Kaminsky. You can also email us at [libcuckoo-dev@googlegroups.com](mailto:libcuckoo-dev@googlegroups.com).


Building
==========

    $ autoreconf -fis
    $ ./configure
    $ make
