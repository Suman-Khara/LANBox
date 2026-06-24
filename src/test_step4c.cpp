#include "platform.hpp"
#include <bits/stdc++.h>
#ifdef _WIN32
    #include <windows.h>
    #include <winsock2.h>
    #include <ws2tcpip.h>
#else
    #include <unistd.h>
    #include <sys/socket.h>
    #include <arpa/inet.h>
    #include <netinet/in.h>
#endif
#include "sync_manager.hpp"
#include "config.hpp"
#include "group_manager.hpp"

using namespace std;

// ============================================================================
// Test harness
// ============================================================================

static int tests_run    = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define CHECK(label, condition)                              \
    do {                                                     \
        tests_run++;                                         \
        if (condition) {                                     \
            cout << "  [PASS] " << label << "\n";           \
            tests_passed++;                                  \
        } else {                                             \
            cout << "  [FAIL] " << label << "\n";           \
            tests_failed++;                                  \
        }                                                    \
    } while(0)

#define SECTION(name) cout << "\n--- " << name << " ---\n";

static void sleepMs(int ms) {
#ifdef _WIN32
    Sleep(ms);
#else
    usleep(ms * 1000);
#endif
}

static string TEST_DIR;

static vector<string> readLastLogLines(int n) {
    string path = GroupManager::getLanboxDir() +
#ifdef _WIN32
        "\\sync.log";
#else
        "/sync.log";
#endif
    ifstream f(path);
    vector<string> lines;
    string line;
    while (getline(f, line)) lines.push_back(line);
    if ((int)lines.size() > n) lines.erase(lines.begin(), lines.end() - n);
    return lines;
}

static bool logContains(const vector<string>& lines, const string& needle) {
    for (const auto& l : lines) if (l.find(needle) != string::npos) return true;
    return false;
}

// Connects to localhost:5000 and sends raw bytes — simulates a sender
static SocketType connectToTransferPort() {
    SocketType sock = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(SYNC_TRANSFER_PORT);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        closesocket(sock);
        return INVALID_SOCKET_VAL;
    }
    return sock;
}

static void sendAll(SocketType sock, const void* data, size_t len) {
    send(sock, (const char*)data, (int)len, 0);
}

// ============================================================================
// Tests
// ============================================================================

void test_transfer_socket_binds() {
    SECTION("Transfer socket binds and daemon starts cleanly");

    Config cfg("test_tmp_step4c_lanbox.json");
    SyncManager sm(cfg);

    bool started = sm.startDaemon();
    CHECK("startDaemon succeeds with transfer socket", started);

    sleepMs(300);

    auto lines = readLastLogLines(20);
    CHECK("Startup log mentions transfer port",
          logContains(lines, "Transfer loop listening on TCP :5000"));

    sm.stopDaemon();
}

void test_legacy_transfer_unencrypted() {
    SECTION("Legacy unencrypted transfer (transfer_type=0)");

    Config cfg("test_tmp_step4c_lanbox.json");
    SyncManager sm(cfg);
    sm.startDaemon();
    sleepMs(300);

    SocketType sock = connectToTransferPort();
    CHECK("Connected to transfer port", sock != INVALID_SOCKET_VAL);

    // SyncTransferHeader: transfer_type=0, rest doesn't matter for legacy
    SyncTransferHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.transfer_type = 0;
    sendAll(sock, &hdr, sizeof(hdr));

    // Legacy wire format
    string filename = "step4c_legacy_test.txt";
    string content  = "hello from legacy transfer test";

    uint32_t nameLenNet = htonl((uint32_t)filename.size());
    sendAll(sock, &nameLenNet, sizeof(nameLenNet));
    sendAll(sock, filename.data(), filename.size());

#ifdef _WIN32
    uint64_t sizeNet = _byteswap_uint64((uint64_t)content.size());
#else
    uint64_t sizeNet = htobe64((uint64_t)content.size());
#endif
    sendAll(sock, &sizeNet, sizeof(sizeNet));

    uint8_t is_encrypted = 0;
    sendAll(sock, &is_encrypted, sizeof(is_encrypted));

    sendAll(sock, content.data(), content.size());

    sleepMs(500); // give the handler thread time to finish writing

    closesocket(sock);

    ifstream check(filename, ios::binary);
    CHECK("Received file exists", check.is_open());
    if (check.is_open()) {
        string read_content((istreambuf_iterator<char>(check)), istreambuf_iterator<char>());
        CHECK("Received file content matches", read_content == content);
    }
    remove(filename.c_str());

    auto lines = readLastLogLines(20);
    CHECK("Legacy receive logged",
          logContains(lines, "[LEGACY] Received 'step4c_legacy_test.txt'"));

    sm.stopDaemon();
}

