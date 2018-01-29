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
#include <vector>
#include <fstream>

using namespace seastar;
using namespace std;

string http_request = string("");
string rtt_path = string("/home/net/jjwang/testrtt");
std::chrono::time_point<std::chrono::steady_clock> started;

static int MY_HTTP_DEBUG = 0;

template <typename... Args>
void http_debug(const char* fmt, Args&&... args) {
if(MY_HTTP_DEBUG)
    print(fmt, std::forward<Args>(args)...);
}

class rtt_test {
    using timePoint = std::chrono::time_point<std::chrono::steady_clock>;
    using arithType = chrono::steady_clock::rep;
    timePoint _begin;
    timePoint _end;
    unsigned _interval;
    unsigned _counter = 0;
    unsigned _max_results = 1000;
    vector<arithType> _results;
    fstream _store_file;
public:
    explicit rtt_test(string path, unsigned interval = 1000) : _interval(interval) {
        _store_file.open(path, ofstream::out);
        if(!_store_file) {
            assert(0 && "Can not open the specific file.");
        }
    }

    inline void start() {
        if(_results.size() < _max_results && ++_counter >= _interval)
            _begin = steady_clock_type::now();
    }

    inline void stop() {
        if(_results.size() < _max_results && _counter >= _interval) {
            _counter = 0;
            _end = steady_clock_type::now();
            auto elapsed = _end - _begin;
            //auto secs = static_cast<double>(elapsed.count() / 1000000000.0);
            //return elapsed;
            _results.push_back(elapsed.count());
        }
    }

    inline auto average() {
        arithType total = 0;
        for(auto &res : _results) {
            total += res;
            _store_file << res << '\n';
        }

        auto avg = total / _results.size();
        _store_file << '\n' << avg << '\n';
        return avg;
    }
};

rtt_test *rtt[20] = {0};
bool global_rtt_mark[20] = {0};

class http_client {
private:
    unsigned _duration;
    unsigned _conn_per_core;
    unsigned _reqs_per_conn;
    std::vector<connected_socket> _sockets;
    semaphore _conn_connected{0};
    semaphore _conn_finished{0};
    timer<> _run_timer;
    bool _timer_based;
    bool _timer_done{false};
    uint64_t _total_reqs{0};
    
public:
    http_client(unsigned duration, unsigned total_conn, unsigned reqs_per_conn)
        : _duration(duration)
        , _conn_per_core(total_conn / smp::count)
        , _reqs_per_conn(reqs_per_conn)
        , _run_timer([this] { _timer_done = true; })
        , _timer_based(reqs_per_conn == 0) {
        
    }

    class connection {
    private:
        connected_socket _fd;
        input_stream<char> _read_buf;
        output_stream<char> _write_buf;
        http_response_parser _parser;
        http_client* _http_client;
        uint64_t _nr_done{0};
        bool _rtt_mark = false;
    public:
        connection(connected_socket&& fd, http_client* client)
            : _fd(std::move(fd))
            , _read_buf(_fd.input())
            , _write_buf(_fd.output())
            , _http_client(client){
            unsigned int id = engine().cpu_id();
            _rtt_mark = global_rtt_mark[id];
            global_rtt_mark[id] = false;
        }

        uint64_t nr_done() {
            return _nr_done;
        }

        future<> do_req() {
            unsigned int id = engine().cpu_id();
            if(_rtt_mark && rtt[id]){
                rtt[id]->start();
            }

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
                        print("Error: HTTP response does not contain: Content-Length\n");
                        return make_ready_future<>();
                    }
                    auto content_len = std::stoi(it->second);
                    //http_debug("Content-Length = %d\n", content_len);
                    //if(content_len != 8194)
                    //    print("content_len = %d ", content_len);
                    // Read HTTP response body
                    return _read_buf.read_exactly(content_len).then([this] (temporary_buffer<char> buf) {
                        _nr_done++;
                        //http_debug("%s\n", buf.get());
                        //if(*(buf.get()) != '"'){
                        //    print("May get wrong response content: %s\n", buf.get());
                        //}
                        unsigned int id = engine().cpu_id();
                        if(_rtt_mark && rtt[id]){
                            rtt[id]->stop();
                        }

                        if (_http_client->done(_nr_done)) {
                            return make_ready_future();
                        } else {
                            return do_req();
                        }
                    });
                });
            });
        }
    };

    future<uint64_t> total_reqs() {
        print("Requests on cpu %2d: %ld\n", engine().cpu_id(), _total_reqs);
        return make_ready_future<uint64_t>(_total_reqs);
    }

    bool done(uint64_t nr_done) {
        if (_timer_based) {
            return _timer_done;
        } else {
            return nr_done >= _reqs_per_conn;
        }
    }

    future<> connect(ipv4_addr server_addr) {
        // Establish all the TCP connections first
        for (unsigned i = 0; i < _conn_per_core; i++) {
            engine().net().connect(make_ipv4_address(server_addr)).then([this] (connected_socket fd) {
                _sockets.push_back(std::move(fd));
                http_debug("Established connection %6d on cpu %3d\n", _conn_connected.current(), engine().cpu_id());
                _conn_connected.signal();
            }).or_terminate();
        }
        return _conn_connected.wait(_conn_per_core);
    }

    future<> run() {
        // All connected, start HTTP request
        http_debug("Established all %6d tcp connections on cpu %3d\n", _conn_per_core, engine().cpu_id());
        if (_timer_based) {
            _run_timer.arm(std::chrono::seconds(_duration));
        }
        for (auto&& fd : _sockets) {
            auto conn = new connection(std::move(fd), this);
            conn->do_req().then_wrapped([this, conn] (auto&& f) {
                http_debug("Finished connection %6d on cpu %3d\n", _conn_finished.current(), engine().cpu_id());
                _total_reqs += conn->nr_done();
                _conn_finished.signal();
                delete conn;
                try {
                    f.get();
                } catch (std::exception& ex) {
                    print("http request error: %s\n", ex.what());
                }
            });
        }

        // All finished
        return _conn_finished.wait(_conn_per_core);
    }
    future<> stop() {
        return make_ready_future();
    }
};

