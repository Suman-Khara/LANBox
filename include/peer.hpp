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
    bool self;
    string public_key;
    string fingerprint;

public:
    Peer() : name(""), ip(""), port(5000), last_seen(0), self(false), 
             public_key(""), fingerprint("") {}
    
    Peer(const string& n, const string& i, int p, long t, bool s = false)
        : name(n), ip(i), port(p), last_seen(t), self(s), 
          public_key(""), fingerprint("") {}


    string getName() const { return name; }
    string getIP() const { return ip; }
    int getPort() const { return port; }
    long getLastSeen() const { return last_seen; }
    bool isSelf() const { return self; }

    string getPublicKey() const { return public_key; }
    string getFingerprint() const { return fingerprint; }
    bool hasPublicKey() const { return !public_key.empty(); }


    void setName(const string& n) { name = n; }
    void setIP(const string& i) { ip = i; }
    void setPort(int p) { port = p; }
    void setLastSeen(long t) { last_seen = t; }
    void setSelf(bool s) { self = s; }

    void setPublicKey(const string& key) { public_key = key; }
    void setFingerprint(const string& fp) { fingerprint = fp; }
};

void to_json(json& j, const Peer& p);
void from_json(const json& j, Peer& p);