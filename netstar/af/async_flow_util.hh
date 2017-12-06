#ifndef _ASYNC_FLOW_UTIL_HH
#define _ASYNC_FLOW_UTIL_HH

#include <cstdint>
#include <assert.h>

namespace netstar{

template<typename Enum>
class generated_events;

#define ENABLE_AF_ASSERTION

#ifndef ENABLE_AF_ASSERTION
#define async_flow_assert(condition) ((void)0)
#else
#define async_flow_assert(condition) assert(condition)
#endif

enum class af_side : bool {
    client=true,
    server=false
};

enum class af_send_recv : bool {
    send=true,
    recv=false
};

namespace internal {

using event_storage_type = uint16_t;

template<typename Enum>
class registered_events;
template<typename Enum>
class filtered_events;

template<typename Enum>
class registered_events {
    using est = event_storage_type;
    est _registered_events;
public:
    registered_events() {
        unregister_all_events();
    }
public:
    void register_event(Enum ev) {
        async_flow_assert(static_cast<uint8_t>(ev) < (sizeof(est)*8-1));
        est mask = 1 << static_cast<est>(ev);
        _registered_events |= mask;
    }

    void unregister_event(Enum ev) {
        async_flow_assert(static_cast<uint8_t>(ev) < (sizeof(est)*8-1));
        est mask = ~(1 << static_cast<est>(ev));
        _registered_events &= mask;
    }

    void unregister_all_events(){
        est mask = 1<< (static_cast<est>(sizeof(est)*8-1));
        _registered_events = mask;
    }

    filtered_events<Enum> filter(generated_events<Enum> ge){
        return filtered_events<Enum>(_registered_events & ge._generated_events);
    }
};

template<typename Enum>
class filtered_events {
    friend class internal::registered_events<Enum>;
    using est = internal::event_storage_type;
    est _filtered_events;
public:
    filtered_events(est fe)
        : _filtered_events(fe){
    }
public:
    bool on_event(Enum ev) const {
        async_flow_assert(static_cast<uint8_t>(ev) < (sizeof(est)*8-1));
        est mask = 1 << static_cast<est>(ev);
        return (_filtered_events&mask) != 0;
    }

    bool on_close_event() const {
        est mask = 1<< (static_cast<est>(sizeof(est)*8-1));
        return (_filtered_events&mask) != 0;
    }

    bool no_event() const {
        return _filtered_events == 0;
    }

    static filtered_events make_close_event() {
        est mask = 1<< (static_cast<est>(sizeof(est)*8-1));
        return filtered_events(mask);
    }
};

} // namespace internal

template<typename Enum>
class generated_events {
    friend class internal::registered_events<Enum>;
    using est = internal::event_storage_type;
    est _generated_events;
public:
    generated_events() {
        _generated_events = 0;
    }

public:
    void event_happen(Enum ev){
        async_flow_assert(static_cast<uint8_t>(ev) < (sizeof(est)*8-1));
        est mask = 1 << static_cast<est>(ev);
        _generated_events |= mask;
    }

    void close_event_happen(){
        est mask = 1<< (static_cast<est>(sizeof(est)*8-1));
        _generated_events |= mask;
    }

    void clear(){
        _generated_events = 0;
    }
};

} // namespace netstar

#endif
