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

* To use virtio device inside a vm. Add -lrte_pmd_virtio -lrte_net library options to configure.py, along with other dpdk specific library options.

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

* Especially, when adding `reactor` options (when calling `reactor::get_options_description()`), `network_stack_registry::list()` is called to get the name of available networking stacks. The options of the `network_stack_registry` is also added to the options of the `reactor`.

    * The `network_stack_registry` contains singltons for registering Seastar software network stacks. Among the set of the public methods provided by `network_stack_registry`, `register_stack` is the one that network stack implementation uses to register. But network implementation in Seastar do not directly call `register_stack` method, instead, Seastar provides another class, `network_stack_registrator` as a warper to call `register_stack` method.

    * Two network stacks are added by `network_stack_registrator` (I can only globally find two actual usage of the `network_stack_registrator` from the Seastar codebase). The first one is the `posix` stack, the second one is the high-performance `native-stack`. We are more interested in the implementation of the second network stack. 
    
    * `network_stack_registry::register_stack` add a key-value pair to the `_map()` of `network_stack_registry`. The key is the name of the network stack. The value is an intialization function. The options associated with the network stack is added to the `opts` field. 
    
    * The `list()` method simply retrieves all the keys from the `_map()` of `network_stack_registry`.

* After some manipulation, the initialization flow steps into `app_template::run_deprecated`. The command line arguments are passed and all the configured options are stored into `bpo::variables_map configuration`. Finally, `run_deprecated` calls `smp::configure(configuration)` to do the actual seastar initializtion work.

* More to come.

## Seastar Network Device and Queue Initialization

* `void reactor::configure(boost::program_options::variables_map vm)`: At the end of `smp::configure(configuration)`, `engine().configure(configuration)` is called to initialize the network stack. Inside `engine().configure(configuration)` function

* `network_stack_registry::create`: Then the corresponding `create` function registered by the network stack will be called, passing in the current `variable_map` options as argument. Let's consider the `native-stack` as an example. The `native_network_stack::create` function in native-stack.cc is called to initialize the native-stack. 

* `native_network_stack::create`: This function will first call `create_native_net_device` to create a native network device. Depending on the the contents of the configuration parameters, `create_native_net_device` may create different network devices, including virtio, dpdk and xen devices. We focus on creating the dpdk net device first.
    
* `create_dpdk_net_device`: Check the number of the available RTE devices. Then construct a `dpdk::dpdk_device`. Note that the default initialization path for Seastar can only use physical port with index 0.

* `dpdk::dpdk_device constructor`: The function first calls `dpdk_device::init_port_start` to perform basic hardware based intialization, including setting up the RSS. Then the function initiates some data collectors and measurement metrics. 

* Back to `create_native_net_device`, the created `dpdk_device` pointer is made into a shared pointer, then passed into each CPU core for further execution. On each CPU core, if CPU core index < # of hardware queues, then a `dpdk_qp` is constructed by calling `init_local_queue` member function. The CPU weights are set and the constructed queue are configured as a proxy. Finally, `set_local_queue` is called to set up the `dpdk_qp` pointer inside the `_queue` vector of the `dpdk_device`. If CPU core index >= # of hardware queues, then no `dpdk_qp` is constructed. The CPU core creates a proxy_net_device by calling `create_proxy_net_device`. This is Seastar's software simulation of the RSS functionality so that more CPUs can be used with smaller number of queues. We do not really care about this functionality. 

* `init_local_queue` is a virtual function, the actual implementation is directed to the implementation of `dpdk_device`. A `dpdk_qp` is constructed and then an annoymous function is sent to each CPU. When all the queues are constructed, `init_port_fini` is called to finalize the initilization.

* `dpdk_qp constructor` sets up the real queues using DPDK api function and sets up some measurement metrics.

* `init_port_fini` will be called when all the required queues are successfully set up. `init_port_fini` will wait for the link status of the DPDK device. If the link status is OK, the `_link_ready_promise` of the `dpdk_device` is set to indicate that the queues have finishes setting up.

* Ownership: Currently I'm not clear the ownership of the `dpdk_device`. But the `dpdk_qp` is finally owned by an annoymous function stored by the engine(). And `dpdk_qp` is safe to hold a pointer to the `dpdk_device`. 

