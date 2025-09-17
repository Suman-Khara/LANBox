#include "discovery.hpp"
#include <bits/stdc++.h>
#include "json.hpp"
#include <chrono>

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #define _HAS_STD_BYTE 0  // prevent conflict with std::byte
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
    typedef SOCKET SocketType;
#else
    #include <sys/socket.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    typedef int SocketType;
    #define INVALID_SOCKET -1
    #define SOCKET_ERROR   -1
    inline void closesocket(SocketType s) { close(s); }
#endif

using namespace std;
using namespace chrono;
using json = nlohmann::json;

extern bool initSockets();
extern void cleanupSockets();

string getHostName() {
    char hostname[256];
#ifdef _WIN32
    DWORD size = sizeof(hostname);
    if (GetComputerNameA(hostname, &size)) {
        return string(hostname);
    }
#else
    if (gethostname(hostname, sizeof(hostname)) == 0) {
        return string(hostname);
    }
#endif
    return "UnknownDevice";
}

namespace {

    void senderLoop(Config& cfg, int port, int interval_sec) {
        if (!initSockets()) {
            cerr << "Socket init failed (sender)\n";
            return;
        }

        SocketType sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock == INVALID_SOCKET) {
            cerr << "Failed to create UDP socket\n";
            cleanupSockets();
            return;
        }

        int broadcastEnable = 1;
        setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (char*)&broadcastEnable, sizeof(broadcastEnable));

        sockaddr_in broadcastAddr{};
        broadcastAddr.sin_family = AF_INET;
        broadcastAddr.sin_port = htons(port);
        broadcastAddr.sin_addr.s_addr = htonl(INADDR_BROADCAST);

        while (Discovery::isRunning()) {
            json j = {
                {"type", "hello"},
                {"name", getHostName()},
                {"port", 5000},
                {"last_seen", duration_cast<seconds>(system_clock::now().time_since_epoch()).count()}
            };

            string msg = j.dump();
            sendto(sock, msg.c_str(), msg.size(), 0, (sockaddr*)&broadcastAddr, sizeof(broadcastAddr));

            this_thread::sleep_for(seconds(interval_sec));
        }

        closesocket(sock);
        cleanupSockets();
    }

    void listenerLoop(Config& cfg, int port) {
        if (!initSockets()) {
            cerr << "Socket init failed (listener)\n";
            return;
        }

        SocketType sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock == INVALID_SOCKET) {
            cerr << "Failed to create UDP socket\n";
            cleanupSockets();
            return;
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_ANY);

        if (bind(sock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
            cerr << "Bind failed\n";
            closesocket(sock);
            cleanupSockets();
            return;
        }

        char buffer[2048];
        while (Discovery::isRunning()) {
            sockaddr_in senderAddr{};
            socklen_t senderLen = sizeof(senderAddr);
            int bytes = recvfrom(sock, buffer, sizeof(buffer), 0, (sockaddr*)&senderAddr, &senderLen);

            if (bytes > 0) {
                buffer[bytes] = '\0';
                try {
                    json j = json::parse(buffer);
                    if (j["type"] == "hello") {
                        Peer p;
                        from_json(j, p);

                        char senderIP[INET_ADDRSTRLEN];
                        inet_ntop(AF_INET, &(senderAddr.sin_addr), senderIP, INET_ADDRSTRLEN);
                        p.setIP(senderIP);

                        cfg.addOrUpdatePeer(p);
                    }
                } catch (const exception& e) {
                    cerr << "JSON parse error: " << e.what() << endl;
                }
            }
        }

        closesocket(sock);
        cleanupSockets();
    }
}

void cleanupLoop(Config& cfg, int timeout_sec) {
    while (Discovery::isRunning()) {
        long now = duration_cast<seconds>(
            system_clock::now().time_since_epoch()
        ).count();

        cfg.removeStalePeers(now, timeout_sec);
        cfg.save();

        this_thread::sleep_for(seconds(10));
    }
}


void Discovery::start(Config& cfg, int discovery_port, int interval_sec, int timeout_sec) {
    if (running) return;
    running = true;
    senderThread = thread(senderLoop, ref(cfg), discovery_port, interval_sec);
    listenerThread = thread(listenerLoop, ref(cfg), discovery_port);
    cleanupThread  = thread(cleanupLoop, ref(cfg), timeout_sec);
}

void Discovery::stop() {
    if (!running) return;
    running = false;
    if (senderThread.joinable()) senderThread.join();
    if (listenerThread.joinable()) listenerThread.join();
    if (cleanupThread.joinable()) cleanupThread.join();
}
