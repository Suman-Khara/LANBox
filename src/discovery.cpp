#include "discovery.hpp"
#include "protocol.hpp"
#include "network_interface.hpp"
#include "crypto.hpp"
#include <bits/stdc++.h>
#include <chrono>
#include <set>

#ifdef _WIN32
    #define _HAS_STD_BYTE 0
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

// Platform-specific byte order conversions for 64-bit
#ifndef htonll
    #ifdef _WIN32
        #define htonll(x) ((1==htonl(1)) ? (x) : ((uint64_t)htonl((x) & 0xFFFFFFFF) << 32) | htonl((x) >> 32))
        #define ntohll(x) ((1==ntohl(1)) ? (x) : ((uint64_t)ntohl((x) & 0xFFFFFFFF) << 32) | ntohl((x) >> 32))
    #else
        #include <endian.h>
        #define htonll(x) htobe64(x)
        #define ntohll(x) be64toh(x)
    #endif
#endif

using namespace std;
using namespace chrono;

extern bool initSockets();
extern void cleanupSockets();

// Get hostname
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

// Get local IP (same as before)
string getLocalIP() {
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        return "0.0.0.0";
    }
#endif

    string localIP = "0.0.0.0";
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        return localIP;
    }

    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(80);
    inet_pton(AF_INET, "8.8.8.8", &addr.sin_addr);

    connect(sock, (struct sockaddr*)&addr, sizeof(addr));

    sockaddr_in name;
    socklen_t namelen = sizeof(name);
    if (getsockname(sock, (struct sockaddr*)&name, &namelen) == 0) {
        char buffer[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &name.sin_addr, buffer, sizeof(buffer));
        localIP = buffer;
    }

#ifdef _WIN32
    closesocket(sock);
    WSACleanup();
#else
    close(sock);
#endif

    return localIP;
}

// Check if IP is in private range
bool isPrivateIP(const string& ip) {
    uint32_t ip_uint = Protocol::ipToUint32(ip);
    
    // 10.0.0.0/8
    if ((ip_uint & 0xFF000000) == 0x0A000000) return true;
    
    // 172.16.0.0/12
    if ((ip_uint & 0xFFF00000) == 0xAC100000) return true;
    
    // 192.168.0.0/16
    if ((ip_uint & 0xFFFF0000) == 0xC0A80000) return true;
    
    return false;
}

namespace {
    static uint32_t g_sequence_number = 0;
    static set<uint32_t> g_seen_sequences;
    static time_t g_last_cleanup = time(nullptr);
    static map<string, pair<int, time_t>> g_packet_counts;  // IP -> (count, timestamp)
    
    const int MAX_PACKETS_PER_IP_PER_SEC = 50;
    
    // Clean old sequence numbers periodically
    void cleanupSequences() {
        time_t now = time(nullptr);
        if (now - g_last_cleanup > 60) {  // Every 60 seconds
            g_seen_sequences.clear();
            g_packet_counts.clear();
            g_last_cleanup = now;
        }
    }
    
    // Check rate limiting per IP
    bool checkRateLimit(const string& ip) {
        time_t now = time(nullptr);
        
        if (g_packet_counts.find(ip) == g_packet_counts.end()) {
            g_packet_counts[ip] = {1, now};
            return true;
        }
        
        auto& [count, timestamp] = g_packet_counts[ip];
        
        if (now - timestamp >= 1) {
            // Reset counter for new second
            count = 1;
            timestamp = now;
            return true;
        }
        
        if (count >= MAX_PACKETS_PER_IP_PER_SEC) {
            // Too many packets from this IP
            return false;
        }
        
        count++;
        return true;
    }

    // Sender thread - broadcasts discovery using BINARY protocol
    void senderLoop(Config& cfg, int port, int interval_sec) {
        if (!initSockets()) {
            cerr << "Socket init failed (sender)\n";
            return;
        }

        SocketType sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock == INVALID_SOCKET) {
            cerr << "Failed to create UDP socket\n";
            return;
        }