namespace bpo = boost::program_options;

future<int> test_once(string server, unsigned reqs_per_conn, unsigned total_conn, unsigned duration) {
    auto http_clients = new distributed<http_client>;

    // Start http requests on all the cores
    print("========== http_client ============\n");
    print("Server: %s\n", server);
    print("Connections: %u\n", total_conn);
    print("Requests/connection: %s\n", reqs_per_conn == 0 ? "dynamic (timer based)" : std::to_string(reqs_per_conn));
    return http_clients->start(std::move(duration), std::move(total_conn), std::move(reqs_per_conn)).then([http_clients, server] {
        return http_clients->invoke_on_all(&http_client::connect, ipv4_addr{server});
    }).then([http_clients] {
        started = steady_clock_type::now();
        return http_clients->invoke_on_all(&http_client::run);
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
    });
}

int main(int ac, char** av) {
    app_template app;
    app.add_options()
        ("server,s", bpo::value<std::string>()->default_value("192.168.66.100:10000"), "Server address")
        ("conn-first,C", bpo::value<unsigned>()->default_value(100), "total connections for first test")
        ("conn,c", bpo::value<unsigned>()->default_value(100), "total connections")
        ("reqs-first,R", bpo::value<unsigned>()->default_value(0), "reqs per connection for first test")
        ("reqs,r", bpo::value<unsigned>()->default_value(0), "reqs per connection")
        ("duration-first,D", bpo::value<unsigned>()->default_value(10), "duration of the test in seconds for first test")
        ("duration,d", bpo::value<unsigned>()->default_value(10), "duration of the test in seconds")
        ("url,u", bpo::value<std::string>()->default_value("/"), "http request url")
        ("host,h", bpo::value<std::string>()->default_value("10.28.1.13:10000"), "http request url")
        ("rttmode,r", bpo::value<unsigned>()->default_value(0), "test rtt if on")
        ("interval,i", bpo::value<unsigned>()->default_value(1000), "rtt test interval (count by request)")
        ("debugmode,g", bpo::value<unsigned>()->default_value(0), "debug mode");

    return app.run(ac, av, [&app] () -> future<int> {
        auto& config = app.configuration();
        auto server = config["server"].as<std::string>();
        auto _reqs_per_conn = config["reqs-first"].as<unsigned>();
        auto _total_conn= config["conn-first"].as<unsigned>();
        auto _duration = config["duration-first"].as<unsigned>();
        auto url = config["url"].as<string>();
        auto host = config["host"].as<string>();
        MY_HTTP_DEBUG = config["debugmode"].as<unsigned>();
        
        http_request = string("GET ") + url + " HTTP/1.1\r\nHost: " + host + "\r\n\r\n";
        http_debug(http_request.c_str());

        if (_total_conn % smp::count != 0) {
            print("Error: conn needs to be n * cpu_nr\n");
            return make_ready_future<int>(-1);
        }

        return test_once(server, _reqs_per_conn, _total_conn, _duration).then([&app] (int i) {
            print("Sleep for 2s.\n");
            using namespace std::chrono_literals;
            return seastar::sleep(2s).then([&app] {
                auto& config = app.configuration();
                auto server = config["server"].as<std::string>();
                auto reqs_per_conn = config["reqs"].as<unsigned>();
                auto total_conn= config["conn"].as<unsigned>();
                auto duration = config["duration"].as<unsigned>();
                unsigned rttmode = config["rttmode"].as<unsigned>();
                unsigned interval = config["interval"].as<unsigned>();
                auto url = config["url"].as<string>();


                if(rttmode) {
                    for(unsigned i = 0; i < smp::count; i++){
                        rtt[i] = new rtt_test(rtt_path + url + to_string(i), interval);
                        global_rtt_mark[i] = true;
                    }
                }

                if (total_conn % smp::count != 0) {
                    print("Error: conn needs to be n * cpu_nr\n");
                    return make_ready_future<int>(-1);
                }

                return test_once(server, reqs_per_conn, total_conn, duration).then_wrapped([] (auto &&f) {
                    for(unsigned i = 0; i < smp::count; i++) {
                        if(rtt[i]){
                            rtt[i]->average();

                            delete rtt[i];
                            rtt[i] = nullptr;
                        }
                    }
                    
                    return std::move(f);
                });
            });
        });
    });
}