void test_multiple_concurrent_connections() {
    SECTION("Multiple concurrent connections handled without blocking");

    Config cfg("test_tmp_step4c_lanbox.json");
    SyncManager sm(cfg);
    sm.startDaemon();
    sleepMs(300);

    const int N = 3;
    vector<SocketType> socks;
    vector<string> filenames;

    for (int i = 0; i < N; ++i) {
        SocketType s = connectToTransferPort();
        CHECK("Connection " + to_string(i) + " established", s != INVALID_SOCKET_VAL);
        socks.push_back(s);
        filenames.push_back("step4c_concurrent_" + to_string(i) + ".txt");
    }

    // Send headers + data for all connections — interleaved to prove
    // the server isn't blocking on one connection while waiting for another
    for (int i = 0; i < N; ++i) {
        SyncTransferHeader hdr;
        memset(&hdr, 0, sizeof(hdr));
        hdr.transfer_type = 0;
        sendAll(socks[i], &hdr, sizeof(hdr));

        uint32_t nameLenNet = htonl((uint32_t)filenames[i].size());
        sendAll(socks[i], &nameLenNet, sizeof(nameLenNet));
        sendAll(socks[i], filenames[i].data(), filenames[i].size());

        string content = "concurrent content " + to_string(i);
#ifdef _WIN32
        uint64_t sizeNet = _byteswap_uint64((uint64_t)content.size());
#else
        uint64_t sizeNet = htobe64((uint64_t)content.size());
#endif
        sendAll(socks[i], &sizeNet, sizeof(sizeNet));

        uint8_t is_encrypted = 0;
        sendAll(socks[i], &is_encrypted, sizeof(is_encrypted));
        sendAll(socks[i], content.data(), content.size());
    }

    sleepMs(700);

    for (auto s : socks) closesocket(s);

    bool all_received = true;
    for (int i = 0; i < N; ++i) {
        ifstream f(filenames[i]);
        if (!f.is_open()) all_received = false;
        remove(filenames[i].c_str());
    }
    CHECK("All concurrent transfers completed", all_received);

    sm.stopDaemon();
}

