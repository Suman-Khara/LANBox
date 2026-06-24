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
#include "protocol.hpp"

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

// Read the last N lines of the sync log so tests can verify dispatch happened
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
    if ((int)lines.size() > n) {
        lines.erase(lines.begin(), lines.end() - n);
    }
    return lines;
}

static bool logContains(const vector<string>& lines, const string& needle) {
    for (const auto& l : lines) {
        if (l.find(needle) != string::npos) return true;
    }
    return false;
}

// Send a raw UDP packet to localhost:5003 (simulates a peer)
static bool sendRawUdp(const vector<uint8_t>& msg) {
    SocketType sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == INVALID_SOCKET_VAL) return false;

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(SYNC_SIGNALING_PORT);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    int sent = sendto(sock, (const char*)msg.data(), (int)msg.size(), 0,
                      (sockaddr*)&addr, sizeof(addr));
    closesocket(sock);
    return sent == (int)msg.size();
}

// ============================================================================
// Tests
// ============================================================================

void test_signaling_socket_binds() {
    SECTION("Signaling socket binds and daemon starts cleanly");

    Config cfg("test_tmp_step4b_lanbox.json");
    SyncManager sm(cfg);

    bool started = sm.startDaemon();
    CHECK("startDaemon succeeds with signaling socket", started);

    sleepMs(300);

    auto lines = readLastLogLines(20);
    CHECK("Startup log mentions signaling port",
          logContains(lines, "Signaling loop listening on UDP :5003"));

    sm.stopDaemon();
}

void test_dispatch_sync_notify() {
    SECTION("SYNC_NOTIFY dispatch routing (loopback)");

    Config cfg("test_tmp_step4b_lanbox.json");
    SyncManager sm(cfg);
    sm.startDaemon();
    sleepMs(300);

    SyncNotifyPayload p;
    memset(&p, 0, sizeof(p));
    strncpy(p.group_id, "a3f5c2d8", 8);
    strncpy(p.filename, "dispatch_test.pdf", sizeof(p.filename) - 1);
    p.version   = 1;
    p.file_size = 1024;

    // Use a sender_ip that's NOT 127.0.0.1, since our own-IP filter in
    // dispatchSyncMessage only triggers if sender matches local_ip_ exactly,
    // which won't be set to loopback in normal operation. Using a fake
    // remote-looking source IP isn't possible with a real socket send
    // (kernel sets the source), so this test relies on local_ip_ being 0
    // (unset) in this minimal test, which means the self-filter is skipped.
    auto msg = Protocol::createSyncNotify(
        Protocol::ipToUint32("192.168.1.99"), 1, p);

    bool sent = sendRawUdp(msg);
    CHECK("Raw UDP packet sent", sent);

    sleepMs(500); // give the signaling loop time to receive + dispatch

    auto lines = readLastLogLines(20);
    CHECK("SYNC_NOTIFY dispatch logged",
          logContains(lines, "[DISPATCH] SYNC_NOTIFY"));
    CHECK("Dispatched filename matches",
          logContains(lines, "dispatch_test.pdf"));
    CHECK("Dispatched version matches",
          logContains(lines, "version=1"));

    sm.stopDaemon();
}

void test_dispatch_sync_request() {
    SECTION("SYNC_REQUEST dispatch routing (loopback)");

    Config cfg("test_tmp_step4b_lanbox.json");
    SyncManager sm(cfg);
    sm.startDaemon();
    sleepMs(300);

    SyncRequestPayload p;
    memset(&p, 0, sizeof(p));
    strncpy(p.group_id, "a3f5c2d8", 8);
    strncpy(p.filename, "needed_file.txt", sizeof(p.filename) - 1);
    p.have_version = 0;
    p.need_version = 1;

    auto msg = Protocol::createSyncRequest(
        Protocol::ipToUint32("192.168.1.99"), 2, p);
    sendRawUdp(msg);

    sleepMs(500);

    auto lines = readLastLogLines(20);
    CHECK("SYNC_REQUEST dispatch logged",
          logContains(lines, "[DISPATCH] SYNC_REQUEST"));
    CHECK("Dispatched filename matches",
          logContains(lines, "needed_file.txt"));

    sm.stopDaemon();
}

void test_dispatch_sync_meta_delta_variable_payload() {
    SECTION("SYNC_META_DELTA dispatch with variable JSON payload");

    Config cfg("test_tmp_step4b_lanbox.json");
    SyncManager sm(cfg);
    sm.startDaemon();
    sleepMs(300);

    string json_data = R"({"filename":"x.txt","version":2})";

    SyncMetaDeltaPayload fixed;
    memset(&fixed, 0, sizeof(fixed));
    strncpy(fixed.group_id,  "a3f5c2d8", 8);
    strncpy(fixed.sender_fp, "deadbeef", sizeof(fixed.sender_fp) - 1);
    fixed.is_full_metadata = 0;

    auto msg = Protocol::createSyncMetaDelta(
        Protocol::ipToUint32("192.168.1.99"), 3, fixed, json_data);
    sendRawUdp(msg);

    sleepMs(500);

    auto lines = readLastLogLines(20);
    CHECK("SYNC_META_DELTA dispatch logged",
          logContains(lines, "[DISPATCH] SYNC_META_DELTA"));
    CHECK("json_len matches payload size",
          logContains(lines, "json_len=" + to_string(json_data.size())));

    sm.stopDaemon();
}

