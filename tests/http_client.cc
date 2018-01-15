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
 * Copyright (C) 2015 Cloudius Systems, Ltd.
 */

#include "http/http_response_parser.hh"
#include "core/print.hh"
#include "core/reactor.hh"
#include "core/app-template.hh"
#include "core/future-util.hh"
#include "core/distributed.hh"
#include "core/semaphore.hh"
#include "core/future-util.hh"
#include "core/sleep.hh"
#include <chrono>
#include <cstring>

using namespace seastar;
using namespace net;
using namespace std::chrono_literals;

#define HTTP_DEBUG 0
const char *http_request = "GET http://202.45.128.157:10000/ HTTP/1.0\r\nHost: 202.45.128.157:10000\r\n\r\n";
std::chrono::time_point<std::chrono::steady_clock> started;

template <typename... Args>
void http_debug(const char* fmt, Args&&... args) {
#if HTTP_DEBUG
    print(fmt, std::forward<Args>(args)...);
#endif
}

class http_client {
private:
    unsigned _duration;
    unsigned _reqs_per_core;
    semaphore _conn_connected{0};
    semaphore _conn_aviliable{0};
    semaphore _conn_finished{0};
    timer<> _run_timer;
    bool _timer_based;
    bool _timer_done{false};
    uint64_t _total_reqs{0};
    uint64_t _nr_done{0};
    uint64_t _nr_started{0};
    ipv4_addr _server_addr;
public:
    http_client(unsigned duration, unsigned reqs_per_core, unsigned max_para_conn)
        : _duration(duration)
        , _reqs_per_core(reqs_per_core)
        , _run_timer([this] { _timer_done = true; })
        , _timer_based(reqs_per_core == 0) {
        _conn_aviliable.signal(max_para_conn);
    }

    class connection {
    private:
        connected_socket _fd;
        input_stream<char> _read_buf;
        output_stream<char> _write_buf;
        http_response_parser _parser;
        http_client* _http_client;
        uint64_t _nr_done{0};
        size_t _no;
    public:
        connection(connected_socket&& fd, http_client* client, size_t no)
            : _fd(std::move(fd))
            , _read_buf(_fd.input())
            , _write_buf(_fd.output())
            , _http_client(client)
            , _no(no){
        }

        void shutdown() {
            _fd.shutdown_output();
            _fd.shutdown_input();
        }

        size_t get_no() {
            return _no;
        }

        uint64_t nr_done() {
            return _nr_done;
        }

        future<> do_req_once() {
            return _write_buf.write(http_request).then([this] {
                return _write_buf.flush();
            }).then([this] {
                _parser.init();
                return _read_buf.consume(_parser).then([this] {
                    // Read HTTP response header first
                    if (_parser.eof()) {
                        return make_ready_future<>();
                    }
                    auto _rsp = _parser.get_parsed_response();
                    auto it = _rsp->_headers.find("Content-Length");
                    if (it == _rsp->_headers.end()) {
                        //print("Error: HTTP response does not contain: Content-Length\n");
                        //return make_ready_future<>();
                        return _read_buf.read().then([this] (temporary_buffer<char> buf) {
                            _nr_done++;
                            http_debug("%s\n", buf.get());
                            if(strncmp(buf.get(), "\"hello\"", 7) && strncmp(buf.get(), "hello", 5)){
                                print("May get wrong response content: %s\n", buf.get());
                            }
                            return make_ready_future();
                        });
                    }
                    auto content_len = std::stoi(it->second);
                    http_debug("Content-Length = %d\n", content_len);
                    // Read HTTP response body
                    return _read_buf.read_exactly(content_len).then([this] (temporary_buffer<char> buf) {
                        _nr_done++;
                        http_debug("%s\n", buf.get());
                        if(strncmp(buf.get(), "\"hello\"", 7) && strncmp(buf.get(), "hello", 5)){
                            print("May get wrong response content: %s\n", buf.get());
                        }
                        return make_ready_future();
                    });
                });
            });
        }
    };

    future<> do_req() {
        return _conn_aviliable.wait(1).then_wrapped([this] (auto &&f) {
            if(f.failed()){
                print("do_req(): _conn_aviliable.wait(1) failed.\n");
                return std::move(f);
            }

            http_debug("Start establishing connection %6d on cpu %3d\n", _conn_connected.current(), engine().cpu_id());
            engine().net().connect(make_ipv4_address(_server_addr)).then([this] (connected_socket fd) {
                http_debug("Established connection %6d on cpu %3d\n", _conn_connected.current(), engine().cpu_id());
                auto conn = new connection(std::move(fd), this, _conn_connected.current());
                _conn_connected.signal();

                return conn->do_req_once().then_wrapped([conn, this] (auto &&f) {
                    conn->shutdown();
                    return std::move(f);
                }).then_wrapped([conn, this] (auto&& f) {
                    size_t no = conn->get_no();
                    http_debug("Finished connection %6d on cpu %3d\n", no, engine().cpu_id());
                    _conn_finished.signal();
                    _total_reqs += 1;
                    delete conn;
                    _conn_aviliable.signal(1);
                    try {
                        f.get();
                        http_debug("Successful connection %6d on cpu %3d\n", no, engine().cpu_id());
                        return make_ready_future();
                    } catch (std::exception& ex) {
                        print("do_req(): http request error: %s\n", ex.what());
                        return make_ready_future();
                    } catch(...) {
                        print("do_req(): http request error: Unknown error\n");
                        return make_ready_future();
                    }
                });
            }).or_terminate();
            http_debug("After starting establishing connection %6d on cpu %3d\n", _conn_connected.current(), engine().cpu_id());

            _nr_started++;
            if(this->done(_nr_started)) {
                return _conn_finished.wait(_reqs_per_core).handle_exception([] (auto &&f) {
                    print("do_req(): _conn_finished.wait(_reqs_per_core) failed.\n");
                    return make_ready_future();
                });
            }
            else {
                return this->do_req();
            }
        });
    }
   

   

