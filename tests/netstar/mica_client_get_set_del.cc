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

#include "core/reactor.hh"
#include "core/app-template.hh"
#include "core/print.hh"
#include "core/distributed.hh"
#include "netstar/netstar_dpdk_device.hh"
#include "netstar/fdir_device.hh"
#include "netstar/port.hh"
#include "netstar/per_core_objs.hh"
#include "netstar/mica_client.hh"
#include "netstar/extendable_buffer.hh"

using namespace seastar;
using namespace netstar;

int main(int ac, char** av) {
    app_template app;
    ports_env all_ports;
    per_core_objs<mica_client> all_objs;
    vector<vector<port_pair>> queue_map;

    return app.run_deprecated(ac, av, [&app, &all_ports, &all_objs, &queue_map]{
        auto& opts = app.configuration();
        return all_ports.add_port(opts, 1, smp::count,
            [](uint16_t port_id, uint16_t queue_num){
                return create_fdir_device(port_id);
        }).then([&all_objs]{
            return all_objs.start(&all_objs);
        }).then([&all_ports, &all_objs]{
            return all_objs.invoke_on_all([&all_ports](mica_client& mc){
                mc.configure_ports(all_ports, 0, 0);
            });
        }).then([&opts, &all_ports, &queue_map]{
            queue_map = calculate_queue_mapping(
                    opts, all_ports.get_ports(0).local_obj());
        }).then([&all_objs, &opts, &queue_map]{
            return all_objs.invoke_on_all([&opts, &queue_map](mica_client& mc){
                mc.bootup(opts, queue_map);
            });
        }).then([&all_objs]{
            return all_objs.invoke_on_all([](mica_client& mc){
                mc.start_receiving();
            });
        }).then([&all_objs]{
            uint64_t key = 10276325;
            extendable_buffer key_buf;
            key_buf.fill_data(key);

            uint64_t val = 8721;
            extendable_buffer val_buf;
            val_buf.fill_data(val);

            printf("Trying to set key %zu to value %zu\n", key, val);
            return all_objs.local_obj().query(Operation::kSet,
                    sizeof(key), key_buf.get_temp_buffer(),
                    sizeof(val), val_buf.get_temp_buffer()).then([key, val](mica_response response){
                assert(response.get_key_len() == 0);
                assert(response.get_val_len() == 0);
                assert(response.get_result() == Result::kSuccess);
                printf("The key %zu is set to value %zu\n", key, val);
            });
        }).then([&all_objs]{
            uint64_t key = 10276325;
            extendable_buffer key_buf;
            key_buf.fill_data(key);

            printf("Trying to read key %zu\n", key);
            return all_objs.local_obj().query(Operation::kGet,
                    sizeof(key), key_buf.get_temp_buffer(),
                    0, temporary_buffer<char>()).then([key](mica_response response){
                assert(response.get_key_len() == 0);
                assert(response.get_val_len() == sizeof(uint64_t));
                assert(response.get_result() == Result::kSuccess);
                assert(response.get_value<uint64_t>() == 8721);
                printf("The key %zu is read as value %zu\n", key, response.get_value<uint64_t>());
            });
        }).then([&all_objs]{
            uint64_t key = 10276325;
            extendable_buffer key_buf;
            key_buf.fill_data(key);

            printf("Trying to delete key %zu\n", key);
            return all_objs.local_obj().query(Operation::kDelete,
                    sizeof(key), key_buf.get_temp_buffer(),
                    0, temporary_buffer<char>()).then([key](mica_response response){
                /*assert(response.get_key_len() == 0);
                assert(response.get_val_len() == sizeof(uint64_t));
                assert(response.get_result() == Result::kSuccess);
                printf("The key %zu is read as value %zu\n", key, response.get_value<uint64_t>());*/
                auto op = static_cast<uint8_t>(response.get_operation());
                auto r = static_cast<uint8_t>(response.get_result());
                printf("Operation %d, result %d\n", op, r);
                auto key_len = response.get_key_len();
                auto val_len = response.get_val_len();
                std::cout<<"key_len "<<key_len<<" val_len "<<val_len<<std::endl;
            });
        }).then_wrapped([](auto&& f){
            try{
                f.get();
                printf("The mica client is successfully booted up\n");
                engine().exit(0);

            }
            catch(...){
                printf("Failure happens\n");
                engine().exit(0);
            }
        });
    });
}