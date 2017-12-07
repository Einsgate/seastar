#ifndef _ASYNC_FLOW_HH
#define _ASYNC_FLOW_HH

#include "net/packet.hh"

#include "core/shared_ptr.hh"
#include "core/future.hh"
#include "core/reactor.hh"
#include "core/timer.hh"
#include "core/queue.hh"

#include "netstar/af/async_flow_util.hh"

#include <deque>
#include <experimental/optional>
#include <unordered_map>
#include <chrono>
#include <array>

using namespace seastar;

namespace netstar {

#define ASYNC_FLOW_DEBUG

template <typename... Args>
void async_flow_debug(const char* fmt, Args&&... args) {
#ifdef ASYNC_FLOW_DEBUG
    print(fmt, std::forward<Args>(args)...);
#endif
}

template<typename Ppr>
class async_flow;
template<typename Ppr>
class af_initial_context;
template<typename Ppr>
class async_flow_manager;

namespace internal {

template<typename Ppr>
class async_flow_impl;

struct buffered_packet {
    net::packet pkt;
    bool is_send;

    buffered_packet(net::packet pkt_arg, bool is_send_arg)
        : pkt(std::move(pkt_arg))
        , is_send(is_send_arg){
    }
};

template<typename Ppr>
struct packet_context {
    using EventEnumType = typename Ppr::EventEnumType;
    net::packet pkt;
    filtered_events<EventEnumType> fe;
    bool is_send;

    packet_context(net::packet pkt_arg,
                   filtered_events<EventEnumType> fe_arg,
                   bool is_send_arg)
        : pkt(std::move(pkt_arg))
        , fe(fe_arg)
        , is_send(is_send_arg) {
    }
};

template<typename Ppr>
struct af_work_unit {
    using EventEnumType = typename Ppr::EventEnumType;
    using FlowKeyType = typename Ppr::FlowKeyType;

    Ppr ppr;
    std::experimental::optional<promise<>> async_loop_pr;
    registered_events<EventEnumType> send_events;
    registered_events<EventEnumType> recv_events;
    circular_buffer<buffered_packet> buffer_q;
    std::experimental::optional<FlowKeyType> flow_key;
    uint8_t direction;
    bool loop_started;
    bool ppr_close;
    std::experimental::optional<packet_context<Ppr>> cur_context;

    af_work_unit(bool is_client_arg,
                 uint8_t direction_arg)
        : ppr(is_client_arg)
        , direction(direction_arg)
        , loop_started(false)
        , ppr_close(false) {
        buffer_q.reserve(5);
    }
};

template<typename Ppr>
class async_flow_impl : public enable_lw_shared_from_this<async_flow_impl<Ppr>>{
    using EventEnumType = typename Ppr::EventEnumType;
    using FlowKeyType = typename Ppr::FlowKeyType;
    static constexpr bool packet_recv = true;
    friend class async_flow<Ppr>;

    async_flow_manager<Ppr>& _manager;
    af_work_unit<Ppr> _client;
    af_work_unit<Ppr> _server;
    unsigned _pkts_in_pipeline; // records number of the packets injected into the pipeline.
    bool _initial_context_destroyed;

private:
    // General helper utility function, useful for reducing the
    // boilerplates used in this class.

    af_work_unit<Ppr>& get_work_unit(bool is_client){
        return is_client ? _client : _server;
    }

    void close_ppr_and_remove_flow_key(af_work_unit<Ppr>& work_unit) {
        work_unit.ppr_close = true;
        if(work_unit.flow_key) {
            _manager.remove_mapping_on_flow_table(*(work_unit.flow_key));
            work_unit.flow_key = std::experimental::nullopt;
        }
    }

    filtered_events<EventEnumType> preprocess_packet(
            af_work_unit<Ppr>& working_unit, net::packet& pkt, bool is_send) {
        generated_events<EventEnumType> ge =
                is_send ?
                working_unit.ppr.handle_packet_send(pkt) :
                working_unit.ppr.handle_packet_recv(pkt);
        filtered_events<EventEnumType> fe =
                is_send ?
                working_unit.send_events.filter(ge) :
                working_unit.recv_events.filter(ge);
        return fe;
    }

    void internal_packet_forward(net::packet pkt, bool is_client, bool is_send) {
        if(is_send) {
            handle_packet_recv(std::move(pkt), !is_client);
        }
        else{
            send_packet_out(std::move(pkt), is_client);
            _pkts_in_pipeline -= 1;
        }
    }

