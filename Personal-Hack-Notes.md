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
