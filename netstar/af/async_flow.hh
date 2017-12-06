#ifndef _ASYNC_FLOW_HH
#define _ASYNC_FLOW_HH

#include "net/packet.hh"

#include "core/shared_ptr.hh"
#include "core/future.hh"
#include "core/reactor.hh"
#include "core/timer.hh"
#include "core/queue.hh"

#include "netstar/af/async_flow_event.hh"

#include <deque>
#include <experimental/optional>
#include <unordered_map>
#include <chrono>
#include <array>

using namespace seastar;

namespace netstar {

template<typename Ppr>
class af_ev_context;
template<typename Ppr>
class async_flow;
template<typename Ppr>
class af_initial_context;
template<typename Ppr>
class async_flow_manager;

#define ENABLE_ASSERTION
#define ASYNC_FLOW_DEBUG
// #define MEASURE_INITIAL_CONTEXT_MOVE

void async_flow_assert(bool boolean_expr) {
#ifdef ENABLE_ASSERTION
    assert(boolean_expr);
#endif
}

template <typename... Args>
void async_flow_debug(const char* fmt, Args&&... args) {
#ifdef ASYNC_FLOW_DEBUG
    print(fmt, std::forward<Args>(args)...);
#endif
}

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
struct af_work_unit {
    using EventEnumType = typename Ppr::EventEnumType;
    using FlowKeyType = typename Ppr::FlowKeyType;

    Ppr ppr;
    std::experimental::optional<promise<af_ev_context<Ppr>>> async_loop_pr;
    registered_events<EventEnumType> send_events;
    registered_events<EventEnumType> recv_events;
    circular_buffer<buffered_packet> buffer_q;
    std::experimental::optional<FlowKeyType> flow_key;
    uint8_t direction;
    bool loop_started;
    bool loop_has_context;
    bool ppr_close;

    af_work_unit(bool is_client_arg,
                 uint8_t direction_arg)
        : ppr(is_client_arg)
        , direction(direction_arg)
        , loop_started(false)
        , loop_has_context(false)
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
            handle_packet_recv(std::move(pkt), ~is_client);
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
                assert(working_unit.buffer_q.empty());
                auto fe = preprocess_packet(working_unit, pkt, is_send);
                if(fe.no_event()) {
                    internal_packet_forward(std::move(pkt), is_client, is_send);
                }
                else{
                    working_unit.loop_has_context = true;
                    working_unit.async_loop_pr->set_value(af_ev_context<Ppr>{
                        std::move(pkt), fe,
                        is_client, is_send, this
                    });
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
                handle_packet_recv(std::move(pkt), ~is_client);
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
        , _initial_context_destroyed(false){
        _client.flow_key = *client_flow_key;
    }

    ~async_flow_impl() {
        assert(_client.loop_has_context == false);
        assert(_server.loop_has_context == false);
        assert(_pkts_in_pipeline == 0);
        // assert(_initial_context_destroyed);
    }

    void destroy_initial_context() {
        _initial_context_destroyed = true;
    }

    void handle_packet_send(net::packet pkt, uint8_t direction) {
        bool is_client = (direction == _client.direction);
        auto& working_unit = get_work_unit(is_client);

        if( (_pkts_in_pipeline >=
             Ppr::async_flow_config::max_event_context_queue_size) ||
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
        auto& working_unit = get_work_unit(is_client);

        if(working_unit.ppr_close) {
            // Unconditionally drop the packet.
            return;
        }

        if(!working_unit.flow_key) {
            assert(!is_client);
            FlowKeyType flow_key = working_unit.ppr.get_reverse_flow_key(pkt);
            _manager.add_new_mapping_to_flow_table(flow_key, this->shared_from_this());
        }

        action_after_packet_handle(working_unit, std::move(pkt),
                                   is_client, false);
    }

    void send_packet_out(net::packet pkt, bool is_client){
        auto& working_unit = get_work_unit(is_client);
        _manager.send(std::move(pkt), working_unit.direction);
    }

    void destroy_event_context(af_ev_context<Ppr> context) {
        assert(context._impl == this);
        auto& working_unit = get_work_unit(context.is_client());
        working_unit.loop_has_context = false;
        _pkts_in_pipeline -= 1;
    }

    void forward_event_context(af_ev_context<Ppr> context) {
        assert(context._impl == this && context._pkt);
        bool is_client = context.is_client();
        auto& working_unit = get_work_unit(is_client);
        working_unit.loop_has_conetxt = false;
        if(context.is_send()){
            handle_packet_recv(std::move(context._pkt), ~context.is_client());
        }
        else{
            send_packet_out(std::move(context._pkt), context.is_client());
            _pkts_in_pipeline -= 1;
        }
    }

    future<af_ev_context<Ppr>> on_new_events(bool is_client) {
        auto& working_unit = get_work_unit(is_client);
        assert(working_unit.loop_has_context == false);
        assert(!working_unit.async_loop_pr);

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
                working_unit.loop_has_context = true;
                auto future = make_ready_future<af_ev_context<Ppr>>(
                    std::move(next_pkt.pkt), fe,
                    is_client, next_pkt.is_send, this
                );
                working_unit.buffer_q.pop_front();
                return future;
            }
        }

        if(working_unit.ppr_close == true) {
            working_unit.loop_has_context = true;
            return make_ready_future<af_ev_context<Ppr>>({
                net::packet::make_null_packet(),
                filtered_events<EventEnumType>::make_close_event(),
                is_client,
                true, this
            });
        }
        else{
            working_unit.async_loop_pr = promise<af_ev_context<Ppr>>();
            return working_unit.async_loop_pr->get_future();
        }
    }

