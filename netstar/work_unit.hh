#ifndef _WORK_UNIT_HH
#define _WORK_UNIT_HH

#include "core/future.hh"
#include "port.hh"

namespace netstar{

class work_unit{
    std::vector<port*> _all_ports;
protected:
    std::vector<port*>& all_ports(){
        return std::ref(_all_ports);
    }
    virtual future<> receive_from_port(uint16_t port_id, net::packet pkt) = 0;
    virtual future<> receive_forwarded(unsigned from_core, net::packet pkt) = 0;
public:
    void add_port(per_core_objs<port>& ports){
        auto local_port = ports.local_obj();
        _all_ports.push_back(local_port);
        auto port_id = _all_ports.size()-1;

        (*local_port).receive([this, port_id](net::packet pkt){
            return receive_from_port(port_id, std::move(pkt));
        });
    }
    inline void send_from_port(uint16_t port_id, net::packet pkt){
        assert(port_id<_all_ports.size());
        (*_all_ports[port_id]).send(std::move(pkt));
    }

    /*template<typename T>
    inline void forward_to(per_core_objs<T>* work_units){
        static_assert(std::is_base_of<work_unit, B>::value)
    }*/
};

} // namespace netstar

#endif
