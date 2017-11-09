#ifndef _ASYNC_FLOW_HH
#define _ASYNC_FLOW_HH

#include "net/packet.hh"

#include "core/shared_ptr.hh"
#include "core/future.hh"
#include "core/reactor.hh"

#include <deque>
#include <experimental/optional>
#include <unordered_map>

using namespace seastar;

namespace netstar{

// An exception raised by async_flow.
class asyn_flow_abort : public std::exception {
public:
    virtual const char* what() const noexcept override {
        return "abort";
    }
};

// Direction of the packet.
enum class direction{
    INGRESS,
    EGRESS
};

// Internal packet representation.
struct directed_packet{
    net::packet pkt;
    const direction dir;
};

// Base class for async flow.
class async_flow_base{
public:
    // Base receive interface.
    virtual void received(net::packet pkt, direction dir) = 0;
};

template<typename FKTrait>
class async_flow_manager{
    // The type of the corresponding flow key.
    using FlowKeyType = typename FKTrait::FlowKeyType;
    // The meta data passed in together with the packet.
    using PacketMetaData = typename FKTrait::PacketMetaData;
    // Possible state experienced by async_flow.
    enum class af_impl_state {
        ACTIVE,         // The flow is active.
        IDLE_TIMEOUT,   // The flow timeouts due to idleness.
        ABORT           // The flow is aborted, primarily by user
    };

public:
    class master_flow_impl;
    class slave_flow_impl;
    class forward_flow_impl;

    class master_flow_impl : public async_flow_base,
                             public enable_lw_shared_from_this<master_flow_impl>{
        // Size of the receive queue
        static constexpr unsigned max_receiveq_size = 5;
        // 5s timeout interval
        static constexpr unsigned timeout_interval = 5;

        // The async_flow_manager
        async_flow_manager& _manager;
        // The flow key
        FlowKeyType _flow_key;
        // The status of the master flow
        af_impl_state _status;
        // The receive queue.
        circular_buffer<directed_packet> _receiveq;
        // The promise to notify new packet arrival.
        std::experimental::optional<future<>> _new_pkt_promise;
        // The timer to timeout the master flow and the
        // packet counters.
        timer<steady_clock> _to;
        unsigned _pkt_counter;
        unsigned _previous_pkt_counter;
    public:
        void received(net::packet pkt, const direction dir) override{
            _pkt_counter += 1;
            if(_receiveq.size() < max_receiveq_size && _status == af_impl_state::ACTIVE) {
                _receiveq.push_back(directed_packet{std::move(pkt), dir});
                if(_new_pkt_promise){
                    _new_pkt_promise->set_value();
                    _new_pkt_promise = {};
                }
            }
        }
    public:
        future<> wait_for_new_pkt(){
            if(!_receiveq.empty() || _status == af_impl_state::IDLE_TIMEOUT){
                return make_ready_future<>();
            }
            _new_pkt_promise = promise<>();
            return _new_pkt_promise->get_future();
        }
        directed_packet get_pkt(){
            if(_status == af_impl_state::IDLE_TIMEOUT){
                return directed_packet{net::packet::make_null_packet(), direction::INGRESS};
            }
            auto pkt = _receiveq.front();
            _receiveq.pop_front();
            return pkt;
        }
        future<> send_pkt(directed_packet pkt){
            return _manager.send(std::move(pkt));
        }
        void abort(){
            assert(_status == af_impl_state::ACTIVE);
            while(!_receiveq.empty()){
                _receiveq.pop_front();
            }
            if(_new_pkt_promise){
                _new_pkt_promise->set_exception(asyn_flow_abort());
                _new_pkt_promise = {};
            }
            _status = af_impl_state::ABORT;
        }
        void timeout(){
            if(_previous_pkt_counter == _pkt_counter){
                _status == af_impl_state::IDLE_TIMEOUT;
                if(_new_pkt_promise){
                    _new_pkt_promise->set_value();
                    _new_pkt_promise = {};
                }
            }
            _previous_pkt_counter = _pkt_counter;
        }
    };

    /*class async_flow_base;
    class slave_flow_impl;
    class master_flow_impl;

    class async_flow;
    class async_flow_impl;


    class slave_flow_impl : async_flow_base {
        static constexpr unsigned max_forwrad_num = 5;
        unsigned _master_coreid;
        unsigned _slave_coreid;
        FlowKeyType _fkey;
        master_flow_impl* _master;
        unsigned _forward_num;
    public:
        virtual void received(net::packet pkt) override {
            if(_master_coreid == _slave_coreid){
                _master->received(std::move(pkt));
            }
            else{
                if(_forward_num < max_forwrad_num){
                    _forward_num += 1;
                    smp::submit_to(_master_coreid, [this, pkt=std::move(pkt)] () mutable {
                        _master->received(std::move(pkt.free_on_cpu(_slave_coreid)));
                    }).then([this]{
                        _forward_num -= 1;
                    });
                }
            }
        }
    };

    // For FKTrait, I actually mean FlowKeyTrait, :).
    class async_flow_impl :  public enable_lw_shared_from_this<async_flow_impl>{
        // Size of the receive queue
        static constexpr unsigned max_receiveq_size = 5;
        // 5s timeout interval
        static constexpr unsigned timeout_interval = 5;

        circular_buffer<net::packet> _receiveq;
        std::function<future<>(net::packet)> _sendf;
        std::experimental::optional<future<>> _new_pkt_promise;
        asyn_flow_impl_state _status;
        unsigned _pkt_counter;
        timer<steady_clock> _to;
        unsigned _previous_pkt_counter;
    public:
        void received(net::packet pkt){
            // We can only receive packets when we are not in IDLE_TIMEOUT.

            _pkt_counter += 1;
            // The packet is silently dropped if
            // 1. The size of the queue is larger than max_receiveq_size
            // 2. The state is in abort, received packets is no longer
            //    cared by the user.
            if(_receiveq.size()<=max_receiveq_size && _status == af_impl_state::ACTIVE) {
                _receiveq.push_back(std::move(pkt));
                if(_new_pkt_promise){
                    _new_pkt_promise->set_value();
                    _new_pkt_promise = {};
                }
            }
        }
        future<> wait_for_new_pkt(){
            if(!_receiveq.empty() || _status == af_impl_state::IDLE_TIMEOUT){
                return make_ready_future<>();
            }
            _new_pkt_promise = promise<>();
            return _new_pkt_promise->get_future();
        }
        net::packet get_pkt(){
            if(_status == af_impl_state::IDLE_TIMEOUT){
                return net::packet::make_null_packet();
            }
            auto pkt = _receiveq.front();
            _receiveq.pop_front();
            return pkt;
        }
        future<> send_pkt(net::packet pkt){
            return _sendf(pkt);
        }
        void abort(){
            assert(_status == af_impl_state::ACTIVE);
            _receiveq.clear();
            if(_new_pkt_promise){
                _new_pkt_promise->set_exception(asyn_flow_abort());
                _new_pkt_promise = {};
            }
            _status = af_impl_state::ABORT;
        }
        void timeout(){
            if(_previous_pkt_counter == _pkt_counter){
                _status == af_impl_state::IDLE_TIMEOUT;
                if(_new_pkt_promise){
                    _new_pkt_promise->set_value();
                    _new_pkt_promise = {};
                }
            }

            _previous_pkt_counter = _pkt_counter;
        }
    };
public:
    class async_flow{
        lw_shared_ptr<async_flow_impl> _impl;

    };*/
};





}

#endif
