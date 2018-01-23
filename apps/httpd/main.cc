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
 * Copyright 2015 Cloudius Systems
 */

#include "http/httpd.hh"
#include "http/handlers.hh"
#include "http/function_handlers.hh"
#include "http/file_handler.hh"
#include "apps/httpd/demo.json.hh"
#include "http/api_docs.hh"
#include <string>

namespace bpo = boost::program_options;

using namespace seastar;
using namespace httpd;

std::string string_64k = std::string(64 * 1024, 'x');
const char *s_64k = string_64k.c_str();
const char *s_32k = s_64k + (32 * 1024);
const char *s_16k = s_32k + (16 * 1024);
const char *s_8k = s_16k + (8 * 1024);
const char *s_4k = s_8k + (4 * 1024);
const char *s_2k = s_4k + (2 * 1024);
const char *s_1k = s_2k + (1 * 1024);

class handl : public httpd::handler_base {
public:
    virtual future<std::unique_ptr<reply> > handle(const sstring& path,
            std::unique_ptr<request> req, std::unique_ptr<reply> rep) {
        rep->_content = "hello";
        rep->done("html");
        return make_ready_future<std::unique_ptr<reply>>(std::move(rep));
    }
};

void set_routes(routes& r) {
    function_handler* h1 = new function_handler([](const_req req) {
        return "hello";
    });
    function_handler* h64k = new function_handler([](const_req req) {
        return s_64k;
    });
    function_handler* h32k = new function_handler([](const_req req) {
        return s_32k;
    });
    function_handler* h16k = new function_handler([](const_req req) {
        return s_16k;
    });
    function_handler* h8k = new function_handler([](const_req req) {
        return s_8k;
    });
    function_handler* h4k = new function_handler([](const_req req) {
        return s_4k;
    });
    function_handler* h2k = new function_handler([](const_req req) {
        return s_2k;
    });
    function_handler* h1k = new function_handler([](const_req req) {
        return s_1k;
    });
    function_handler* h2 = new function_handler([](std::unique_ptr<request> req) {
        return make_ready_future<json::json_return_type>("json-future");
    });
    r.add(operation_type::GET, url("http://10.28.1.13:10000/"), h1);
    r.add(operation_type::GET, url("/"), h1);
    r.add(operation_type::GET, url("/64k"), h64k);
    r.add(operation_type::GET, url("http://10.28.1.13:10000/32k"), h32k);
    r.add(operation_type::GET, url("/32k"), h32k);
    r.add(operation_type::GET, url("/16k"), h16k);
    r.add(operation_type::GET, url("http://10.28.1.13:10000/8k"), h8k);
    r.add(operation_type::GET, url("/8k"), h8k);
    r.add(operation_type::GET, url("/4k"), h4k);
    r.add(operation_type::GET, url("http://10.28.1.13:10000/2k"), h2k);
    r.add(operation_type::GET, url("/2k"), h2k);
    r.add(operation_type::GET, url("http://10.28.1.13:10000/1k"), h1k);
    r.add(operation_type::GET, url("/1k"), h1k);
    r.add(operation_type::GET, url("/jf"), h2);
    r.add(operation_type::GET, url("/file").remainder("path"),
            new directory_handler("/"));
    demo_json::hello_world.set(r, [] (const_req req) {
        demo_json::my_object obj;
        obj.var1 = req.param.at("var1");
        obj.var2 = req.param.at("var2");
        demo_json::ns_hello_world::query_enum v = demo_json::ns_hello_world::str2query_enum(req.query_parameters.at("query_enum"));
        // This demonstrate enum conversion
        obj.enum_var = v;
        return obj;
    });
}

int main(int ac, char** av) {
    app_template app;
    app.add_options()("port", bpo::value<uint16_t>()->default_value(10000),
            "HTTP Server port");
    return app.run_deprecated(ac, av, [&] {
        auto&& config = app.configuration();
        uint16_t port = config["port"].as<uint16_t>();
        auto server = new http_server_control();
        auto rb = make_shared<api_registry_builder>("apps/httpd/");
        server->start().then([server] {
            return server->set_routes(set_routes);
        }).then([server, rb]{
            return server->set_routes([rb](routes& r){rb->set_api_doc(r);});
        }).then([server, rb]{
            return server->set_routes([rb](routes& r) {rb->register_function(r, "demo", "hello world application");});
        }).then([server, port] {
            return server->listen(port);
        }).then([server, port] {
            std::cout << "Seastar HTTP server listening on port " << port << " ...\n";
            engine().at_exit([server] {
                return server->stop();
            });
        });

    });
}
