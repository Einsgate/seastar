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
#include "netstar/tcp_monitoring_flow.hh"

using namespace seastar;
using namespace netstar;
using namespace std::chrono_literals;

class tester{
    lw_shared_ptr<tcp_monitor> _monitor;
public:
    tester(lw_shared_ptr<tcp_monitor> monitor)
        : _monitor(std::move(monitor)){
    }

    future<> run(){
        return _monitor->on_new_packet().then([this]{
            printf("Monitor receives packet\n");
            _monitor->read_packet();
            if(_monitor->is_impl_ended()){
                return make_ready_future<>();
            }
            else{
                return run();
            }
        });
    }
};

int main(int ac, char** av) {
    app_template app;
    timer<steady_clock_type> to;

    return app.run_deprecated(ac, av, [&app, &to]{
        auto mon_impl = make_lw_shared<netstar::internal::tcp_monitor_impl>();
        auto mon = make_lw_shared<tcp_monitor>(mon_impl);
        auto tester_ptr = new tester(std::move(mon));

        to.set_callback([mon_impl]{
            printf("Timer called\n");
            mon_impl->receive_pkt(net::packet(), direction::EGRESS);
        });
        to.arm_periodic(1s);

        return tester_ptr->run().then([tester_ptr]{
            delete tester_ptr;
        });
    });
}
