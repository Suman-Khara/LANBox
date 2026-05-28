#include "platform.hpp"  // Include this FIRST
#include <bits/stdc++.h>
#include "json.hpp"
#include "config.hpp"
#include "discovery.hpp"
#include "peer.hpp"
#include "crypto.hpp"
#include "network_interface.hpp"
#include <chrono>
#include <csignal>
#include <atomic>

// Global flag for signal handling
atomic<bool> g_running(true);

void handleSignal(int) {
    if (!g_running) {
        // Second Ctrl+C - force exit
        cout << "\nForce stopping...\n";
        exit(1);
    }
    
    cout << "\n\nReceived interrupt signal. Shutting down gracefully...\n";
    cout << "(Press Ctrl+C again to force quit)\n";
    g_running = false;
    Discovery::stop();
}

#ifdef _WIN32
    #define _HAS_STD_BYTE 0  // prevent conflict with byte
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

// Helper: Add suffix before extension
string addSuffixBeforeExtension(const string& filename, const string& suffix) {
    size_t dot_pos = filename.find_last_of('.');
    if (dot_pos == string::npos) {
        return filename + suffix;
    } else {
        string name = filename.substr(0, dot_pos);
        string ext = filename.substr(dot_pos);
        return name + suffix + ext;
    }
}

