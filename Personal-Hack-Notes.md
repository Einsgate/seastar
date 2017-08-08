My personal hack notes
----------------------

* Which version to build on Ubuntu 16.04 - The one and the only one release version. 
The master version just doesn't work.

* How to add clang support - Configure the project with --compiler=clang-3.9. 

* How to link against google benchmark - Build google benchmark and install
it first. The install directory on Ubuntu 16.04 /usr/local/lib is included in the
defalut search directory of ld. For the default Seastar library, just configure Seastar with 
--ldflags=-lbenchmark. For the test cases, we need to modify the configure.py file.
Find out the extralibs variable and add '-lbenchmark'.

* To correctly compile dpdk using clang, we must remove some compiler flags added by configure.py. 
Remove " -Wno-error=literal-suffix -Wno-literal-suffix" in configure.py

* The full command for configuration when using clang:
./congifure --enable-dpdk --compiler=clang-3.9 --ldflags=-lbenchmark --mode=release --cflags=-Wno-deprecated-register
The added cflags prevents complier from complaining about the "register" keyword when compiling dpdk.

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

C++ Programming Note
--------------------

```cpp
template <typename T1, typename T2, typename T3_or_F, typename... More>
inline
auto
do_with(T1&& rv1, T2&& rv2, T3_or_F&& rv3, More&&... more) {
    auto all = std::forward_as_tuple(
            std::forward<T1>(rv1),
            std::forward<T2>(rv2),
            std::forward<T3_or_F>(rv3),
            std::forward<More>(more)...);
    constexpr size_t nr = std::tuple_size<decltype(all)>::value - 1;
    using idx = std::make_index_sequence<nr>;
    auto&& just_values = cherry_pick_tuple(idx(), std::move(all));
    auto&& just_func = std::move(std::get<nr>(std::move(all)));
    auto obj = std::make_unique<std::remove_reference_t<decltype(just_values)>>(std::move(just_values));
    auto fut = apply(just_func, *obj);
    return fut.then_wrapped([obj = std::move(obj)] (auto&& fut) {
        return std::move(fut);
    });
}
```
The above code manipulates at three arbitrary arguments. It uses std::forward_as_tuple to construct a tuple of references 
for the input arguments. Then it constructs a tuple, containing the references to the first n-1 arguments, and assigns it to rvalue refenence just_values. It constructs a rvalue reference just_func for the last arguments. 

Finally, it constructs obj, moving the first n-1 arguments inside the obj unique_ptr and applys just_func to the underlying object pointed to by obj.

# Seastar Code Review

## Seastar initialization

* Discuss how to initialize Seastar system code, especially initialize the network stack.

* Initially, define a `app_template` object. The `app_template` object contains `boost::program_options::options_description _opts`. Several program options are then added to `_ops` when `app_template` is constructed, including `reactor` options, `seastar::metrics` options, `smp` options (including how many cpus and memories are used) and `scollectd` options.

* Especially, when adding `reactor` options (when calling `reactor::get_options_description()`), `network_stack_registry::list()` is called to get the name of available networking stacks.

* 
