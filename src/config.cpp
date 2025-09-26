#include "config.hpp"
#include <fstream>
#include <iostream>
#include <iomanip>
#include <ctime>

Config::Config(const string& path) : filepath(path) {}

bool Config::load() {
    ifstream file(filepath);
    if (!file.is_open()) {
        cerr << "Config file not found. Starting with empty config.\n";
        return false;
    }

    json j;
    file >> j;

    peers.clear();
    if (j.contains("peers")) {
        for (const auto& item : j["peers"]) {
            peers.push_back(item.get<Peer>());
        }
    }

    return true;
}

bool Config::save() const {
    json j;
    j["peers"] = peers;

    ofstream file(filepath);
    if (!file.is_open()) {
        cerr << "Failed to save config file.\n";
        return false;
    }

    file << j.dump(4); // pretty print with 4 spaces
    return true;
}

void Config::addOrUpdatePeer(const Peer& p) {
    for (auto& existing : peers) {
        if (existing.getIP() == p.getIP()) {
            existing.setName(p.getName());
            existing.setPort(p.getPort());
            existing.setLastSeen(p.getLastSeen());
            return; // updated
        }
    }
    peers.push_back(p); // new
}

void Config::removePeer(const string& ip) {
    peers.erase(
        remove_if(peers.begin(), peers.end(),
                  [&](const Peer& peer) { return peer.getIP() == ip; }),
        peers.end()
    );
}

void Config::removeStalePeers(long now, int timeout_sec) {
    peers.erase(
        remove_if(peers.begin(), peers.end(),
                  [&](const Peer& peer) {
                      return (now - peer.getLastSeen()) > timeout_sec;
                  }),
        peers.end()
    );
}

vector<Peer> Config::getPeers() const {
    return peers;
}

void Config::printPeers() const {
    cout << "Peers:\n";
    for (const auto& p : peers) {
        // Convert last_seen to readable datetime
        time_t ts = static_cast<time_t>(p.getLastSeen());
        tm* tm_info = localtime(&ts);

        cout << " - " << p.getName()
             << " (" << p.getIP() << ":" << p.getPort() << ")";
        
        if (p.isSelf()) {
            cout << " (self)";
        }

        if (tm_info) {
            cout << " last seen: " << put_time(tm_info, "%Y-%m-%d %H:%M:%S");
        } else {
            cout << " last seen: (invalid time)";
        }

        cout << "\n";
    }
}
