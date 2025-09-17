#pragma once
#include <string>
#include <vector>
#include "peer.hpp"
#include "json.hpp"

using json = nlohmann::json;
using namespace std;

class Config {
private:
    string filepath;
    vector<Peer> peers;

public:
    Config(const string& path = "../data/lanbox.json");

    bool load();
    bool save() const;

    void addOrUpdatePeer(const Peer& p);
    void removePeer(const string& ip);
    void removeStalePeers(long now, int timeout_sec);
    vector<Peer> getPeers() const;

    void printPeers() const;
};