void test_group_sync_transfer_writes_temp_file() {
    SECTION("Group sync transfer (transfer_type=1) writes .lanbox_recv_tmp_*");

    TEST_DIR = "test_tmp_step4c_group";
    GroupManager::mkdirP(TEST_DIR);

    // Create a real group so SyncManager can resolve group_id -> folder_path
    GroupManager::createGroup(TEST_DIR, "Step4cGroup", "Tester", "fingerprint4c");
    GroupConfig gcfg;
    GroupManager::loadGroupFromFolder(TEST_DIR, gcfg);

    Config cfg("test_tmp_step4c_lanbox.json");
    SyncManager sm(cfg);
    sm.startDaemon();
    sleepMs(300);

    SocketType sock = connectToTransferPort();
    CHECK("Connected to transfer port", sock != INVALID_SOCKET_VAL);

    SyncTransferHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.transfer_type = 1;
    strncpy(hdr.group_id, gcfg.group_id.c_str(), 8);
    strncpy(hdr.relative_path, "synced_file.txt", sizeof(hdr.relative_path) - 1);
    sendAll(sock, &hdr, sizeof(hdr));

    string filename = "synced_file.txt";
    string content  = "group sync content for step 4c";

    uint32_t nameLenNet = htonl((uint32_t)filename.size());
    sendAll(sock, &nameLenNet, sizeof(nameLenNet));
    sendAll(sock, filename.data(), filename.size());

#ifdef _WIN32
    uint64_t sizeNet = _byteswap_uint64((uint64_t)content.size());
#else
    uint64_t sizeNet = htobe64((uint64_t)content.size());
#endif
    sendAll(sock, &sizeNet, sizeof(sizeNet));

    uint8_t is_encrypted = 0;
    sendAll(sock, &is_encrypted, sizeof(is_encrypted));
    sendAll(sock, content.data(), content.size());

    sleepMs(500);
    closesocket(sock);

    string tmp_path = TEST_DIR +
#ifdef _WIN32
        "\\" + RECV_TMP_PREFIX + "synced_file.txt";
#else
        "/" + RECV_TMP_PREFIX + "synced_file.txt";
#endif

    ifstream check(tmp_path, ios::binary);
    CHECK("Temp file was created", check.is_open());
    if (check.is_open()) {
        string read_content((istreambuf_iterator<char>(check)), istreambuf_iterator<char>());
        CHECK("Temp file content matches", read_content == content);
    }

    auto lines = readLastLogLines(20);
    CHECK("Group sync receive logged",
          logContains(lines, "[SYNC-RECV] Receiving 'synced_file.txt'"));
    CHECK("Temp file finalize-pending logged",
          logContains(lines, "awaiting finalize logic from Step 4g"));

    sm.stopDaemon();

    // Cleanup
    remove(tmp_path.c_str());
    string group_cfg_path = TEST_DIR +
#ifdef _WIN32
        "\\.lanbox_group.json";
#else
        "/.lanbox_group.json";
#endif
    string meta_path = TEST_DIR +
#ifdef _WIN32
        "\\.lanbox_metadata.json";
#else
        "/.lanbox_metadata.json";
#endif
    remove(group_cfg_path.c_str());
    remove(meta_path.c_str());
#ifdef _WIN32
    RemoveDirectoryA(TEST_DIR.c_str());
#else
    rmdir(TEST_DIR.c_str());
#endif
}

void test_group_sync_unknown_group_rejected() {
    SECTION("Group sync transfer with unknown group_id is rejected");

    Config cfg("test_tmp_step4c_lanbox.json");
    SyncManager sm(cfg);
    sm.startDaemon();
    sleepMs(300);

    SocketType sock = connectToTransferPort();
    CHECK("Connected to transfer port", sock != INVALID_SOCKET_VAL);

    SyncTransferHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.transfer_type = 1;
    strncpy(hdr.group_id, "nosuchid", 8); // Not a real group
    strncpy(hdr.relative_path, "ghost.txt", sizeof(hdr.relative_path) - 1);
    sendAll(sock, &hdr, sizeof(hdr));

    sleepMs(500);

    // Connection should have been closed by the server — verify by trying
    // to send more data and checking for failure, OR just check the log.
    auto lines = readLastLogLines(20);
    CHECK("Unknown group rejection logged",
          logContains(lines, "Unknown group_id 'nosuchid'"));

    closesocket(sock);
    sm.stopDaemon();
}

void test_incomplete_transfer_discarded() {
    SECTION("Incomplete transfer (connection drops mid-stream) is discarded");

    Config cfg("test_tmp_step4c_lanbox.json");
    SyncManager sm(cfg);
    sm.startDaemon();
    sleepMs(300);

    SocketType sock = connectToTransferPort();
    CHECK("Connected to transfer port", sock != INVALID_SOCKET_VAL);

    SyncTransferHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.transfer_type = 0;
    sendAll(sock, &hdr, sizeof(hdr));

    string filename = "step4c_incomplete.txt";
    uint32_t nameLenNet = htonl((uint32_t)filename.size());
    sendAll(sock, &nameLenNet, sizeof(nameLenNet));
    sendAll(sock, filename.data(), filename.size());

    // Claim a large file size, but only send a few bytes then close
    uint64_t claimed_size = 1000000;
#ifdef _WIN32
    uint64_t sizeNet = _byteswap_uint64(claimed_size);
#else
    uint64_t sizeNet = htobe64(claimed_size);
#endif
    sendAll(sock, &sizeNet, sizeof(sizeNet));

    uint8_t is_encrypted = 0;
    sendAll(sock, &is_encrypted, sizeof(is_encrypted));
    sendAll(sock, "short", 5); // far less than claimed_size

    closesocket(sock); // abrupt close

    sleepMs(500);

    ifstream check(filename);
    CHECK("Incomplete file was NOT left on disk", !check.is_open());

    auto lines = readLastLogLines(20);
    CHECK("Incomplete transfer logged as discarded",
          logContains(lines, "Transfer incomplete for 'step4c_incomplete.txt'"));

    remove(filename.c_str()); // safety cleanup if it somehow exists
    sm.stopDaemon();
}

