My personal hack notes
----------------------

* Which version to build on Ubuntu 16.04 - The one and the only one release version. 
The master version just doesn't work.

* How to add clang support - Configure the project with --compiler=clang-3.9. 
But this makes DPDK unusable due to a build error.

* How to link against google benchmark - Build google benchmark and install
it first. The install directory on Ubuntu 16.04 /usr/local/lib is included in the
defalut search directory of ld. For the default Seastar library, just configure Seastar with 
--ldflags=-lbenchmark. For the test cases, we need to modify the configure.py file.
Find out the extralibs variable and add '-lbenchmark'.

Promise and Future in Seastar
-----------------------------

* I treat promises and futures as re-implementation of OCaml lwt in C++.
* In lwt, there are only promises, with a basic monadic bind opeartor. The monadic bind operator 
adds a callback to the sleeping promise. When the sleeping promise is waken up, the callback
is called to connect the bind operation with the newly constructed and returned promise.
* In Seastar, things are similar. Since C++ has no GC, Seastar adds an object called future, which is
a pointer object to the underlying promise.
* Memory management: Callbacks and captured variables are stored in _task field of the promise. When the promise is waken 
up by setting a value, the _task is moved to the reactor for execution, and the original promise is freed. 
* Future and promise pair: If the promise is deconstructed, it must not contain uncompleted callbacks. It's state will be 
moved to the underlying future. Promise and future hold a non-owning pointer to each other. When either one is deconstructed,
the pointer on the other should be invalidated.