    template<EventEnumType EvT> void event_registration (bool is_client, bool is_send) {
        auto& working_unit = is_client ? _client : _server;
        auto& events = is_send ? working_unit.send_events : working_unit.recv_events;
        events.register_event<EvT>(1);
    }

    void close_async_loop (bool is_client) {
        auto& working_unit = get_work_unit(is_client);

        assert(working_unit.loop_has_context == false);
        assert(!working_unit.async_loop_pr);
        assert(working_unit.loop_started == true);

        working_unit.loop_started = false;
        while(!working_unit.buffer_q.empty()) {
            auto& next_pkt = working_unit.buffer_q.front();
            if(next_pkt.is_send) {
                working_unit.ppr.handle_packet_send(next_pkt.pkt);
                handle_packet_recv(std::move(next_pkt.pkt), ~is_client);
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
            working_unit.loop_has_context = true;
            _pkts_in_pipeline += 1;
            working_unit.async_loop_pr->set_value(
                af_ev_context<Ppr>({
                      net::packet::make_null_packet(),
                      filtered_events<EventEnumType>::make_close_event(),
                      is_client,
                      true, this
                })
            );
            working_unit.async_loop_pr = {};
        }
    }
};

} // namespace internal

template<typename Ppr>
class af_ev_context{
    using EventEnumType = typename Ppr::EventEnumType;

    net::packet _pkt;
    filtered_events<EventEnumType> _fe;
    bool _is_client;
    bool _is_send;
    internal::async_flow_impl<Ppr>* _impl;

    friend class internal::async_flow_impl<Ppr>;

public:

    // Internal constructor used by async_flow_impl
    af_ev_context(net::packet pkt,
                  filtered_events<EventEnumType> fe,
                  bool is_client,
                  bool is_send,
                  internal::async_flow_impl<Ppr>* impl)
        : _pkt(std::move(pkt))
        , _fe(fe)
        , _is_client(is_client)
        , _is_send(is_send)
        , _impl(impl){
    }

    // Public constructor, uesful for temporarily storing
    // af_ev_context object.
    af_ev_context()
        : _pkt(net::packet::make_null_packet())
        , _fe(0)
        , _is_client(false)
        , _is_send(false)
        , _impl(nullptr) {
    }

    af_ev_context(const af_ev_context& other) = delete;
    af_ev_context(af_ev_context&& other) noexcept
        : _pkt(std::move(other._pkt))
        , _fe(other._fe)
        , _is_client(other._is_client)
        , _is_send(other._is_send)
        , _impl(other._impl) {
        other._impl = nullptr;
    }
    af_ev_context& operator=(const af_ev_context& other) = delete;
    af_ev_context& operator=(af_ev_context&& other) noexcept {
        if(&other != this){
            this->~af_ev_context();
            new (this) af_ev_context(std::move(other));
        }
        return *this;
    }

    const filtered_events<EventEnumType>& events() {
        return _fe;
    }
    bool is_client() {
        return _is_client;
    }
    bool is_send() {
        return _is_send;
    }
    bool is_null_pkt() {
        return _pkt;
    }
};

template<typename Ppr>
class async_flow{
    using impl_type = lw_shared_ptr<internal::async_flow_impl<Ppr>>;
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

    future<af_ev_context<Ppr>> on_client_side_events() {
        return _impl->on_new_events(true);
    }
    future<af_ev_context<Ppr>> on_server_side_events() {
        return _impl->on_new_events(true);
    }
};

template<typename Ppr>
class af_initial_context {
    using impl_type = lw_shared_ptr<internal::async_flow_impl<Ppr>>;

