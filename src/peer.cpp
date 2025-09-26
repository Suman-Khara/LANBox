#include "peer.hpp"

void to_json(json& j, const Peer& p) {
    j = json{
        {"name", p.getName()},
        {"ip", p.getIP()},
        {"port", p.getPort()},
        {"last_seen", p.getLastSeen()},
        {"self", p.isSelf()}
    };
}

void from_json(const json& j, Peer& p) {
    p.setName(j.value("name", ""));
    p.setIP(j.value("ip", ""));
    p.setPort(j.value("port", 5000));
    p.setLastSeen(j.value("last_seen", 0L));
    p.setSelf(j.value("self", false));
}
