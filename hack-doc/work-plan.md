Aug 11:

1. On the receiver side, modify `future<> interface::dispatch_packet(packet p)`. Depending on the ethernet header, either forward th packet to the NFV processing logic, or forward the packet to the Seastar TCP/IP stack.

2. For the NFV stack, we should create two abstractions.
  * General purpose packet stream (`nfv_raw_packet_stream`) : dispacket_packet function should feed NFV raw packets into this stream. The subscription part of this stream should handle the actual NFV processing.
  * Then, based on the `nfv_raw_packet_stream`, we can continue to build other high-level abstraction, such as `nfv_per_flow_stream`.

3. Set up a VM-based testbed. We should run at least 3 VMs.
  * The first VM generates traffic, using either DPDK packet trafficgen, or Seastar.
  * The second VM runs NF.
  * The last VM receives the generated traffic.

Nov 29.
1. Refactor, remove boilerplates and replace them with helper utility functions.
2. Refactor, change how the async loop run. After the async loop get the packet,
the async loop should run the preprocessing first, then process the packet.
