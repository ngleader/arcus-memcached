## arcus-memcached : Arcus Memory Cache Server

Arcus is a memcached-based cache cloud developed by NAVER Corp.
Memcached has been heavily modified to support functional and performance
requirements of NAVER services.
Arcus is normally positioned in front of a back-end storage, such as DB,
where it stores/retrieves hot-spot data of service applications using fast main memory.
Therefore, Arcus provides very fast data access for service applications
and reduces the load on back-end storage.
In addition, Arcus can serve as fast shared storage.  Remote services processes
usually store complex query results, computation-intensive results, and intermediate results
of web processing in Arcus cache cloud so any process can access them.

Arcus provides two new significant features over the existing memcached.

First, Arcus supports collection types.  A single key can have
a collection of data as its value.  Arcus currently supports three collection
data structures.

* **List** - a doubly-linked list structure.
* **Set** - an unordered set of unique data.
* **B+tree** - a b+tree structure, conceptually similar to sorted map.

Second, Arcus can operate as a cache cloud using Zookeeper.
When a cache node fails, it is quickly detected and is excluded from the cache cloud.
The remaining cache nodes keep on serving using the new cloud configuration.
Likewise, a cache node may join the cloud on the fly.  Keys are automatically
re-distributed to maintain load balancing.

## Build on Linux

arcus-memcached builds successfully on 64-bit Linux (CentOS, Ubuntu) environments.
Other Linux distributions have not been tested.

**Dependencies**

arcus-memcached has the following dependencies. Make sure to install them.
- [libevent](http://libevent.org/) - An event notification library
- [arcus-zookeeper](https://github.com/naver/arcus-zookeeper) - Zookeeper c library with Arcus modification

**Compile**

To build arcus-memcached from the git-cloned source code,

```
$ ./config/autorun.sh
$ ./configure [--with-libevent=<libevent_install_path>]
$ make
$ make install
```

The use of ZooKeeper based clustering is optional.
To enable it, use --enable-zk-integration along with --with-zookeeper when running configure.
Make sure to use the ZooKeeper library with Arcus modifications.

## Run

arcus-memcached has a pluggable engine structure.
Only the default engine provides all of the above Arcus functionalities.

To start Arcus cache server with the default engine, run the following command line.

```
$ <arcus_install_path>/bin/memcached -m 4000 -p 11211 -E <arcus_install_path>/lib/default_egine.so
```

Two important start options are here.
- To enable Zookeeper-based clustering, use -z to specify the Zookeeper ensemble ip:port list.
- The scrub command is provided as an ASCII command extension.
  To use the command, use -X to specify the location of ascii_scrub.so library.

To see details on arcus-memcached start options, run memcache with -h option like below.
```
$ <arcus_install_path>/bin/memcached -h
```

To stop the running Arcus cache server, use kill command like below.
```
$ kill -INT <pid>
```

## ASCII Protocol

Please refer to
[Arcus cache server ascii protocol](/doc/arcus-ascii-protocol.md)
for details on Arcus ASCII commands.

## Issues

If you find a bug, please report it via the GitHub issues page.

https://github.com/naver/arcus-memcached/issues

## Arcus Contributors

In addition to those who had contributed to the original memcached,
the following people at NAVER have contributed to arcus-memcached.

JunHyun Park <junhyun.park@navercorp.com>; <jhpark816@gmail.com>  
HyongYoub Kim <hyongyoub.kim@navercorp.com>  
YeaSol Kim (ngleader) <sol.k@navercorp.com>; <ngleader@gmail.com>  
HoonMin Kim (harebox) <hoonmin.kim@navercorp.com>; <harebox@gmail.com>  
SeongHwan Jeong (scryner) <scryner@nhnent.com>  
ChangHyeon Song <chang.song@navercorp.com>  

## License

Licensed under the Apache License, Version 2.0: http://www.apache.org/licenses/LICENSE-2.0

## Patents

Arcus has patents on b+tree smget operation.
Refer to PATENTS file in this directory to get the patent information.

Under the Apache License 2.0, a perpetual, worldwide, non-exclusive,
no-charge, royalty-free, irrevocable patent license is granted to any user for any usage.
You can see the specifics on the grant of patent license in LICENSE file in this directory.