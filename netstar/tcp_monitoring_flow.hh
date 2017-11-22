#ifndef _TCP_MONITORING_FLOW_HH
#define _TCP_MONITORING_FLOW_HH

#include "net/packet.hh"

#include "core/shared_ptr.hh"
#include "core/future.hh"
#include "core/reactor.hh"
#include "core/timer.hh"
#include "core/queue.hh"

#include <deque>
#include <experimental/optional>
#include <unordered_map>
#include <chrono>

using namespace seastar;

namespace netstar{

class tcp_monitor;

enum class direction{
    INGRESS,
    EGRESS
};

struct directed_pkt{
    net::packet pkt;
    direction d;
};

namespace internal{

class tcp_monitor_impl;

enum class monitor_state{
    pre_tcp_sender_processing,

};

struct cur_pkt_ctx {
    net::packet pkt;
    direction d;
    cur_pkt_ctx(net::packet&& new_pkt, direction new_d)
        : pkt(std::move(new_pkt)), d(new_d){}
};

class tcp_monitor_impl {
    static constexpr unsigned max_receiveq_size = 5;
    circular_buffer<directed_pkt> _receiveq;
    bool _end;
    std::experimental::optional<promise<>> _new_pkt_promise;
    std::experimental::optional<cur_pkt_ctx> _pkt_ctx;
public:
    tcp_monitor_impl()
        : _end(false){
        _receiveq.reserve(max_receiveq_size);
    }
    void receive_pkt(net::packet pkt, direction dir){
        if(_receiveq.size()<=max_receiveq_size){
            _receiveq.push_back(directed_pkt{std::move(pkt), dir});
            /*if(_new_pkt_promise){
                _new_pkt_promise->set_value();
                _new_pkt_promise = {};
            }*/

            // start the actual processing in case that
            // the _pkt_ctx does not exist
            if(!_pkt_ctx){
                // Build the packet context.
                _pkt_ctx.emplace(std::move(_receiveq.front().pkt),
                                 _receiveq.front().d);
                _receiveq.pop_front();
                handle_cur_pkt_ctx().then([this]{
                    _pkt_ctx = {};
                    return make_ready_future<>();
                });
            }
        }
    }
    future<> handle_cur_pkt_ctx(){
        assert(_pkt_ctx);
        return make_ready_future<>();
    }
private:
    future<> on_new_packet(){
        assert(!_new_pkt_promise);
        if(_receiveq.size()>0){
            return make_ready_future<>();
        }
        _new_pkt_promise = promise<>();
        return _new_pkt_promise->get_future();
    }
    directed_pkt read_packet(){
        directed_pkt p(std::move(_receiveq.front()));
        _receiveq.pop_front();
        return p;
    }
    void set_end(){
        _end = true;
    }
    bool get_end(){
        return _end;
    }
    friend class netstar::tcp_monitor;
};

} // namespace internal

class tcp_monitor{
    lw_shared_ptr<internal::tcp_monitor_impl> _impl;
public:
    tcp_monitor(lw_shared_ptr<internal::tcp_monitor_impl> impl)
        : _impl(std::move(impl)) {
    }
    future<> on_new_packet(){
        return _impl->on_new_packet();
    }
    directed_pkt read_packet(){
        return _impl->read_packet();
    }
    bool is_impl_ended(){
        return _impl->get_end();
    }
};

} // namespace netstar

#endif
