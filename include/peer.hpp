#pragma once
#include <string>
#include "json.hpp"

using json = nlohmann::json;
using namespace std;

class Peer {
private:
    string name;
    string ip;
    int port;
    long last_seen;

public:
    Peer() : name(""), ip(""), port(5000), last_seen(0) {}
    Peer(const string& n, const string& i, int p, long t)
        : name(n), ip(i), port(p), last_seen(t) {}

    string getName() const { return name; }
    string getIP() const { return ip; }
    int getPort() const { return port; }
    long getLastSeen() const { return last_seen; }

    void setName(const string& n) { name = n; }
    void setIP(const string& i) { ip = i; }
    void setPort(int p) { port = p; }
    void setLastSeen(long t) { last_seen = t; }
};

void to_json(json& j, const Peer& p);
void from_json(const json& j, Peer& p);