// Unified send function (handles both encrypted and unencrypted)
void sendFile(const string& server_ip, const string& filename, bool encrypt = false, const string& peer_public_key = "") {
    if (!initSockets()) {
        cerr << "Socket init failed\n";
        return;
    }
    
    SocketType sock = socket(AF_INET, SOCK_STREAM, 0);
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

    cout << "Connected to server. " << (encrypt ? "Encrypting and sending" : "Sending") << " file...\n";

    // Encryption handling
    string file_to_send = filename;
    string temp_encrypted;
    Crypto::EncryptedFileInfo enc_info;
    
    if (encrypt) {
        temp_encrypted = addSuffixBeforeExtension(filename, "_temp_enc");
        try {
            enc_info = Crypto::encryptFile(filename, temp_encrypted, peer_public_key);
            file_to_send = temp_encrypted;
            cout << "File encrypted successfully.\n";
        } catch (const exception& e) {
            cerr << "Encryption failed: " << e.what() << "\n";
            closesocket(sock);
            cleanupSockets();
            return;
        }
    }

    // Open file
    ifstream infile(file_to_send, ios::binary | ios::ate);
    if (!infile.is_open()) {
        cerr << "Cannot open file: " << file_to_send << "\n";
        closesocket(sock);
        cleanupSockets();
        if (encrypt) remove(temp_encrypted.c_str());
        exit(EXIT_FAILURE);
    }

    uint64_t fileSize = infile.tellg();
    infile.seekg(0);

    string baseName = filename.substr(filename.find_last_of("/\\") + 1);

    // Send metadata
    uint32_t nameLen = htonl(baseName.size());
    send(sock, (char*)&nameLen, sizeof(nameLen), 0);
    send(sock, baseName.c_str(), baseName.size(), 0);

    #if defined(_WIN32)
        uint64_t sizeNet = _byteswap_uint64(fileSize);
    #else
        uint64_t sizeNet = htobe64(fileSize);
    #endif
    send(sock, (char*)&sizeNet, sizeof(sizeNet), 0);

    // Send encryption flag and metadata
    uint8_t is_encrypted = encrypt ? 1 : 0;
    send(sock, (char*)&is_encrypted, sizeof(is_encrypted), 0);

    if (encrypt) {
        uint32_t key_len = htonl(enc_info.encrypted_aes_key.size());
        send(sock, (char*)&key_len, sizeof(key_len), 0);
        send(sock, (char*)enc_info.encrypted_aes_key.data(), enc_info.encrypted_aes_key.size(), 0);
        send(sock, (char*)enc_info.iv.data(), 16, 0);
        cout << "Encryption metadata sent.\n";
    }

    cout << "Transferring " << (encrypt ? "encrypted " : "") << "file...\n";

    // Send file
    char buffer[BUFFER_SIZE];
    uint64_t sent = 0;
    auto start = steady_clock::now();

    while (!infile.eof()) {
        infile.read(buffer, BUFFER_SIZE);
        int bytesRead = infile.gcount();
        if (bytesRead <= 0) break;

        send(sock, buffer, bytesRead, 0);
        sent += bytesRead;

        // Progress with ETA
        auto now = steady_clock::now();
        double elapsedSec = duration<double>(now - start).count();
        double sentMB = sent / (1024.0 * 1024.0);
        double totalMB = fileSize / (1024.0 * 1024.0);
        double speedMBs = (sentMB / elapsedSec);
        double progress = (double)sent / fileSize * 100.0;

        // Calculate ETA
        double remainingMB = totalMB - sentMB;
        double etaSec = (speedMBs > 0) ? (remainingMB / speedMBs) : -1;
        int etaMin = (etaSec > 0) ? static_cast<int>(etaSec / 60) : 0;
        int etaS = (etaSec > 0) ? static_cast<int>(etaSec) % 60 : 0;

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
    cout << "\n✓ File sent successfully!\n";
    cout << "Total time: " << fixed << setprecision(2) << totalTimeSec << " seconds\n";

    infile.close();
    closesocket(sock);
    cleanupSockets();

    // Cleanup temp file
    if (encrypt) {
        remove(temp_encrypted.c_str());
    }
}

// Unified receive function (handles both encrypted and unencrypted)
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

    // Receive metadata
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

    // Check encryption flag
    uint8_t is_encrypted;
    recv(new_socket, (char*)&is_encrypted, sizeof(is_encrypted), 0);

    vector<unsigned char> encrypted_aes_key;
    vector<unsigned char> iv;

    if (is_encrypted) {
        cout << "Receiving ENCRYPTED file: " << filename << " (" << fileSize << " bytes)\n";

        // Receive encrypted AES key
        uint32_t key_len;
        recv(new_socket, (char*)&key_len, sizeof(key_len), 0);
        key_len = ntohl(key_len);

        encrypted_aes_key.resize(key_len);
        recv(new_socket, (char*)encrypted_aes_key.data(), key_len, 0);

        // Receive IV
        iv.resize(16);
        recv(new_socket, (char*)iv.data(), 16, 0);

        cout << "Encryption metadata received.\n";
    } else {
        cout << "Receiving file: " << filename << " (" << fileSize << " bytes)\n";
    }

    // Receive file
    string temp_file = is_encrypted ? addSuffixBeforeExtension(filename, "_temp_enc") : filename;
    ofstream outfile(temp_file, ios::binary);
    
    char buffer[BUFFER_SIZE];
    uint64_t received = 0;
    auto start = steady_clock::now();

    while (received < fileSize) {
        int bytesRead = recv(new_socket, buffer, BUFFER_SIZE, 0);
        if (bytesRead <= 0) break;

        outfile.write(buffer, bytesRead);
        received += bytesRead;

        // Progress with ETA
        double receivedMB = received / (1024.0 * 1024.0);
        double totalMB = fileSize / (1024.0 * 1024.0);
        auto now = steady_clock::now();
        double elapsedSec = duration<double>(now - start).count();
        double speedMBps = (receivedMB / elapsedSec);

        // Calculate ETA
        double remainingMB = totalMB - receivedMB;
        double etaSec = (speedMBps > 0) ? (remainingMB / speedMBps) : -1;
        int etaMin = (etaSec > 0) ? static_cast<int>(etaSec / 60) : 0;
        int etaS = (etaSec > 0) ? static_cast<int>(etaSec) % 60 : 0;

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

    outfile.close();
    auto end = steady_clock::now();
    double totalTime = duration<double>(end - start).count();
    
    cout << "\nFile received in " << fixed << setprecision(2) << totalTime << " seconds.\n";

    // Decrypt if encrypted
    if (is_encrypted) {
        cout << "Decrypting file...\n";
        auto decrypt_start = steady_clock::now();
        
        try {
            if (Crypto::decryptFile(temp_file, filename, encrypted_aes_key, iv)) {
                auto decrypt_end = steady_clock::now();
                double decrypt_time = duration<double>(decrypt_end - decrypt_start).count();
                
                cout << "✓ File decrypted successfully: " << filename << "\n";
                cout << "Decryption time: " << fixed << setprecision(2) << decrypt_time << " seconds\n";
                
                // Remove temporary encrypted file
                remove(temp_file.c_str());
            } else {
                cerr << "Decryption failed!\n";
            }
        } catch (const exception& e) {
            cerr << "Decryption error: " << e.what() << "\n";
        }
    }

    closesocket(new_socket);
    closesocket(server_fd);
    cleanupSockets();
}

int main(int argc, char* argv[]) {
    #ifdef _WIN32
        // Enable UTF-8 output on Windows
        SetConsoleOutputCP(CP_UTF8);
    #endif
    if (argc < 2) {
        cout << "LANBox - LAN File Synchronization Tool\n";
        cout << "========================================\n";
        cout << "Usage:\n";
        cout << "  lanbox server                       - Start file receive server\n";
        cout << "  lanbox send <peer> <file>           - Send file (unencrypted)\n";
        cout << "  lanbox send <peer> <file> --encrypt - Send file (encrypted)\n";
        cout << "  lanbox peers                        - Show discovered peers\n";
        cout << "  lanbox interfaces                   - Show network interfaces\n";
        cout << "  lanbox discover                     - Start peer discovery\n";
        cout << "  lanbox keygen                       - Generate RSA key pair\n";
        return 1;
    }

    string mode = argv[1];
    if (mode == "keygen") {
        cout << "=== LANBox Key Generation ===\n\n";
        
        if (Crypto::keysExist()) {
            cout << "Keys already exist!\n";
            cout << "Current fingerprint: " << Crypto::getPublicKeyFingerprint() << "\n\n";
            cout << "Do you want to regenerate? This will create NEW keys. (y/n): ";
            
            char response;
            cin >> response;
            
            if (response != 'y' && response != 'Y') {
                cout << "Keeping existing keys.\n";
                return 0;
            }
        }
        
        if (Crypto::generateKeyPair(2048)) {
            cout << "\n✓ Success!\n";
            cout << "Your device fingerprint: " << Crypto::getPublicKeyFingerprint() << "\n";
            cout << "\nThis fingerprint uniquely identifies your device.\n";
            return 0;
        } else {
            cerr << "Key generation failed.\n";
            return 1;
        }
    }

    // Commands that DON'T need discovery running
    if (mode == "interfaces") {
        auto interfaces = NetworkInterfaceManager::getActiveInterfaces();
        NetworkInterfaceManager::printInterfaces(interfaces);
        return 0;
    }
    
    if (mode == "server") {
        receiveFile();
        return 0;
    }
    
    if (mode == "send" && argc >= 4) {
        string peerNameOrIP = argv[2];
        string filename = argv[3];
        bool encrypt = (argc >= 5 && string(argv[4]) == "--encrypt");
        
        // Load config to check discovered peers
        Config cfg;
        cfg.load();
        
        string targetIP = peerNameOrIP;
        Peer* peer = nullptr;
        
        // Check if it's a peer name (not an IP address)
        if (peerNameOrIP.find('.') == string::npos) {
            // Looks like a name, not an IP - search in peers
            peer = cfg.findPeer(peerNameOrIP);
            
            if (peer == nullptr) {
                cerr << "Error: Peer '" << peerNameOrIP << "' not found in discovered peers.\n";
                cerr << "Run 'lanbox peers' to see available peers.\n";
                cerr << "Or run 'lanbox discover' to find peers on the network.\n";
                return 1;
            }
            
            if (peer->isSelf()) {
                cerr << "Error: Cannot send file to yourself!\n";
                return 1;
            }
            
            targetIP = peer->getIP();
            cout << "Resolved '" << peerNameOrIP << "' to " << targetIP << "\n";
        } else {
            // It's an IP address - check if we know this peer
            if (cfg.hasPeer(targetIP)) {
                peer = cfg.findPeer(targetIP);
                cout << "Sending to known peer: " << peer->getName() 
                     << " (" << targetIP << ")\n";
            } else {
                cout << "Warning: " << targetIP << " is not in discovered peers list.\n";
                cout << "Attempting to send anyway...\n";
            }
        }
        
        if (encrypt) {
            // Need peer's public key for encryption
            if (peer == nullptr || !peer->hasPublicKey()) {
                cerr << "Error: Cannot encrypt - peer's public key not found.\n";
                cerr << "Make sure peer has been discovered and has keys.\n";
                cerr << "Run 'lanbox peers' to check.\n";
                return 1;
            }
            
            cout << "Encrypting file for " << peerNameOrIP << "...\n";
            sendFile(targetIP, filename, encrypt, peer->getPublicKey());
        } else {
            sendFile(targetIP, filename);
        }
        return 0;
    }
    
    if (mode == "test-encrypt") {
        if (argc < 3) {
            cout << "Usage: lanbox test-encrypt <filename>\n";
            return 1;
        }
        
        string test_file = argv[2];
        
        // Helper function to add suffix before extension
        auto addSuffix = [](const string& filename, const string& suffix) -> string {
            size_t dot_pos = filename.find_last_of('.');
            if (dot_pos == string::npos) {
                return filename + suffix;
            } else {
                string name = filename.substr(0, dot_pos);
                string ext = filename.substr(dot_pos);
                return name + suffix + ext;
            }
        };
        
        string enc_file = addSuffix(test_file, "_enc");
        string dec_file = addSuffix(test_file, "_dec");
        
        cout << "=== Testing File Encryption ===\n\n";
        cout << "Original: " << test_file << "\n";
        cout << "Encrypted: " << enc_file << "\n";
        cout << "Decrypted: " << dec_file << "\n\n";
        
        try {
            // Get original file size
            ifstream orig(test_file, ios::binary | ios::ate);
            size_t orig_size = orig.tellg();
            orig.close();
            cout << "Original file size: " << orig_size << " bytes\n";
            
            // Encrypt
            string pub_key = Crypto::loadPublicKey();
            cout << "Encrypting...\n";
            auto info = Crypto::encryptFile(test_file, enc_file, pub_key);
            
            cout << "[OK] File encrypted to: " << enc_file << "\n";
            cout << "[OK] Encrypted AES key: " << info.encrypted_aes_key.size() << " bytes\n";
            cout << "[OK] IV: " << info.iv.size() << " bytes\n\n";
            
            // Decrypt
            cout << "Decrypting...\n";
            if (Crypto::decryptFile(enc_file, dec_file, info.encrypted_aes_key, info.iv)) {
                
                // Get decrypted file size
                ifstream dec(dec_file, ios::binary | ios::ate);
                size_t dec_size = dec.tellg();
                dec.close();
                
                cout << "[OK] File decrypted to: " << dec_file << "\n";
                cout << "Decrypted file size: " << dec_size << " bytes\n";
                
                if (dec_size == orig_size) {
                    cout << "[OK] ✓ Sizes match!\n\n";
                    cout << "✓ Encryption test PASSED!\n";
                    cout << "\nYou can now:\n";
                    cout << "  - Open " << dec_file << " directly (no rename needed!)\n";
                    cout << "  - Try to open " << enc_file << " (should fail - it's encrypted)\n";
                } else {
                    cout << "[ERROR] Size mismatch!\n";
                }
                
            } else {
                cerr << "Decryption failed!\n";
                return 1;
            }
            
        } catch (const exception& e) {
            cerr << "Test failed: " << e.what() << "\n";
            return 1;
        }
        
        return 0;
    }

    // Commands that NEED discovery running
    Config cfg;
    cfg.load();
    
    if (mode == "discover") {
        signal(SIGINT, handleSignal);
        signal(SIGTERM, handleSignal);
        
        cout << "Starting peer discovery...\n";
        cout << "Press Ctrl+C to stop.\n\n";
        
        Discovery::start(cfg);
        
        int counter = 0;
        while (g_running) {
            this_thread::sleep_for(chrono::seconds(1));
            
            if (++counter % 5 == 0) {
                cout << "\r=== Discovered Peers (refresh #" << counter/5 << ") ===\n";
                cfg.printPeers();
                cfg.save();
                cout << "\n";
            } else {
                auto peers = cfg.getPeers();
                size_t active_count = 0;
                for (const auto& p : peers) {
                    if (!p.isSelf()) active_count++;
                }
                cout << "\r[Active] Peers: " << active_count << " | Time: " << counter << "s   ";
                cout.flush();
            }
        }
        
        cout << "\n\nStopping discovery...\n";
        Discovery::stop();
        
        cout << "\nFinal peer list:\n";
        cfg.printPeers();
        cfg.save();
        
        cout << "\nShutdown complete.\n";
        return 0;
    }
    
    if (mode == "peers") {
        cout << "=== Current Peers (from saved config) ===\n";
        if (cfg.getPeers().empty()) {
            cout << "No peers discovered yet.\n";
            cout << "Run 'lanbox discover' to find peers on the network.\n";
        } else {
            cfg.printPeers();
            cout << "\nYou can send files using peer names:\n";
            cout << "Example: lanbox send <peer_name> <filename>\n";
        }
        return 0;
    }
    
    cout << "Invalid command. Use 'lanbox' without arguments to see usage.\n";
    return 1;
}