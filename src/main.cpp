#include <bits/stdc++.h>
#include "json.hpp"
#include "config.hpp"
#include "discovery.hpp"
#include "peer.hpp"

#include <chrono>
#include <csignal>

void handleSignal(int) {
    cout << "\nStopping LANBox..." << endl;
    Discovery::stop();
    exit(0);
}

#ifdef _WIN32
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

const string CONFIG_FILE = "../data/lanbox.json";
const int PORT = 5000;
const int BUFFER_SIZE = 64 * 1024; // 64 KB

bool initSockets() {
#ifdef _WIN32
    WSADATA wsaData;
    return (WSAStartup(MAKEWORD(2, 2), &wsaData) == 0);
#else
    return true;
#endif
}

void cleanupSockets() {
#ifdef _WIN32
    WSACleanup();
#endif
}

void receiveFile() {
    if (!initSockets()) {
        cerr << "Socket init failed\n";
        return;
    }

    SocketType server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == INVALID_SOCKET) {
        cerr << "Socket creation failed\n";
        cleanupSockets();
        exit(EXIT_FAILURE);
    }

    sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) == SOCKET_ERROR) {
        cerr << "Bind failed\n";
        closesocket(server_fd);
        cleanupSockets();
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, 3) == SOCKET_ERROR) {
        cerr << "Listen failed\n";
        closesocket(server_fd);
        cleanupSockets();
        exit(EXIT_FAILURE);
    }

    cout << "Server listening on port " << PORT << "...\n";
    socklen_t addrlen = sizeof(address);
    SocketType new_socket = accept(server_fd, (struct sockaddr*)&address, &addrlen);
    if (new_socket == INVALID_SOCKET) {
        cerr << "Accept failed\n";
        closesocket(server_fd);
        cleanupSockets();
        exit(EXIT_FAILURE);
    }

    cout << "Client connected!\n";

    // --- Receive metadata first ---
    uint32_t nameLen;
    recv(new_socket, (char*)&nameLen, sizeof(nameLen), 0);
    nameLen = ntohl(nameLen);

    string filename(nameLen, '\0');
    recv(new_socket, filename.data(), nameLen, 0);

    uint64_t netFileSize;
    recv(new_socket, (char*)&netFileSize, sizeof(netFileSize), 0);

#if defined(_WIN32)
    uint64_t fileSize = _byteswap_uint64(netFileSize);
#else
    uint64_t fileSize = be64toh(netFileSize);
#endif

    cout << "Receiving file: " << filename << " (" << fileSize << " bytes)\n";

    // --- Receive file ---
    ofstream outfile(filename, ios::binary);
    char buffer[BUFFER_SIZE];
    uint64_t received = 0;
    auto start = steady_clock::now();

    while (received < fileSize) {
        int bytesRead = recv(new_socket, buffer, BUFFER_SIZE, 0);
        if (bytesRead <= 0) break;

        outfile.write(buffer, bytesRead);
        received += bytesRead;

        // --- Progress display ---
        double receivedMB = received / (1024.0 * 1024.0);
        double totalMB   = fileSize / (1024.0 * 1024.0);

        auto now = std::chrono::steady_clock::now();
        double elapsedSec = std::chrono::duration<double>(now - start).count();

        double speedMBps = (received / (1024.0 * 1024.0)) / elapsedSec; // MB/s

        double remainingMB = totalMB - receivedMB;
        double etaSec = (speedMBps > 0) ? (remainingMB / speedMBps) : -1;

        // Format ETA
        int etaMin = (etaSec > 0) ? static_cast<int>(etaSec / 60) : 0;
        int etaS   = (etaSec > 0) ? static_cast<int>(etaSec) % 60 : 0;

        cout << "\rReceived: " << fixed << setprecision(2)
            << receivedMB << " MB / " << totalMB << " MB"
            << " | Speed: " << setprecision(2) << speedMBps << " MB/s"
            << " | ETA: ";
        if (etaSec > 0)
            cout << etaMin << "m " << etaS << "s   ";
        else
            cout << "--   ";

        cout.flush();
    }

    auto end = steady_clock::now();
    double totalTime = duration<double>(end - start).count();
    cout << "\nFile received successfully in " << fixed << setprecision(2)
        << totalTime << " seconds.\n";

    outfile.close();
    closesocket(new_socket);
    closesocket(server_fd);
    cleanupSockets();
}

