#ifndef _ASYNC_FLOW_EVENT_HH
#define _ASYNC_FLOW_EVENT_HH

#include <cstdint>
#include <assert.h>

namespace netstar{

namespace internal{

using event_storage_type = uint32_t;

} // namespace internal

template<typename Enum>
class filtered_events;
template<typename Enum>
class generated_events;
template<typename Enum>
class registered_events;

template<typename Enum>
class filtered_events {
    friend class registered_events<Enum>;
    using est = internal::event_storage_type;
    est _filtered_events;
public:
    filtered_events(est fe)
        : _filtered_events(fe){
    }
public:
    template<Enum EvT> bool on_event() const {
        static_assert((static_cast<uint8_t>(EvT) < (sizeof(est)*8-1)),
                      "event_type too large.\n");
        est mask = 1 << static_cast<est>(EvT);
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

template<typename Enum>
class generated_events {
    friend class registered_events<Enum>;
    using est = internal::event_storage_type;
    est _generated_events;
public:
    generated_events() {
        _generated_events = 0;
    }

public:
    template<Enum EvT> void event_happen(){
        static_assert((static_cast<uint8_t>(EvT) < (sizeof(est)*8-1)),
                      "event_type too large.\n");
        est mask = 1 << static_cast<est>(EvT);
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

template<typename Enum>
class registered_events {
    using est = internal::event_storage_type;
    est _registered_events;
public:
    registered_events() {
        unregister_all_events();
    }
public:
    template<Enum EvT> void register_event() {
        static_assert((static_cast<uint8_t>(EvT) < (sizeof(est)*8-1)),
                      "event_type too large.\n");
        est mask = 1 << static_cast<est>(EvT);
        assert((_registered_events & mask) == 0);
        _registered_events |= mask;
    }

    template<Enum EvT> void unregister_event() {
        static_assert((static_cast<uint8_t>(EvT) < (sizeof(est)*8-1)),
                      "event_type too large.\n");
        est mask = ~(1 << static_cast<est>(EvT));
        _registered_events &= mask;
    }

    void unregister_all_events(){
        est mask = 1<< (static_cast<est>(sizeof(est)*8-1));
        _registered_events = mask;
    }

    filtered_events<Enum> filter(generated_events<Enum> ge){
        return filtered_events<Enum>{
            _registered_events&ge._generated_events};
    }
};

} // namespace netstar


#endif