    future<> run(ipv4_addr server_addr) {
        if (_timer_based) {
            _run_timer.arm(std::chrono::seconds(_duration));
        }

        _server_addr = std::move(server_addr);

        return do_req().then_wrapped([] (auto &&f) {
            try{
                f.get();
            }
            catch(std::system_error &e){
                std::cout << "Caught system_error with code " << e.code() 
                  << " meaning " << e.what() << '\n';
            }
            catch(...){
                print("run(): Unknown error in do_req()\n");
            }
            
            return make_ready_future();
        });
    }

    future<uint64_t> total_reqs() {
        print("Requests on cpu %2d: %ld\n", engine().cpu_id(), _total_reqs);
        return make_ready_future<uint64_t>(_total_reqs);
    }

    bool done(uint64_t nr_done) {
        if (_timer_based) {
            return _timer_done;
        } else {
            return nr_done >= _reqs_per_core;
        }
    }

    future<> stop() {
        return make_ready_future();
    }

};

future<int> test_once(std::string server, unsigned max_para_conn, unsigned total_reqs, unsigned duration) {
    if (total_reqs % smp::count != 0) {
        print("Error: reqs needs to be n * cpu_nr\n");
        return make_ready_future<int>(-1);
    }

    auto reqs_per_core = total_reqs / smp::count;
    auto http_clients = new distributed<http_client>;
    // Start http requests on all the cores
    print("========== http_client ============\n");
    print("Server: %s\n", server);
    print("Requests: %u\n", total_reqs);
    print("Requests/core: %s\n", reqs_per_core == 0 ? "dynamic (timer based)" : std::to_string(reqs_per_core));
    return http_clients->start(std::move(duration), std::move(reqs_per_core), std::move(max_para_conn)).then([] {
        started = steady_clock_type::now();
    }).then([http_clients, server] {
        return http_clients->invoke_on_all(&http_client::run, ipv4_addr{server});
    }).then([http_clients] {
        return http_clients->map_reduce(adder<uint64_t>(), &http_client::total_reqs);
    }).then([http_clients] (auto total_reqs) {
       // All the http requests are finished
       auto finished = steady_clock_type::now();
       auto elapsed = finished - started;
       auto secs = static_cast<double>(elapsed.count() / 1000000000.0);
       print("Total cpus: %u\n", smp::count);
       print("Total requests: %u\n", total_reqs);
       print("Total time: %f\n", secs);
       print("Requests/sec: %f\n", static_cast<double>(total_reqs) / secs);
       print("==========     done     ============\n");
       return http_clients->stop().then([http_clients] {
           // FIXME: If we call engine().exit(0) here to exit when
           // requests are done. The tcp connection will not be closed
           // properly, becasue we exit too earily and the FIN packets are
           // not exchanged.
            delete http_clients;
            return make_ready_future<int>(0);
       });
    }).then_wrapped([] (auto &&f) {
        try{
            f.get();
            return make_ready_future<int>(0);
        }
        catch(...){
            return make_ready_future<int>(-1);
        }
    });
}


namespace bpo = boost::program_options;

int main(int ac, char** av) {
    app_template app;
    app.add_options()
        ("server,s", bpo::value<std::string>()->default_value("192.168.66.100:10000"), "Server address")
        ("first-conn,C", bpo::value<unsigned>()->default_value(1), "max parallel connections for first test")
        ("first-reqs,R", bpo::value<unsigned>()->default_value(0), "total reqs (must be n * cpu_nr) for first test")
        ("first-duration,D", bpo::value<unsigned>()->default_value(5), "duration of the test in seconds for first test")
        ("conn,c", bpo::value<unsigned>()->default_value(10), "max parallel connections")
        ("reqs,r", bpo::value<unsigned>()->default_value(0), "total reqs (must be n * cpu_nr)")
        ("duration,d", bpo::value<unsigned>()->default_value(10), "duration of the test in seconds");

    return app.run(ac, av, [&app] () -> future<int> {
        auto& config = app.configuration();
        auto server = config["server"].as<std::string>();
        auto first_max_para_conn = config["first-conn"].as<unsigned>();
        auto max_para_conn = config["conn"].as<unsigned>();
        auto first_total_reqs= config["first-reqs"].as<unsigned>();
        auto total_reqs= config["reqs"].as<unsigned>();
        auto first_duration = config["first-duration"].as<unsigned>();
        auto duration = config["duration"].as<unsigned>();

        return test_once(server, first_max_para_conn, first_total_reqs, first_duration).then([server, max_para_conn, total_reqs, duration] (int res) {
            if(res){
                print("\x1B[31mFirst test failed. Can not guarantee the correctness of next test.\x1B[0m\n");
            }
            return test_once(server, max_para_conn, total_reqs, duration);
        });
    });
}