void sendFile(const string &server_ip, const string &filename) {
    if (!initSockets()) {
        cerr << "Socket init failed\n";
        return;
    }
    
    SocketType sock;
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET) {
        cerr << "Socket creation failed\n";
        cleanupSockets();
        exit(EXIT_FAILURE);
    }
    
    sockaddr_in serv_addr;
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT);

    if (inet_pton(AF_INET, server_ip.c_str(), &serv_addr.sin_addr) <= 0) {
        cerr << "Invalid address\n";
        closesocket(sock);
        cleanupSockets();
        exit(EXIT_FAILURE);
    }

    if (connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) == SOCKET_ERROR) {
        cerr << "Connection failed\n";
        closesocket(sock);
        cleanupSockets();
        exit(EXIT_FAILURE);
    }

    cout << "Connected to server. Sending file...\n";

    // Open file
    ifstream infile(filename, ios::binary | ios::ate);
    if (!infile.is_open()) {
        cerr << "Cannot open file: " << filename << "\n";
        closesocket(sock);
        cleanupSockets();
        exit(EXIT_FAILURE);
    }

    uint64_t fileSize = infile.tellg();
    infile.seekg(0);

    // Extract just the base filename (no path)
    string baseName = filename.substr(filename.find_last_of("/\\") + 1);

    // --- Send metadata ---
    uint32_t nameLen = htonl(baseName.size());
    send(sock, (char*)&nameLen, sizeof(nameLen), 0);
    send(sock, baseName.c_str(), baseName.size(), 0);

#if defined(_WIN32)
    uint64_t sizeNet = _byteswap_uint64(fileSize);
#else
    uint64_t sizeNet = htobe64(fileSize);
#endif
    send(sock, (char*)&sizeNet, sizeof(sizeNet), 0);

    // --- Send file ---
    char buffer[BUFFER_SIZE];
    uint64_t sent = 0;

    auto start = steady_clock::now();

    while (!infile.eof()) {
        infile.read(buffer, BUFFER_SIZE);
        int bytesRead = infile.gcount();
        if (bytesRead <= 0) break;

        send(sock, buffer, bytesRead, 0);
        sent += bytesRead;

        // Progress & speed
        auto now = steady_clock::now();
        double elapsedSec = duration<double>(now - start).count();

        double sentMB   = sent / (1024.0 * 1024.0);
        double totalMB  = fileSize / (1024.0 * 1024.0);
        double speedMBs = (sentMB / elapsedSec); // MB/s

        double remainingMB = totalMB - sentMB;
        double etaSec = (speedMBs > 0) ? (remainingMB / speedMBs) : -1;

        int etaMin = (etaSec > 0) ? static_cast<int>(etaSec / 60) : 0;
        int etaS   = (etaSec > 0) ? static_cast<int>(etaSec) % 60 : 0;

        double progress = (double)sent / fileSize * 100.0;

        cout << "\rSent: " << fixed << setprecision(2)
            << sentMB << " MB / " << totalMB << " MB ("
            << progress << "%)"
            << " | Speed: " << setprecision(2) << speedMBs << " MB/s"
            << " | ETA: ";
        if (etaSec > 0)
            cout << etaMin << "m " << etaS << "s   ";
        else
            cout << "--   ";

        cout.flush();
    }

    auto end = steady_clock::now();
    double totalTimeSec = duration<double>(end - start).count();
    cout << "\nFile sent successfully!\n";
    cout << "Total time: " << fixed << setprecision(2) 
        << totalTimeSec << " seconds\n";

    infile.close();
    closesocket(sock);
    cleanupSockets();
}

int main(int argc, char* argv[]) {
    Config cfg;
    Discovery::start(cfg);

    if (argc < 2) {
        cout << "Usage:\n";
        cout << "  lanbox.exe server\n";
        cout << "  lanbox.exe send <server_ip> <filename>\n";
        cout << "  lanbox.exe peers\n";
        return 1;
    }

    string mode = argv[1];
    if (mode == "server") {
        receiveFile();
    } else if (mode == "send" && argc == 4) {
        sendFile(argv[2], argv[3]);
    } else if (mode == "peers") {
        cfg.printPeers();
    } else {
        cout << "Invalid arguments\n";
    }

    signal(SIGINT, handleSignal);
    return 0;
}