    void action_after_packet_handle(af_work_unit<Ppr>& working_unit,
                                    net::packet pkt,
                                    bool is_client, bool is_send) {
        if(working_unit.loop_started) {
            if(working_unit.async_loop_pr) {
                async_flow_assert(working_unit.buffer_q.empty());
                auto fe = preprocess_packet(working_unit, pkt, is_send);
                if(fe.no_event()) {
                    internal_packet_forward(std::move(pkt), is_client, is_send);
                }
                else{
                    working_unit.cur_context.emplace(std::move(pkt), fe, is_send);
                    working_unit.async_loop_pr->set_value();
                    working_unit.async_loop_pr = {};
                }
            }
            else{
                working_unit.buffer_q.emplace_back(std::move(pkt), is_send);
            }
        }
        else{
            if(is_send) {
                working_unit.ppr.handle_packet_send(pkt);
                handle_packet_recv(std::move(pkt), !is_client);
            }
            else{
                working_unit.ppr.handle_packet_recv(pkt);
                send_packet_out(std::move(pkt), is_client);
                _pkts_in_pipeline -= 1;
            }
        }
    }

public:
    // Internal interfaces, exposed to async_flow and
    // async_flow manager.
    async_flow_impl(async_flow_manager<Ppr>& manager,
                    uint8_t client_direction,
                    FlowKeyType* client_flow_key)
        : _manager(manager)
        , _client(true, client_direction)
        , _server(false, manager.get_reverse_direction(client_direction))
        , _pkts_in_pipeline(0)
        , _initial_context_destroyed(false) {
        _client.flow_key = *client_flow_key;
    }

    ~async_flow_impl() {
        async_flow_assert(!_client.cur_context);
        async_flow_assert(!_server.cur_context);
        async_flow_assert(_pkts_in_pipeline == 0);
        async_flow_assert(_initial_context_destroyed);
    }

    void destroy_initial_context() {
        _initial_context_destroyed = true;
    }

    void handle_packet_send(net::packet pkt, uint8_t direction) {
        async_flow_debug("async_flow_impl: handle_packet_send is called\n");

        bool is_client = (direction == _client.direction);
        auto& working_unit = get_work_unit(is_client);

        if( _pkts_in_pipeline >= Ppr::async_flow_config::max_event_context_queue_size ||
             working_unit.ppr_close ||
             !_initial_context_destroyed) {
            // Unconditionally drop the packet.
            return;
        }

        _pkts_in_pipeline += 1;

        action_after_packet_handle(working_unit, std::move(pkt),
                                   is_client, true);
    }

    void handle_packet_recv(net::packet pkt, bool is_client){
        async_flow_debug("async_flow_impl: handle_packet_recv is called\n");

        auto& working_unit = get_work_unit(is_client);

        if(working_unit.ppr_close) {
            // Unconditionally drop the packet.
            return;
        }

        if(!working_unit.flow_key) {
            async_flow_assert(!is_client);
            FlowKeyType flow_key = working_unit.ppr.get_reverse_flow_key(pkt);
            _manager.add_new_mapping_to_flow_table(flow_key, this->shared_from_this());
        }

        action_after_packet_handle(working_unit, std::move(pkt),
                                   is_client, false);
    }

    void send_packet_out(net::packet pkt, bool is_client){
        auto& working_unit = get_work_unit(is_client);
        _manager.send(std::move(pkt), working_unit.direction);
        async_flow_debug("async_flow_impl: send packet out from direction %d.\n", working_unit.direction);
    }

    void destroy_event_context(bool is_client) {
        auto& working_unit = get_work_unit(is_client);
        working_unit.cur_context = {};
        _pkts_in_pipeline -= 1;
    }

    void forward_event_context(bool is_client) {
        auto& working_unit = get_work_unit(is_client);
        auto& context = working_unit.cur_context.value();
        if(context.pkt) {
            if(context.is_send){
                handle_packet_recv(std::move(context.pkt), !is_client);
            }
            else{
                send_packet_out(std::move(context.pkt), is_client);
                _pkts_in_pipeline -= 1;
            }
        }
        working_unit.cur_context = {};
    }

    future<> on_new_events(bool is_client) {
        auto& working_unit = get_work_unit(is_client);

        async_flow_assert(!working_unit.cur_context);
        async_flow_assert(!working_unit.async_loop_pr);

        if(working_unit.loop_started == false) {
            working_unit.loop_started = true;
        }
        while(!working_unit.buffer_q.empty()) {
            auto& next_pkt = working_unit.buffer_q.front();
            auto fe = preprocess_packet(working_unit,
                    next_pkt.pkt, next_pkt.is_send);
            if(fe.no_event()) {
                internal_packet_forward(std::move(next_pkt),
                        is_client, next_pkt.is_send);
                working_unit.buffer_q.pop_front();
            }
            else{
                working_unit.cur_context.emplace(std::move(next_pkt.pkt),
                                                 fe,
                                                 next_pkt.is_send);
                working_unit.buffer_q.pop_front();
                return make_ready_future<>();
            }
        }

        if(working_unit.ppr_close == true) {
            working_unit.cur_context.emplace(net::packet::make_null_packet(),
                                             filtered_events<EventEnumType>::make_close_event(),
                                             true);
            return make_ready_future<>();
        }
        else{
            working_unit.async_loop_pr = promise<>();
            return working_unit.async_loop_pr->get_future();
        }
    }

