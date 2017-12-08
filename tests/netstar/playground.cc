/*
 * This file is open source software, licensed to you under the terms
 * of the Apache License, Version 2.0 (the "License").  See the NOTICE file
 * distributed with this work for additional information regarding copyright
 * ownership.  You may not use this file except in compliance with the License.
 *
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */
/*
 * Copyright (C) 2014 Cloudius Systems, Ltd.
 */

#include "netstar/port.hh"
#include "core/reactor.hh"
#include "core/app-template.hh"
#include "core/print.hh"
#include "core/distributed.hh"

#include "netstar/per_core_objs.hh"
#include "netstar/mica_client.hh"
#include "netstar/extendable_buffer.hh"
#include "netstar/stack_port.hh"
#include "netstar/port_env.hh"
#include "netstar/af/async_flow.hh"

#include "net/ip.hh"
#include "net/byteorder.hh"

#include <array>

using namespace seastar;
using namespace netstar;
using namespace std::chrono_literals;

enum class dummy_udp_events : uint8_t{
    pkt_in=0
};

class dummy_udp_ppr{
private:
    bool _is_client;
public:
    using EventEnumType = dummy_udp_events;
    using FlowKeyType = net::l4connid<net::ipv4_traits>;
    using HashFunc = net::l4connid<net::ipv4_traits>::connid_hash;

    dummy_udp_ppr(bool is_client)
        : _is_client(is_client) {
    }

public:
    generated_events<EventEnumType> handle_packet_send(net::packet& pkt){
        generated_events<EventEnumType> ge;
        ge.event_happen(dummy_udp_events::pkt_in);
        return ge;
    }

    generated_events<EventEnumType> handle_packet_recv(net::packet& pkt){
        generated_events<EventEnumType> ge;
        ge.event_happen(dummy_udp_events::pkt_in);
        return ge;
    }

    FlowKeyType get_reverse_flow_key(net::packet& pkt){
        auto ip_hd_ptr = pkt.get_header<net::ip_hdr>(sizeof(net::eth_hdr));
        auto udp_hd_ptr = pkt.get_header<net::udp_hdr>(sizeof(net::eth_hdr)+sizeof(net::ip_hdr));
        return FlowKeyType{net::ntoh(ip_hd_ptr->src_ip),
                           net::ntoh(ip_hd_ptr->dst_ip),
                           net::ntoh(udp_hd_ptr->src_port),
                           net::ntoh(udp_hd_ptr->dst_port)};
    }

public:
    struct async_flow_config {
        static constexpr int max_event_context_queue_size = 5;
        static constexpr int new_flow_queue_size = 100;
        static constexpr int max_flow_table_size = 10000;
        static constexpr int max_directions = 2;

        static FlowKeyType get_flow_key(net::packet& pkt){
            auto ip_hd_ptr = pkt.get_header<net::ip_hdr>(sizeof(net::eth_hdr));
            auto udp_hd_ptr = pkt.get_header<net::udp_hdr>(sizeof(net::eth_hdr)+sizeof(net::ip_hdr));
            return FlowKeyType{net::ntoh(ip_hd_ptr->dst_ip),
                               net::ntoh(ip_hd_ptr->src_ip),
                               net::ntoh(udp_hd_ptr->dst_port),
                               net::ntoh(udp_hd_ptr->src_port)};
        }

        static net::packet build_pkt(const char* buf){
            ipv4_addr ipv4_src_addr("10.1.2.4:666");
            ipv4_addr ipv4_dst_addr("10.1.2.5:666");
            auto pkt = net::packet::from_static_data(buf, strlen(buf));

            auto hdr = pkt.prepend_header<net::udp_hdr>();
            hdr->src_port = ipv4_src_addr.port;
            hdr->dst_port = ipv4_dst_addr.port;
            hdr->len = pkt.len();
            *hdr = net::hton(*hdr);

            net::checksummer csum;
            net::ipv4_traits::udp_pseudo_header_checksum(csum, ipv4_src_addr, ipv4_dst_addr, pkt.len());
            csum.sum(pkt);
            hdr->cksum = csum.get();

            net::offload_info oi;
            oi.needs_csum = false;
            oi.protocol = net::ip_protocol_num::udp;
            pkt.set_offload_info(oi);

            auto iph = pkt.prepend_header<net::ip_hdr>();
            iph->ihl = sizeof(*iph) / 4;
            iph->ver = 4;
            iph->dscp = 0;
            iph->ecn = 0;
            iph->len = pkt.len();
            iph->id = 0;
            iph->frag = 0;
            iph->ttl = 64;
            iph->ip_proto = (uint8_t)net::ip_protocol_num::udp;
            iph->csum = 0;
            iph->src_ip = net::ipv4_address(ipv4_src_addr.ip);
            iph->dst_ip = net::ipv4_address(ipv4_dst_addr.ip);
            *iph = net::hton(*iph);
            net::checksummer ip_csum;
            ip_csum.sum(reinterpret_cast<char*>(iph), sizeof(*iph));
            iph->csum = csum.get();

            auto eh = pkt.prepend_header<net::eth_hdr>();
            net::ethernet_address eth_src{0x3c, 0xfd, 0xfe, 0x06, 0x07, 0x82};
            net::ethernet_address eth_dst{0x3c, 0xfd, 0xfe, 0x06, 0x09, 0x62};
            eh->dst_mac = eth_dst;
            eh->src_mac = eth_src;
            eh->eth_proto = uint16_t(net::eth_protocol_num::ipv4);
            *eh = net::hton(*eh);

            pkt.linearize();
            assert(pkt.nr_frags() == 1);
            return pkt;
        }
    };
};

