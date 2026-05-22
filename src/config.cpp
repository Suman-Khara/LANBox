#include "config.hpp"
#include <fstream>
#include <iostream>
#include <iomanip>
#include <ctime>
#include <sys/stat.h>
#include <sys/types.h>
#ifdef _WIN32
    #include <direct.h>
#endif
// Add this helper function at the top
static bool createDirectoryIfNotExists(const string& path) {
#ifdef _WIN32
    // Windows: extract directory from full path
    size_t pos = path.find_last_of("/\\");
    if (pos == string::npos) return true;
    
    string dir = path.substr(0, pos);
    struct _stat info;
    if (_stat(dir.c_str(), &info) != 0) {
        // Directory doesn't exist, create it
        return _mkdir(dir.c_str()) == 0;
    }
#else
    // Linux: extract directory from full path
    size_t pos = path.find_last_of('/');
    if (pos == string::npos) return true;
    
    string dir = path.substr(0, pos);
    struct stat info;
    if (stat(dir.c_str(), &info) != 0) {
        // Directory doesn't exist, create it (0755 permissions)
        return mkdir(dir.c_str(), 0755) == 0;
    }
#endif
    return true;  // Directory already exists
}

Config::Config(const string& path) : filepath(path) {
    // Ensure directory exists
    createDirectoryIfNotExists(filepath);
}

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

Peer* Config::findPeer(const string& nameOrIP) {
    for (auto& peer : peers) {
        // Check if matches name (case-insensitive)
        string peerName = peer.getName();
        transform(peerName.begin(), peerName.end(), peerName.begin(), ::tolower);
        string searchName = nameOrIP;
        transform(searchName.begin(), searchName.end(), searchName.begin(), ::tolower);
        
        if (peerName == searchName) {
            return &peer;
        }
        
        // Check if matches IP
        if (peer.getIP() == nameOrIP) {
            return &peer;
        }
    }
    return nullptr;  // Not found
}

bool Config::hasPeer(const string& nameOrIP) const {
    for (const auto& peer : peers) {
        string peerName = peer.getName();
        transform(peerName.begin(), peerName.end(), peerName.begin(), ::tolower);
        string searchName = nameOrIP;
        transform(searchName.begin(), searchName.end(), searchName.begin(), ::tolower);
        
        if (peerName == searchName || peer.getIP() == nameOrIP) {
            return true;
        }
    }
    return false;
}