* Back to `create_native_net_device`, when the annonymous function submitted to each CPU core finishes executing, the function will flip a semaphore. Finally, `create_native_net_device` waits for the `_link_ready_promise` that is set inside `init_port_fini` to become ready. Then `create_native_net_device` starts to create the network stack.

## Sestar Native Network Stack Initialization

* In `native_network_stack::create`, `native_network_stack`'s `ready_promise` is finally returned. The `ready_promise` is a thread local variable in that every thread has one of one `ready_promise`. The Seastar program will wait on the `ready_promise` so that the native network stack is successfully initialized.

* In `create_native_stack`, the `ready_promise` on the CPU core is finally set after constructing a `native_network_stack`.

* `native_network_stack constructor`: First, construct `interface _netif`, then construct `ipv4 _inet`. Then, if `_dhcp` is not configured, the `ipv4 _inet` will have an IP address.

* `interface _inet constructor`: Store the shared pointer in the `dpdk_device`. Create a `subscription<packet> _rx`, whose listening funciton is actually `dispatch_packet(std::move(p))`, that is called when the `dpdk_qp` receives a packet. Also, For each `dpdk_qp`, the `rx_start()` member function is called when creating the `_rx` for the `_inet`. `rx_start()` added a poller to the reactor. Then the hardware address of the device is set, the hardware feature is also set. Finally, for the `dpdk_qp` associated with the current CPU core, a `packet_provider` is created and pushed into the `_packet_providers` vector of the `qp` base class. (We got to figure out what `_packet_providers` do actually. )

* `ipv4 _inet constructor`: Hell, this function constructs a lot of things. The most important ones should be a subscription for the produce function that is called by the `qp`. In `_rx_packets`, `handle_received_packet` is called after `dispatch_packet`, then the stream tranfering is over. Before `handle_received_packet` is over, `l4->received` is called. Depending on the l4 protocol, tcp/icmp/udp protocol stack may actually be used. Let's see that the protocol is TCP, then the `tcp<InetTraits>::received` in tcp.hh will finally be called to go over the TCP stack.     

## Seastar Natvie Network Stack UDP Code Analysis

* The UDP part of the Seastar network stack is very close to our NFV definition. We can treat the UDP part as our starting point. I will analyze how the UDP part of Seastar native network stack works, using the test/udp_client.cc and test/udp_server.cc as example.

* Setup: Two Seastar programs, one running test/udp_client.cc, another one running test/udp_server.cc

* The server receive path:

1. Normal system initialization as I have discussed before.

2. The `start` funciton of `client`. First, `start` asks the `native_network_stack` to create a udp channel by calling `engine.net().make_udp_channel()`, the actual call path is:
`engine.net().make_udp_channel()` ->
`native_network_stack::make_udp_channel` ->
`ipv4_udp::make_channel`->
`constructor of native_channel in udp.cc` ->
`constructor of udp_channel`.
In particular, in the call of `ipv4_udp::make_channel`, a `udp_channel_state` is created and assigned to a lightweight shared pointer, called `chan_state`. The `chan_state` is put into an `unordered_map` with the corresponding UDP port as the indexing key. The `chan_state` is also passed to the `native_channel`.

3. The call path of when a packet is received:
`dpdk_qp<HugetlbfsMemBackend>::poll_rx_once` ->
`dpdk_qp<HugetlbfsMemBackend>::process_packets` ->
`dpdk_qp._rx_stream.produce()` ->
`future<> interface::dispatch_packet(packet p)` ->
`l3_rx_stream l3.packet_stream.produce(std::move(p), from)` ->
`future<> ipv4::handle_received_packet(packet p, ethernet_address from)`->
`l4->received(std::move(ip_data), h.src_ip, h.dst_ip)` (considering UDP, it's actually `ipv4_udp::received`) ->
`chan->_queue.push(std::move(dgram))` ->
`queue<udp_datagram>::notify_not_empty()` ->
`[this] (auto) { n_received++;}` in test/udp_client.cc

## NAT and connection tracking

* Refer to https://people.netfilter.org/pablo/docs/login.pdf this artical for a simple explanation of linux netfilter connection tracking.

* OVS now support connection tracking to, they are in lib/conntrack*

## udp_client start up exception

* To correctly start udp_client, we have to add --server ip program option to udp_client binary. However, if there are no udp_server, udp_client generates tons of uncaught exceptions which are arp_queue_full_error. Got to figure out the cause.