void test_stop_daemon_closes_transfer_socket_promptly() {
    SECTION("stopDaemon exits promptly with transfer loop running");

    Config cfg("test_tmp_step4c_lanbox.json");
    SyncManager sm(cfg);
    sm.startDaemon();
    sleepMs(300);

    auto start = chrono::steady_clock::now();
    sm.stopDaemon();
    auto elapsed = chrono::duration_cast<chrono::milliseconds>(
                       chrono::steady_clock::now() - start).count();

    CHECK("stopDaemon returns within 2000ms", elapsed < 2000);
}

// ============================================================================
// Cross-machine manual test mode
// ============================================================================
// Run as: ./test_step4c --listen
// Starts the daemon and blocks, printing dispatched transfer log lines live,
// until you press Enter. Use this on Machine A while Machine B sends a real
// file using the --send-file tool below.
void runListenMode() {
    cout << "=========================================\n";
    cout << "  LANBox Step 4c — Cross-Machine LISTENER \n";
    cout << "=========================================\n";

    Config cfg("test_tmp_step4c_listen.json");
    SyncManager sm(cfg);

    bool started = sm.startDaemon();
    if (!started) {
        cout << "Failed to start daemon (transfer socket bind failed?)\n";
        return;
    }

    cout << "Daemon started. Listening on TCP :5000.\n";
    cout << "Waiting for an incoming file transfer from another machine...\n";
    cout << "Press Enter to stop and exit.\n\n";

    string log_path = GroupManager::getLanboxDir() +
#ifdef _WIN32
        "\\sync.log";
#else
        "/sync.log";
#endif

    size_t last_size = 0;
    {
        ifstream f(log_path, ios::ate);
        if (f.is_open()) last_size = (size_t)f.tellg();
    }

    atomic<bool> stop_watching{false};
    thread watcher_thread([&]() {
        while (!stop_watching.load()) {
            sleepMs(300);
            ifstream f(log_path);
            if (!f.is_open()) continue;
            f.seekg(last_size);
            string line;
            while (getline(f, line)) {
                if (line.find("[LEGACY]") != string::npos ||
                    line.find("[SYNC-RECV]") != string::npos) {
                    cout << ">>> " << line << "\n";
                }
            }
            f.clear();
            f.seekg(0, ios::end);
            last_size = (size_t)f.tellg();
        }
    });

    cin.get();

    stop_watching.store(true);
    watcher_thread.join();
    sm.stopDaemon();
    cout << "Listener stopped.\n";
    cout << "Check the current directory for the received file.\n";
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2,2), &wsaData);
#endif

    if (argc > 1 && string(argv[1]) == "--listen") {
        runListenMode();
#ifdef _WIN32
        WSACleanup();
#endif
        return 0;
    }

    cout << "=========================================\n";
    cout << "  LANBox Step 4c — Transfer Loop Tests   \n";
    cout << "=========================================\n";

    test_transfer_socket_binds();
    test_legacy_transfer_unencrypted();
    test_multiple_concurrent_connections();
    test_group_sync_transfer_writes_temp_file();
    test_group_sync_unknown_group_rejected();
    test_incomplete_transfer_discarded();
    test_stop_daemon_closes_transfer_socket_promptly();

    cout << "\n=========================================\n";
    cout << "  Results: "
         << tests_passed << " passed, "
         << tests_failed << " failed, "
         << tests_run    << " total\n";
    cout << "=========================================\n";

    remove("test_tmp_step4c_lanbox.json");

#ifdef _WIN32
    WSACleanup();
#endif

    return (tests_failed == 0) ? 0 : 1;
}