class async_flow_loop {
    client_async_flow<dummy_udp_ppr> _af;
public:
    async_flow_loop(client_async_flow<dummy_udp_ppr> af)
        : _af(std::move(af)){
    }

    void configure() {
        _af.register_events(af_send_recv::send, dummy_udp_events::pkt_in);
    }

    future<> run() {
        auto f = _af.run_async_loop([](){
            printf("async loop runs!\n");

            return make_ready_future<af_action>(af_action::forward);
        });
        return f.then([af = std::move(_af)](){});
    }

    /*future<> run() {
        _af.on_client_side_events().then([](client_accessor cac){

            auto& pkt = _af.get_pkt_ref(cac);

            auto& wu = _af.get_work_unit(cac);
            auto data = wu.read_data();

            auto result = detection_engine().feed(std::move(data));



            return _mica_client.query(xxx);
        }).then([this](response){
            _af.destroy_context(std::move(cac));
        });
    }*/

    /* final api 1:
     * af_ptr = _af.get();
     *
     * af_ptr->run_client_async_loop([af_ptr](client_accessor ca){
     *    af_ptr->update_packet(ca);
     *    af_ptr->check_for_event(ca);
     *
     *    return mica_client.query(xxx).then([std::move(ca), af_ptr](){
     *          af_ptr->update_packet(ca);
     *          ca.set_post_action(drop);
     *          return ca;
     *    });
     * }).then([std::move(_af)](){
     *
     * });
     *
     * final api 2:
     *
     * 1. choose a simplified API design. Use this.
     * 2. We can add a macro to choose whether protect the
     * async loop.
     * 3.
     *
     * af_ptr = _af.get();
     * af_ptr->run_client_async_loop([af_ptr](){
     *    af_ptr->update_client_packet();
     *    af_ptr->check_for_client_event();
     *
     *    return mica_client.query(xxx).then([af_ptr](){
     *          af_ptr->update_client_packet();
     *          return drop;
     *    })
     * }).then([std::move(_af)](){
     *
     * });
     *
     * final api 3:
     *
     * may abort the entire program.
     *
     */
};

struct dummy{
    int i;
    dummy(int i_arg) : i(i_arg){}
    ~dummy(){
        printf("dummy is deconstructed\n");
    }
};

int main(int ac, char** av) {
    app_template app;
    timer<steady_clock_type> to;
    async_flow_manager<dummy_udp_ppr> manager;
    async_flow_manager<dummy_udp_ppr>::external_io_direction ingress(0);
    async_flow_manager<dummy_udp_ppr>::external_io_direction egress(1);
    net::packet the_pkt = dummy_udp_ppr::async_flow_config::build_pkt("abcdefg");
    std::experimental::optional<dummy> d;
    d.emplace(1);
    d = {};
    assert(!d);

    return app.run_deprecated(ac, av, [&app, &to, &manager, &ingress, &egress, &the_pkt]{
        ingress.register_to_manager(manager, [](net::packet pkt){return make_ready_future();}, egress);
        egress.register_to_manager(manager, [](net::packet pkt){return make_ready_future();}, ingress);
        net::packet pkt(the_pkt.frag(0));
        dummy_udp_ppr::FlowKeyType fk = dummy_udp_ppr::async_flow_config::get_flow_key(pkt);
        ingress.get_send_stream().produce(std::move(pkt), &fk);

        return manager.on_new_initial_context().then([&manager]() mutable {
            auto ic = manager.get_initial_context();

            do_with(ic.get_client_async_flow(), [](client_async_flow<dummy_udp_ppr>& ac){
                ac.register_events(af_send_recv::send, dummy_udp_events::pkt_in);
                return ac.run_async_loop([ac](){
                    printf("async loop runs!\n");
                    return make_ready_future<af_action>(af_action::forward);
                });
            });
        }).then([](){
            engine().exit(0);
        });
    });
}