    void event_registration (bool is_client, bool is_send, EventEnumType ev) {
        auto& working_unit = is_client ? _client : _server;
        auto& events = is_send ? working_unit.send_events : working_unit.recv_events;
        events.register_event(ev);
    }

    void close_async_loop (bool is_client) {
        auto& working_unit = get_work_unit(is_client);

        async_flow_assert(!working_unit.cur_context);
        async_flow_assert(!working_unit.async_loop_pr);
        async_flow_assert(working_unit.loop_started == true);

        working_unit.loop_started = false;
        while(!working_unit.buffer_q.empty()) {
            auto& next_pkt = working_unit.buffer_q.front();
            if(next_pkt.is_send) {
                working_unit.ppr.handle_packet_send(next_pkt.pkt);
                handle_packet_recv(std::move(next_pkt.pkt), !is_client);
            }
            else{
                working_unit.ppr.handle_packet_recv(next_pkt.pkt);
                send_packet_out(std::move(next_pkt.pkt), is_client);
                _pkts_in_pipeline -= 1;
            }
            working_unit.buffer_q.pop_front();
        }
    }

    void ppr_passive_close(bool is_client){
        auto& working_unit = get_work_unit(is_client);
        close_ppr_and_remove_flow_key(working_unit);

        if(working_unit.loop_started && working_unit.async_loop_pr) {
            _pkts_in_pipeline += 1;
            working_unit.cur_context.emplace(net::packet::make_null_packet(),
                                             filtered_events<EventEnumType>::make_close_event(),
                                             true);
            working_unit.async_loop_pr->set_value();
            working_unit.async_loop_pr = {};
        }
    }
};

} // namespace internal

template<typename Ppr>
class async_flow{
    using impl_type = lw_shared_ptr<internal::async_flow_impl<Ppr>>;
    using EventEnumType = typename Ppr::EventEnumType;
    impl_type _impl;
public:
    explicit async_flow(impl_type impl)
        : _impl(std::move(impl)) {
    }
    ~async_flow(){
    }
    async_flow(const async_flow& other) = delete;
    async_flow(async_flow&& other) noexcept
        : _impl(std::move(other._impl)) {
    }
    async_flow& operator=(const async_flow& other) = delete;
    async_flow& operator=(async_flow&& other) {
        if(&other != this){
            this->~async_flow();
            new (this) async_flow(std::move(other));
        }
        return *this;
    }

    future<> on_client_side_events() {
        return _impl->on_new_events(true);
    }

    future<> on_server_side_events() {
        return _impl->on_new_events(false);
    }

    void register_client_events(af_send_recv sr, EventEnumType ev) {
        _impl->event_registration(true, static_cast<bool>(sr), ev);
    }

    void register_server_events(af_send_recv sr, EventEnumType ev) {
        _impl->event_registration(false, static_cast<bool>(sr), ev);
    }
};

template<typename Ppr>
class af_initial_context {
    using impl_type = lw_shared_ptr<internal::async_flow_impl<Ppr>>;
    friend class async_flow_manager<Ppr>;

    impl_type _impl_ptr;
    net::packet _pkt;
    uint8_t _direction;
    bool _extract_async_flow;
    int _move_construct_count;
private:
    explicit af_initial_context(net::packet pkt, uint8_t direction,
                                impl_type impl_ptr)
        : _impl_ptr(std::move(impl_ptr))
        , _pkt(std::move(pkt))
        , _direction(direction)
        , _extract_async_flow(false)
        , _move_construct_count(0)
        {
    }
public:
    af_initial_context(af_initial_context&& other) noexcept
        : _impl_ptr(std::move(other._impl_ptr))
        , _pkt(std::move(other._pkt))
        , _direction(other._direction)
        , _extract_async_flow(other._extract_async_flow)
        , _move_construct_count(other._move_construct_count) {
        _move_construct_count += 1;
    }
    af_initial_context& operator=(af_initial_context&& other) noexcept {
        if(&other != this) {
            this->~af_initial_context();
            new (this) af_initial_context(std::move(other));
        }
        return *this;
    }
    ~af_initial_context(){
        if(_impl_ptr) {
            async_flow_assert(_move_construct_count == 0);
            _impl_ptr->destroy_initial_context();
            _impl_ptr->handle_packet_send(std::move(_pkt), _direction);
        }
    }
    async_flow<Ppr> get_async_flow() {
        async_flow_assert(!_extract_async_flow);
        _extract_async_flow = true;
        return async_flow<Ppr>(_impl_ptr);
    }
};

template<typename Ppr>
class async_flow_manager {
    using FlowKeyType = typename Ppr::FlowKeyType;
    using HashFunc = typename Ppr::HashFunc;
    using impl_type = lw_shared_ptr<internal::async_flow_impl<Ppr>>;
    friend class internal::async_flow_impl<Ppr>;