void test_dispatch_group_join_request_variable_payload() {
    SECTION("GROUP_JOIN_REQUEST dispatch with variable pubkey payload");

    Config cfg("test_tmp_step4b_lanbox.json");
    SyncManager sm(cfg);
    sm.startDaemon();
    sleepMs(300);

    string fake_pubkey = "-----BEGIN PUBLIC KEY-----\nFAKEDATA\n-----END PUBLIC KEY-----\n";

    GroupJoinRequestPayload fixed;
    memset(&fixed, 0, sizeof(fixed));
    strncpy(fixed.group_id,       "a3f5c2d8", 8);
    strncpy(fixed.requester_name, "TestBob", sizeof(fixed.requester_name) - 1);
    strncpy(fixed.requester_fp,   "8a3f1b2c", sizeof(fixed.requester_fp) - 1);

    auto msg = Protocol::createGroupJoinRequest(
        Protocol::ipToUint32("192.168.1.99"), 4, fixed, fake_pubkey);
    sendRawUdp(msg);

    sleepMs(500);

    auto lines = readLastLogLines(20);
    CHECK("GROUP_JOIN_REQUEST dispatch logged",
          logContains(lines, "[DISPATCH] GROUP_JOIN_REQUEST"));
    CHECK("Requester name matches",
          logContains(lines, "TestBob"));

    sm.stopDaemon();
}

void test_corrupt_message_dropped_silently() {
    SECTION("Corrupt/tampered message is dropped without crashing");

    Config cfg("test_tmp_step4b_lanbox.json");
    SyncManager sm(cfg);
    sm.startDaemon();
    sleepMs(300);

    SyncNotifyPayload p;
    memset(&p, 0, sizeof(p));
    strncpy(p.filename, "tamper_test.txt", sizeof(p.filename) - 1);
    auto msg = Protocol::createSyncNotify(
        Protocol::ipToUint32("192.168.1.99"), 5, p);

    // Corrupt a payload byte without recalculating CRC
    msg[sizeof(LANBoxHeader) + 10] ^= 0xFF;

    bool sent = sendRawUdp(msg);
    CHECK("Corrupt packet sent without error", sent);

    sleepMs(500);

    auto lines = readLastLogLines(20);
    CHECK("Corrupt SYNC_NOTIFY NOT dispatched",
          !logContains(lines, "tamper_test.txt"));

    // Daemon should still be alive and responsive after corrupt packet
    CHECK("Daemon still running after corrupt packet", sm.isDaemonRunning());

    sm.stopDaemon();
}

void test_stop_daemon_closes_socket_promptly() {
    SECTION("stopDaemon exits promptly (signaling thread not stuck in recvfrom)");

    Config cfg("test_tmp_step4b_lanbox.json");
    SyncManager sm(cfg);
    sm.startDaemon();
    sleepMs(300);

    auto start = chrono::steady_clock::now();
    sm.stopDaemon();
    auto elapsed = chrono::duration_cast<chrono::milliseconds>(
                       chrono::steady_clock::now() - start).count();

    // With 200ms recv timeout, shutdown should happen within ~1s comfortably
    CHECK("stopDaemon returns within 1500ms", elapsed < 1500);
}

// ============================================================================
// Cross-machine manual test mode
// ============================================================================
// Run as: ./test_step4b --listen
// Starts the daemon and blocks, printing any dispatched sync messages live,
// until you press Enter. Use this on Machine A while Machine B sends a
// crafted packet using the --send-notify tool below.
void runListenMode() {
    cout << "=========================================\n";
    cout << "  LANBox Step 4b — Cross-Machine LISTENER \n";
    cout << "=========================================\n";

    Config cfg("test_tmp_step4b_listen.json");
    SyncManager sm(cfg);

    bool started = sm.startDaemon();
    if (!started) {
        cout << "Failed to start daemon (signaling socket bind failed?)\n";
        return;
    }

    cout << "Daemon started. Listening on UDP :5003.\n";
    cout << "Waiting for SYNC_NOTIFY from another machine...\n";
    cout << "Press Enter to stop and exit.\n\n";

    // Poll the log file and print new lines live
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
                if (line.find("[DISPATCH]") != string::npos) {
                    cout << ">>> " << line << "\n";
                }
            }
            f.clear();
            f.seekg(0, ios::end);
            last_size = (size_t)f.tellg();
        }
    });

    cin.get(); // wait for Enter

    stop_watching.store(true);
    watcher_thread.join();
    sm.stopDaemon();
    cout << "Listener stopped.\n";
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
    cout << "  LANBox Step 4b — Signaling Loop Tests  \n";
    cout << "=========================================\n";

    test_signaling_socket_binds();
    test_dispatch_sync_notify();
    test_dispatch_sync_request();
    test_dispatch_sync_meta_delta_variable_payload();
    test_dispatch_group_join_request_variable_payload();
    test_corrupt_message_dropped_silently();
    test_stop_daemon_closes_socket_promptly();

    cout << "\n=========================================\n";
    cout << "  Results: "
         << tests_passed << " passed, "
         << tests_failed << " failed, "
         << tests_run    << " total\n";
    cout << "=========================================\n";

    remove("test_tmp_step4b_lanbox.json");

#ifdef _WIN32
    WSACleanup();
#endif

    return (tests_failed == 0) ? 0 : 1;
}