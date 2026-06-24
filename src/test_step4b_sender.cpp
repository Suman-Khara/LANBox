#include "platform.hpp"
#include <bits/stdc++.h>
#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
#else
    #include <sys/socket.h>
    #include <arpa/inet.h>
    #include <netinet/in.h>
    #include <unistd.h>
#endif
#include "protocol.hpp"
#include "sync_manager.hpp"

using namespace std;

int main(int argc, char** argv) {
#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2,2), &wsaData);
#endif

    string broadcast_ip = "255.255.255.255";
    if (argc > 1) broadcast_ip = argv[1]; // optionally pass subnet broadcast, e.g. 192.168.1.255

    SocketType sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == INVALID_SOCKET_VAL) {
        cerr << "Failed to create socket\n";
        return 1;
    }

    int broadcast_enable = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST,
              (const char*)&broadcast_enable, sizeof(broadcast_enable));

    SyncNotifyPayload p;
    memset(&p, 0, sizeof(p));
    strncpy(p.group_id, "testgrp1", 8);
    strncpy(p.filename, "cross_machine_test.pdf", sizeof(p.filename) - 1);
    p.version   = 1;
    p.file_size = 99999;
    strncpy(p.checksum, "sha256:testchecksumvalue", sizeof(p.checksum) - 1);
    strncpy(p.owner_fp, "testfingerprint1", sizeof(p.owner_fp) - 1);

    auto msg = Protocol::createSyncNotify(0, 1, p);

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(SYNC_SIGNALING_PORT);
    inet_pton(AF_INET, broadcast_ip.c_str(), &addr.sin_addr);

    int sent = sendto(sock, (const char*)msg.data(), (int)msg.size(), 0,
                      (sockaddr*)&addr, sizeof(addr));

    if (sent == (int)msg.size()) {
        cout << "Sent SYNC_NOTIFY (" << sent << " bytes) to "
             << broadcast_ip << ":" << SYNC_SIGNALING_PORT << "\n";
        cout << "Check the listener's terminal/sync.log for:\n";
        cout << "  [DISPATCH] SYNC_NOTIFY ... cross_machine_test.pdf\n";
    } else {
        cerr << "Send failed\n";
    }

    closesocket(sock);
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}