    struct internal_io_direction {
        std::experimental::optional<subscription<net::packet, FlowKeyType*>> input_sub;
        stream<net::packet> output_stream;
        uint8_t reverse_direction;
        internal_io_direction()
            : reverse_direction(0) {
        }
    };
    struct queue_item {
        impl_type impl_ptr;
        net::packet pkt;
        uint8_t direction;

        queue_item(impl_type impl_ptr_arg,
                   net::packet pkt_arg,
                   uint8_t direction_arg)
            : impl_ptr(std::move(impl_ptr_arg))
            , pkt(std::move(pkt_arg))
            , direction(direction_arg) {
        }
    };


    std::unordered_map<FlowKeyType, lw_shared_ptr<internal::async_flow_impl<Ppr>>, HashFunc> _flow_table;
    std::array<internal_io_direction, Ppr::async_flow_config::max_directions> _directions;
    seastar::queue<queue_item> _new_ic_q{Ppr::async_flow_config::new_flow_queue_size};

public:
    class external_io_direction {
        std::experimental::optional<subscription<net::packet>> _receive_sub;
        stream<net::packet, FlowKeyType*> _send_stream;
        uint8_t _direction;
        bool _is_registered;
    public:
        external_io_direction(uint8_t direction)
            : _direction(direction)
            , _is_registered(false) {
        }
        void register_to_manager(async_flow_manager<Ppr>& manager,
                                 std::function<future<>(net::packet)> receive_fn,
                                 external_io_direction& reverse_io) {
            async_flow_assert(!_is_registered);
            _is_registered = true;
            _receive_sub.emplace(
                    manager.direction_registration(_direction, reverse_io.get_direction(),
                                                   _send_stream, std::move(receive_fn))
            );
        }
        uint8_t get_direction() {
            return _direction;
        }
        stream<net::packet, FlowKeyType*>& get_send_stream(){
            return _send_stream;
        }
    };
    future<> on_new_initial_context() {
        return _new_ic_q.not_empty();
    }
    af_initial_context<Ppr> get_initial_context() {
        async_flow_assert(!_new_ic_q.empty());
        auto qitem = _new_ic_q.pop();
        return af_initial_context<Ppr>(std::move(qitem.pkt), qitem.direction, std::move(qitem.impl_ptr));
    }

private:
    subscription<net::packet> direction_registration(uint8_t direction,
                                                     uint8_t reverse_direction,
                                                     stream<net::packet, FlowKeyType*>& istream,
                                                     std::function<future<>(net::packet)> fn) {
        async_flow_assert(direction < _directions.size());
        async_flow_assert(!_directions[direction].input_sub);

        _directions[direction].input_sub.emplace(
                istream.listen([this, direction](net::packet pkt, FlowKeyType* key) {
            async_flow_debug("Receive a new packet from direction %d\n", direction);
            auto afi = _flow_table.find(*key);
            if(afi == _flow_table.end()) {
                if(!_new_ic_q.full() &&
                   (_flow_table.size() <
                    Ppr::async_flow_config::max_flow_table_size) ){
                    auto impl_lw_ptr =
                            make_lw_shared<internal::async_flow_impl<Ppr>>(
                                (*this), direction, key
                            );
                    auto succeed = _flow_table.insert({*key, impl_lw_ptr}).second;
                    assert(succeed);
                    _new_ic_q.push(queue_item(std::move(impl_lw_ptr), std::move(pkt), direction));
                }
            }
            else {
                afi->second->handle_packet_send(std::move(pkt), direction);
            }

            return make_ready_future<>();
        }));
        auto sub = _directions[direction].output_stream.listen(std::move(fn));
        _directions[direction].reverse_direction = reverse_direction;
        return sub;
    }
    future<> send(net::packet pkt, uint8_t direction) {
        return _directions[direction].output_stream.produce(std::move(pkt));
    }
    uint8_t get_reverse_direction(uint8_t direction) {
        return _directions[direction].reverse_direction;
    }
    void add_new_mapping_to_flow_table(FlowKeyType& flow_key,
                                       lw_shared_ptr<internal::async_flow_impl<Ppr>> impl_lw_ptr){
        assert(_flow_table.insert({flow_key, impl_lw_ptr}).second);
    }
    void remove_mapping_on_flow_table(FlowKeyType& flow_key) {
        _flow_table.erase(flow_key);
    }
};

} // namespace netstar

#endif
