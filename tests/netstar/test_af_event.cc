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
#include "netstar/af/async_flow_util.hh"

using namespace seastar;
using namespace netstar::internal;
using namespace netstar;
using namespace std::chrono_literals;

enum class fk_events : uint8_t{
    fk_me,
    fk_you,
    fk_everybody
};

int main(int ac, char** av) {
    /*app_template app;
    timer<steady_clock_type> to;

    return app.run_deprecated(ac, av, [&app, &to]{

    });*/

    {
        registered_events<fk_events> re;
        re.register_event(fk_events::fk_me);
        re.register_event(fk_events::fk_you);

        generated_events<fk_events> ge;
        ge.event_happen(fk_events::fk_me);
        auto fe = re.filter(ge);
        assert(fe.on_event(fk_events::fk_me));
        assert(!fe.on_event(fk_events::fk_you));
        assert(!fe.on_event(fk_events::fk_everybody));
        assert(!fe.on_close_event());
    }

    {
        registered_events<fk_events> re;
        re.register_event(fk_events::fk_me);
        re.register_event(fk_events::fk_you);

        generated_events<fk_events> ge;
        ge.event_happen(fk_events::fk_you);
        auto fe = re.filter(ge);
        assert(!fe.on_event(fk_events::fk_me));
        assert(fe.on_event(fk_events::fk_you));
        assert(!fe.on_event(fk_events::fk_everybody));
        assert(!fe.on_close_event());
    }


    {
        registered_events<fk_events> re;
        re.register_event(fk_events::fk_me);
        re.register_event(fk_events::fk_you);

        generated_events<fk_events> ge;
        ge.event_happen(fk_events::fk_everybody);
        auto fe = re.filter(ge);
        assert(!fe.on_event(fk_events::fk_me));
        assert(!fe.on_event(fk_events::fk_you));
        assert(!fe.on_event(fk_events::fk_everybody));
        assert(!fe.on_close_event());
    }

    {
        registered_events<fk_events> re;
        re.register_event(fk_events::fk_me);
        re.register_event(fk_events::fk_you);

        generated_events<fk_events> ge;
        ge.close_event_happen();
        auto fe = re.filter(ge);
        assert(!fe.on_event(fk_events::fk_me));
        assert(!fe.on_event(fk_events::fk_you));
        assert(!fe.on_event(fk_events::fk_everybody));
        assert(fe.on_close_event());
    }

    {
        registered_events<fk_events> re;
        re.register_event(fk_events::fk_me);
        re.register_event(fk_events::fk_you);
        re.unregister_event(fk_events::fk_me);

        generated_events<fk_events> ge;
        ge.event_happen(fk_events::fk_me);
        auto fe = re.filter(ge);
        assert(!fe.on_event(fk_events::fk_me));
        assert(!fe.on_event(fk_events::fk_you));
        assert(!fe.on_event(fk_events::fk_everybody));
        assert(!fe.on_close_event());
    }
}

/*class flow_processor {
    async_flow<TCP> _af;
    gate _g;

};

do_with(flow_processor(std::move(af)), [](auto& obj){
    _g.enter();
    repeat([obj]{
        return obj.on_client_side_events().then([](bool side_flag){
            auto cur_context = obj.get_current_context(side_flag);
            if(cur_context.close_event_happen()){
                return iteration::no;
            }
            else{
                cur_context.event_happen<fk_event::wtf>(send){

                }
            }
        });
    }).then([obj]{
        obj_g.leave();
    })

    _g.enter();
    repeat([obj]{

    }).then([obj]{
        obj_g.leave();
    })

    return _g.close();
});*/