        int broadcastEnable = 1;
        setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (char*)&broadcastEnable, sizeof(broadcastEnable));

        sockaddr_in broadcastAddr{};
        broadcastAddr.sin_family = AF_INET;
        broadcastAddr.sin_port = htons(port);
        broadcastAddr.sin_addr.s_addr = htonl(INADDR_BROADCAST);

        string hostname = getHostName();
        string localIP = getLocalIP();
        uint32_t sender_ip = Protocol::ipToUint32(localIP);
        
        // NEW: Load public key
        string publicKey;
        try {
            publicKey = Crypto::loadPublicKey();
            cout << "[Sender] Loaded public key (fingerprint: " 
                << Crypto::getPublicKeyFingerprint() << ")\n";
        } catch (const exception& e) {
            cerr << "[Sender] Warning: No public key found. Run 'lanbox keygen' first.\n";
            cerr << "[Sender] Continuing without encryption...\n";
        }

        cout << "[Sender] Broadcasting as " << hostname << " (" << localIP << ")\n";

        while (Discovery::isRunning()) {
            // Create binary discovery message with public key
            vector<uint8_t> message = Protocol::createDiscoveryRequest(
                hostname,
                sender_ip,
                5000,
                g_sequence_number++,
                publicKey  // NEW: Include public key
            );

            int sent = sendto(sock, (char*)message.data(), message.size(), 0, 
                            (sockaddr*)&broadcastAddr, sizeof(broadcastAddr));
            
            if (sent < 0) {
                cerr << "[Sender] Send failed\n";
            }

            this_thread::sleep_for(seconds(interval_sec));
        }

        closesocket(sock);
    }

    void listenerLoop(Config& cfg, int port) {
        if (!initSockets()) {
            cerr << "Socket init failed (listener)\n";
            return;
        }

        SocketType sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock == INVALID_SOCKET) {
            cerr << "Failed to create UDP socket\n";
            return;
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_ANY);

        if (bind(sock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
            cerr << "Bind failed\n";
            closesocket(sock);
            return;
        }

        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));

        cout << "[Listener] Listening on port " << port << " (binary protocol)\n";

        char buffer[8192];
        string localIP = getLocalIP();
        string hostname = getHostName();
        uint32_t local_ip_uint = Protocol::ipToUint32(localIP);

        while (Discovery::isRunning()) {
            sockaddr_in senderAddr{};
            socklen_t senderLen = sizeof(senderAddr);
            
            int bytes = recvfrom(sock, buffer, sizeof(buffer), 0, 
                            (sockaddr*)&senderAddr, &senderLen);

            if (bytes > 0) {
                char senderIP[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &(senderAddr.sin_addr), senderIP, INET_ADDRSTRLEN);
                string sender_ip_str = senderIP;

                // Security checks
                cleanupSequences();
                
                if (!isPrivateIP(sender_ip_str)) continue;
                if (!checkRateLimit(sender_ip_str)) continue;
                if (bytes < (int)HEADER_SIZE || bytes > (int)MAX_PAYLOAD_SIZE) continue;

                LANBoxHeader header;
                vector<uint8_t> payload;
                
                if (!Protocol::parseMessage((uint8_t*)buffer, bytes, header, payload)) {
                    continue;
                }

                if (!Protocol::validateMessage((uint8_t*)buffer, bytes)) {
                    cerr << "[Listener] Invalid checksum from " << sender_ip_str << "\n";
                    continue;
                }

                uint32_t seq = ntohl(header.sequence_number);
                if (g_seen_sequences.count(seq)) continue;
                g_seen_sequences.insert(seq);

                MessageType msg_type = static_cast<MessageType>(ntohs(header.message_type));
                
                if (msg_type == MessageType::DISCOVERY_REQUEST) {
                    if (payload.size() >= sizeof(DiscoveryPayload)) {
                        DiscoveryPayload disc_payload;
                        memcpy(&disc_payload, payload.data(), sizeof(disc_payload));
                        
                        string device_name = string(disc_payload.device_name);
                        uint16_t tcp_port = ntohs(disc_payload.tcp_port);
                        uint32_t pub_key_len = ntohl(disc_payload.public_key_length);
                        uint32_t sig_len = ntohl(disc_payload.signature_length);
                        
                        // Extract public key
                        string peer_public_key;
                        string peer_fingerprint;
                        
                        if (pub_key_len > 0 && payload.size() >= sizeof(DiscoveryPayload) + pub_key_len) {
                            // Public key starts after the DiscoveryPayload struct
                            peer_public_key = string(
                                (char*)(payload.data() + sizeof(DiscoveryPayload)),
                                pub_key_len
                            );
                            
                            // Calculate fingerprint
                            unsigned char hash[32];
                            SHA256((unsigned char*)peer_public_key.c_str(), 
                                peer_public_key.length(), hash);
                            
                            char fingerprint[33];
                            for (int i = 0; i < 16; i++) {
                                sprintf(fingerprint + (i * 2), "%02x", hash[i]);
                            }
                            fingerprint[32] = '\0';
                            peer_fingerprint = string(fingerprint);
                        }
                        
                        // NEW: Extract and verify signature
                        bool signature_valid = false;
                        if (sig_len > 0 && payload.size() >= sizeof(DiscoveryPayload) + pub_key_len + sig_len) {
                            // Extract signature
                            vector<unsigned char> signature(
                                payload.data() + sizeof(DiscoveryPayload) + pub_key_len,
                                payload.data() + sizeof(DiscoveryPayload) + pub_key_len + sig_len
                            );
                            
                            // Reconstruct the signed message
                            uint32_t sender_ip_uint = ntohl(header.sender_ip);
                            uint64_t timestamp = ntohll(header.timestamp);
                            string message_to_verify = device_name + 
                                                    to_string(sender_ip_uint) + 
                                                    to_string(timestamp) + 
                                                    peer_public_key;
                            
                            // Verify signature
                            try {
                                signature_valid = Crypto::verifySignature(message_to_verify, signature, peer_public_key);
                                
                                if (signature_valid) {
                                    cout << "[Listener] ✓ Signature VALID from " << device_name << "\n";
                                } else {
                                    cerr << "[Listener] ✗ Signature INVALID from " << device_name << " - REJECTING\n";
                                    continue;  // Reject this peer!
                                }
                            } catch (const exception& e) {
                                cerr << "[Listener] Signature verification failed: " << e.what() << "\n";
                                continue;  // Reject on error
                            }
                        } else if (pub_key_len > 0) {
                            // Has public key but no signature - warn but accept for now
                            cout << "[Listener] Warning: " << device_name << " sent no signature (old version?)\n";
                        }
                        
                        Peer p;
                        p.setName(device_name);
                        p.setIP(sender_ip_str);
                        p.setPort(tcp_port);
                        p.setLastSeen(time(nullptr));
                        p.setSelf(sender_ip_str == localIP);
                        p.setPublicKey(peer_public_key);      // NEW
                        p.setFingerprint(peer_fingerprint);    // NEW
                        
                        cfg.addOrUpdatePeer(p);
                        
                        // Send response with OUR public key
                        if (sender_ip_str != localIP) {
                            string our_public_key;
                            try {
                                our_public_key = Crypto::loadPublicKey();
                            } catch (...) {
                                // No key, send without it
                            }
                            
                            vector<uint8_t> response = Protocol::createDiscoveryRequest(
                                hostname,
                                local_ip_uint,
                                5000,
                                g_sequence_number++,
                                our_public_key  // NEW: Include our public key
                            );
                            
                            // Change message type to RESPONSE
                            LANBoxHeader* resp_header = (LANBoxHeader*)response.data();
                            resp_header->message_type = htons(static_cast<uint16_t>(MessageType::DISCOVERY_RESPONSE));
                            
                            // Recalculate checksum
                            resp_header->checksum = 0;
                            uint32_t crc = Protocol::calculateCRC32(response.data(), response.size());
                            resp_header->checksum = htonl(crc);
                            
                            sendto(sock, (char*)response.data(), response.size(), 0,
                                (sockaddr*)&senderAddr, senderLen);
                        }
                    }
                } 
                else if (msg_type == MessageType::DISCOVERY_RESPONSE) {
                    // *** Handle RESPONSE messages ***
                    if (payload.size() >= sizeof(DiscoveryPayload)) {
                        DiscoveryPayload disc_payload;
                        memcpy(&disc_payload, payload.data(), sizeof(disc_payload));
                        
                        string device_name = string(disc_payload.device_name);
                        uint16_t tcp_port = ntohs(disc_payload.tcp_port);
                        uint32_t pub_key_len = ntohl(disc_payload.public_key_length);
                        
                        // NEW: Extract public key if present
                        string peer_public_key;
                        string peer_fingerprint;
                        
                        if (pub_key_len > 0 && payload.size() >= sizeof(DiscoveryPayload) + pub_key_len) {
                            // Public key starts after the DiscoveryPayload struct
                            peer_public_key = string(
                                (char*)(payload.data() + sizeof(DiscoveryPayload)),
                                pub_key_len
                            );
                            
                            // Calculate fingerprint
                            unsigned char hash[32];
                            SHA256((unsigned char*)peer_public_key.c_str(), 
                                peer_public_key.length(), hash);
                            
                            char fingerprint[33];
                            for (int i = 0; i < 16; i++) {
                                sprintf(fingerprint + (i * 2), "%02x", hash[i]);
                            }
                            fingerprint[32] = '\0';
                            peer_fingerprint = string(fingerprint);
                            
                            cout << "[Listener] Received response with public key from " << device_name 
                                << " (fingerprint: " << peer_fingerprint << ")\n";
                        }
                        
                        // Create/update peer with public key
                        Peer p;
                        p.setName(device_name);
                        p.setIP(sender_ip_str);
                        p.setPort(tcp_port);
                        p.setLastSeen(time(nullptr));
                        p.setSelf(sender_ip_str == localIP);
                        p.setPublicKey(peer_public_key);      // Store public key
                        p.setFingerprint(peer_fingerprint);    // Store fingerprint
                        
                        cfg.addOrUpdatePeer(p);
                    }
                }
                else if (msg_type == MessageType::HEARTBEAT) {
                    if (payload.size() >= sizeof(HeartbeatPayload)) {
                        HeartbeatPayload hb_payload;
                        memcpy(&hb_payload, payload.data(), sizeof(hb_payload));
                        
                        string device_name = string(hb_payload.device_name);
                        
                        Peer p;
                        p.setName(device_name);
                        p.setIP(sender_ip_str);
                        p.setPort(5000);
                        p.setLastSeen(time(nullptr));
                        p.setSelf(sender_ip_str == localIP);
                        
                        cfg.addOrUpdatePeer(p);
                    }
                }

            } else if (bytes == SOCKET_ERROR) {
    #ifdef _WIN32
                int err = WSAGetLastError();
                if (err != WSAETIMEDOUT && err != WSAEWOULDBLOCK) {
                    break;
                }
    #else
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    break;
                }
    #endif
            }
        }

        closesocket(sock);
        cout << "[Listener] Stopped\n";
    }

    // Cleanup thread (same as before)
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
}

void Discovery::start(Config& cfg, int discovery_port, int interval_sec, int timeout_sec) {
    if (running) return;
    running = true;
    
    cout << "=== Starting Discovery with Binary Protocol ===\n";
    cout << "Magic Number: 0x" << hex << LANBOX_MAGIC << dec << "\n";
    cout << "Protocol Version: " << PROTOCOL_VERSION << "\n";
    cout << "Discovery Port: " << discovery_port << "\n\n";
    
    senderThread = thread(senderLoop, ref(cfg), discovery_port, interval_sec);
    listenerThread = thread(listenerLoop, ref(cfg), discovery_port);
    cleanupThread = thread(cleanupLoop, ref(cfg), timeout_sec);
}

void Discovery::stop() {
    if (!running) return;
    running = false;
    if (senderThread.joinable()) senderThread.join();
    if (listenerThread.joinable()) listenerThread.join();
    if (cleanupThread.joinable()) cleanupThread.join();
}