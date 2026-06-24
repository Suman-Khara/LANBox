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
#include "sync_manager.hpp"
#include "protocol.hpp"

using namespace std;

int main(int argc, char** argv) {
#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2,2), &wsaData);
#endif

    if (argc < 2) {
        cout << "Usage: test_step4c_sender <target_ip> [file_to_send]\n";
        cout << "If no file is given, sends a small generated test file.\n";
        return 1;
    }

    string target_ip = argv[1];
    string filepath, filename, content;
    bool using_real_file = (argc >= 3);

    if (using_real_file) {
        filepath = argv[2];
        size_t slash = filepath.find_last_of("/\\");
        filename = (slash != string::npos) ? filepath.substr(slash + 1) : filepath;
    } else {
        filename = "cross_machine_step4c.txt";
        content  = "Hello from the Step 4c cross-machine sender! "
                  "This file proves real TCP transfer works over the LAN.";
    }

    SocketType sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET_VAL) {
        cerr << "Failed to create socket\n";
        return 1;
    }

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(SYNC_TRANSFER_PORT);
    inet_pton(AF_INET, target_ip.c_str(), &addr.sin_addr);

    if (connect(sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        cerr << "Failed to connect to " << target_ip << ":" << SYNC_TRANSFER_PORT << "\n";
        cerr << "Check the target machine's daemon is running and firewall allows it.\n";
        closesocket(sock);
        return 1;
    }

    cout << "Connected to " << target_ip << ":" << SYNC_TRANSFER_PORT << "\n";

    // SyncTransferHeader — transfer_type = 0 (legacy)
    SyncTransferHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.transfer_type = 0;
    send(sock, (const char*)&hdr, sizeof(hdr), 0);

    uint64_t file_size;
    ifstream infile;

    if (using_real_file) {
        infile.open(filepath, ios::binary | ios::ate);
        if (!infile.is_open()) {
            cerr << "Cannot open file: " << filepath << "\n";
            closesocket(sock);
            return 1;
        }
        file_size = (uint64_t)infile.tellg();
        infile.seekg(0);
    } else {
        file_size = content.size();
    }

    uint32_t nameLenNet = htonl((uint32_t)filename.size());
    send(sock, (const char*)&nameLenNet, sizeof(nameLenNet), 0);
    send(sock, filename.data(), (int)filename.size(), 0);

#ifdef _WIN32
    uint64_t sizeNet = _byteswap_uint64(file_size);
#else
    uint64_t sizeNet = htobe64(file_size);
#endif
    send(sock, (const char*)&sizeNet, sizeof(sizeNet), 0);

    uint8_t is_encrypted = 0;
    send(sock, (const char*)&is_encrypted, sizeof(is_encrypted), 0);

    cout << "Sending '" << filename << "' (" << file_size << " bytes)...\n";

    if (using_real_file) {
        vector<char> buffer(65536);
        uint64_t sent_total = 0;
        while (sent_total < file_size) {
            infile.read(buffer.data(), buffer.size());
            streamsize n = infile.gcount();
            if (n <= 0) break;
            send(sock, buffer.data(), (int)n, 0);
            sent_total += n;
        }
        infile.close();
    } else {
        send(sock, content.data(), (int)content.size(), 0);
    }

    cout << "Done. Check the listener's output for confirmation.\n";

    closesocket(sock);
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}