    impl_type _impl_ptr;
    net::packet _pkt;
    uint8_t _direction;
    bool _is_valid;
    bool _extract_async_flow;
    int _move_construct_count;

public:
    explicit af_initial_context(net::packet pkt, uint8_t direction,
                                impl_type impl_ptr)
        : _impl_ptr(std::move(impl_ptr))
        , _pkt(std::move(pkt))
        , _direction(direction)
        , _is_valid(true)
        , _extract_async_flow(false)
#ifdef MEASURE_INITIAL_CONTEXT_MOVE
        , _move_construct_count(0)
#else
        , _move_construct_count(4)
#endif
        {
    }
    af_initial_context(af_initial_context&& other) noexcept
        : _impl_ptr(std::move(other._impl_ptr))
        , _pkt(std::move(other._pkt))
        , _direction(other._direction)
        , _is_valid(other._is_valid)
        , _extract_async_flow(other._extract_async_flow)
        , _move_construct_count(other._move_construct_count) {
        other._is_valid = false;
#ifdef MEASURE_INITIAL_CONTEXT_MOVE
        _move_construct_count += 1;
#else
        assert(_move_construct_count > 0);
        _move_construct_count -= 1;
#endif
    }
    af_initial_context& operator=(af_initial_context&& other) noexcept {
        if(&other != this) {
            this->~af_initial_context();
            new (this) af_initial_context(std::move(other));
        }
        return *this;
    }
    ~af_initial_context(){
        if(_is_valid) {
#ifdef MEASURE_INITIAL_CONTEXT_MOVE
            async_flow_debug("af_initial_context is move-constructed %d "
                             "times.\n",
                             _move_construct_count);
#else
            assert(_move_construct_count == 0);
#endif
            _impl_ptr->destroy_initial_context();
            // _impl_ptr->destroy_initial_context();
            // _impl_ptr->handle_packet_send(std::move(_pkt), _direction);
        }
    }
    async_flow<Ppr> get_async_flow() {
        assert(!_extract_async_flow);
        _extract_async_flow = true;
        return async_flow<Ppr>(_impl_ptr);
    }
    void check_impl(){
        assert(_impl_ptr);
        async_flow_debug("check_impl succeeds\n");
    }
};

template<typename Ppr>
class async_flow_manager {
    using FlowKeyType = typename Ppr::FlowKeyType;
    using HashFunc = typename Ppr::HashFunc;
    using impl_type = lw_shared_ptr<internal::async_flow_impl<Ppr>>;

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

        queue_item(queue_item&& other) noexcept
            : impl_ptr(std::move(other.impl_ptr))
            , pkt(std::move(other.pkt))
            , direction(other.direction) {
        }

        queue_item& operator=(queue_item&& other) noexcept {
            if(this != &other) {
                this->~queue_item();
                new (this) queue_item(std::move(other));
            }
            return *this;
        }

        queue_item(const queue_item& other) = delete;
        queue_item& operator=(const queue_item& other) = delete;
    };


    std::unordered_map<FlowKeyType, lw_shared_ptr<internal::async_flow_impl<Ppr>>, HashFunc> _flow_table;
    std::array<internal_io_direction, Ppr::async_flow_config::max_directions> _directions;
    seastar::queue<queue_item> _new_flow_q{Ppr::async_flow_config::new_flow_queue_size};
    friend class internal::async_flow_impl<Ppr>;
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
            assert(!_is_registered);
            _is_registered = true;
            manager.direction_registration(_direction, reverse_io.get_direction(),
                                           _send_stream, std::move(receive_fn));
        }
        uint8_t get_direction() {
            return _direction;
        }
        stream<net::packet, FlowKeyType*>& get_send_stream(){
            return _send_stream;
        }
    };

    subscription<net::packet> direction_registration(uint8_t direction,
                                                     uint8_t reverse_direction,
                                                     stream<net::packet, FlowKeyType*>& istream,
                                                     std::function<future<>(net::packet)> fn) {
        assert(direction < _directions.size());
        assert(!_directions[direction].input_sub);

        _directions[direction].input_sub.emplace(
                istream.listen([this, direction](net::packet pkt, FlowKeyType* key) {
            async_flow_debug("Receive a new packet from direction %d\n", direction);
            auto afi = _flow_table.find(*key);
            if(afi == _flow_table.end()) {
                if(!_new_flow_q.full() &&
                   (_flow_table.size() <
                    Ppr::async_flow_config::max_flow_table_size) ){
                    auto impl_lw_ptr =
                            make_lw_shared<internal::async_flow_impl<Ppr>>(
                                (*this), direction, key
                            );
                    _flow_table.insert({*key, impl_lw_ptr});
                    assert(impl_lw_ptr);
                    _new_flow_q.push(queue_item(std::move(impl_lw_ptr), std::move(pkt), direction));
                    assert(!impl_lw_ptr);
                }
            }
            else {
                afi->second->handle_packet_send(std::move(pkt), direction);
            }

            return make_ready_future<>();
        }));
        auto sub = _directions[direction].output_stream.listen(std::move(fn));
        return sub;
    }

    future<af_initial_context<Ppr>> on_new_flow() {
        return _new_flow_q.not_empty().then([this]{
           auto qitem = _new_flow_q.pop();
           assert(qitem.impl_ptr);
           return make_ready_future<af_initial_context<Ppr>>(
               af_initial_context<Ppr>(
                   std::move(qitem.pkt),
                   qitem.direction,
                   std::move(qitem.impl_ptr)
               )
           );
        });
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
