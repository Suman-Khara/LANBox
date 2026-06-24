#include "platform.hpp"
#include "sync_manager.hpp"
#include "crypto.hpp"
#include "protocol.hpp"
#include <bits/stdc++.h>
#include <fstream>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #ifndef htonll
        #define htonll(x) ((1==htonl(1)) ? (x) : ((uint64_t)htonl((x) & 0xFFFFFFFF) << 32) | htonl((x) >> 32))
        #define ntohll(x) ((1==ntohl(1)) ? (x) : ((uint64_t)ntohl((x) & 0xFFFFFFFF) << 32) | ntohl((x) >> 32))
    #endif
#else
    #include <sys/socket.h>
    #include <arpa/inet.h>
    #include <netinet/in.h>
    #include <unistd.h>
    #include <fcntl.h>
    #ifndef htonll
        #define htonll(x) htobe64(x)
        #define ntohll(x) be64toh(x)
    #endif
#endif

#ifndef _WIN32
    #include <sys/select.h>
#endif

using namespace std;
using json = nlohmann::json;

// ============================================================================
// PersistedState JSON
// ============================================================================

void to_json(json& j, const PersistedState& s) {
    j = json{
        {"sync_active",       s.sync_active},
        {"daemon_started_at", s.daemon_started_at}
    };
}

void from_json(const json& j, PersistedState& s) {
    s.sync_active       = j.value("sync_active", false);
    s.daemon_started_at = j.value("daemon_started_at", 0L);
}

// ============================================================================
// Construction / destruction
// ============================================================================

SyncManager::SyncManager(Config& cfg) : cfg_(cfg) {
    device_name_ = "lanbox-device"; // overwritten in startDaemon() with real hostname
    local_ip_    = 0;
}

SyncManager::~SyncManager() {
    if (daemon_running_.load()) {
        stopDaemon();
    }
}

// ============================================================================
// Helpers
// ============================================================================

long SyncManager::nowSeconds() const {
    return static_cast<long>(time(nullptr));
}

string SyncManager::myFingerprint() const {
    try {
        return Crypto::getPublicKeyFingerprint();
    } catch (...) {
        return "";
    }
}

string SyncManager::myName() const {
    return device_name_;
}

string SyncManager::resolveGroupForPath(const string& abs_path) const {
    lock_guard<mutex> lock(groups_mutex_);
    for (const auto& g : known_groups_) {
        string root = g.folder_path;
        while (!root.empty() && (root.back() == '/' || root.back() == '\\'))
            root.pop_back();

        if (abs_path.size() > root.size() &&
            abs_path.substr(0, root.size()) == root &&
            (abs_path[root.size()] == '/' || abs_path[root.size()] == '\\')) {
            return g.group_id;
        }
        if (abs_path == root) return g.group_id;
    }
    return "";
}

void SyncManager::refreshGroupList() {
    auto groups = GroupManager::listAllGroups();
    lock_guard<mutex> lock(groups_mutex_);
    known_groups_ = groups;
}

void SyncManager::logSync(const string& message) {
    // Full rotating-log implementation comes in Step 4m.
    // For 4a, write directly to ~/.lanbox/sync.log so we have visibility
    // while building/testing earlier steps.
    string path = GroupManager::getLanboxDir() +
#ifdef _WIN32
        "\\" + SYNC_LOG_FILENAME;
#else
        "/" + SYNC_LOG_FILENAME;
#endif
    ofstream f(path, ios::app);
    if (!f.is_open()) return;

    time_t now = time(nullptr);
    char timebuf[32];
    struct tm* tm_info = localtime(&now);
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", tm_info);

    f << timebuf << "  " << message << "\n";
}

// ============================================================================
// State persistence
// ============================================================================

string SyncManager::statePath() const {
    return GroupManager::getLanboxDir() +
#ifdef _WIN32
        "\\" + STATE_FILENAME;
#else
        "/" + STATE_FILENAME;
#endif
}

bool SyncManager::loadState(PersistedState& out) const {
    string path = statePath();
    ifstream f(path);
    if (!f.is_open()) return false;
    try {
        json j; f >> j;
        out = j.get<PersistedState>();
        return true;
    } catch (...) {
        return false;
    }
}

bool SyncManager::saveState(const PersistedState& s) const {
    GroupManager::ensureLanboxDirs();
    string path = statePath();
    string tmp  = path + ".tmp";

    ofstream f(tmp);
    if (!f.is_open()) return false;
    f << json(s).dump(4);
    f.close();

#ifdef _WIN32
    DeleteFileA(path.c_str());
    return MoveFileA(tmp.c_str(), path.c_str()) != 0;
#else
    return rename(tmp.c_str(), path.c_str()) == 0;
#endif
}

// ============================================================================
// Daemon lifecycle
// ============================================================================

bool SyncManager::startDaemon() {
    if (daemon_running_.load()) return false;

    GroupManager::ensureLanboxDirs();

    // Determine device name (hostname)
#ifdef _WIN32
    char buf[256];
    DWORD len = sizeof(buf);
    if (GetComputerNameA(buf, &len)) device_name_ = string(buf, len);
#else
    char buf[256];
    if (gethostname(buf, sizeof(buf)) == 0) device_name_ = string(buf);
#endif

    refreshGroupList();

    stop_requested_.store(false);
    daemon_running_.store(true);

    // Start always-on threads (independent of sync state)
    signaling_thread_   = thread(&SyncManager::signalingLoop, this);
    transfer_thread_     = thread(&SyncManager::transferLoop, this);
    ipc_thread_          = thread(&SyncManager::ipcLoop, this);
    maintenance_thread_ = thread(&SyncManager::maintenanceLoop, this);

    logSync("[STARTUP] LANBox daemon started");

    // Restore sync state from last session
    PersistedState saved;
    if (loadState(saved) && saved.sync_active) {
        startSync();
    } else {
        sync_state_.store(SyncState::PAUSED);
    }

    return true;
}

void SyncManager::stopDaemon() {
    if (!daemon_running_.load()) return;

    // If sync is active, perform an inline soft pause first.
    if (sync_state_.load() == SyncState::ACTIVE) {
        pauseSync(false); // soft pause
    }

    stop_requested_.store(true);

    if (signaling_thread_.joinable())   signaling_thread_.join();
    if (transfer_thread_.joinable())     transfer_thread_.join();
    if (ipc_thread_.joinable())          ipc_thread_.join();
    if (maintenance_thread_.joinable()) maintenance_thread_.join();

    daemon_running_.store(false);

    PersistedState s;
    s.sync_active       = (sync_state_.load() == SyncState::ACTIVE);
    s.daemon_started_at = 0;
    saveState(s);

    logSync("[SHUTDOWN] LANBox daemon stopped");
}

// ============================================================================
// Sync engine control
// ============================================================================

bool SyncManager::startSync() {
    if (sync_state_.load() == SyncState::ACTIVE) return true; // already active

    refreshGroupList();

    // Start the watcher if it isn't running yet
    if (!watcher_.isRunning()) {
        watcher_.start(
            [this](const WatchEvent& e) { watcherCallback(e); },
            5 // default max_depth — per-group override comes when we wire
              // GroupSettings::max_subdirectory_depth into watcher add calls
        );
    }

    // Register all known group folders with the watcher
    {
        lock_guard<mutex> lock(groups_mutex_);
        for (const auto& g : known_groups_) {
            watcher_.addWatchPath(g.folder_path);
        }
    }

    sync_state_.store(SyncState::ACTIVE);

    PersistedState s;
    s.sync_active = true;
    saveState(s);

    logSync("[SYNC] Sync started/resumed");

    // Local diff + remote catch-up implemented in Step 4j.
    // For 4a this is a stub — sync state flips on, watcher runs, that's it.

    return true;
}

bool SyncManager::pauseSync(bool hard) {
    if (sync_state_.load() == SyncState::PAUSED) return true; // already paused

    // Full soft/hard pause handshake (SYNC_PAUSE_NOTIFY/ACK, transfer
    // draining, rollback) implemented in Step 4k.
    // For 4a: just stop the watcher and flip state, so the skeleton is testable.

    watcher_.stop();

    sync_state_.store(SyncState::PAUSED);

    PersistedState s;
    s.sync_active = false;
    saveState(s);

    logSync(hard ? "[SYNC] Sync paused (hard)" : "[SYNC] Sync paused (soft)");

    return true;
}

// ============================================================================
// Status
// ============================================================================

string SyncManager::getStatusJson() const {
    json j;
    j["daemon_running"] = daemon_running_.load();
    j["sync_state"]      = (sync_state_.load() == SyncState::ACTIVE) ? "active" : "paused";

    lock_guard<mutex> lock(groups_mutex_);
    j["group_count"] = known_groups_.size();

    return j.dump(4);
}

// ============================================================================
// Thread loop stubs (filled in across Steps 4b–4n)
// ============================================================================

// ============================================================================
// Signaling socket setup
// ============================================================================

bool SyncManager::initSignalingSocket() {
    signaling_socket_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (signaling_socket_ == INVALID_SOCKET_VAL) {
        cerr << "[SyncManager] Failed to create signaling socket\n";
        return false;
    }

    // Allow address reuse (helps with quick restarts during testing)
    int reuse = 1;
    setsockopt(signaling_socket_, SOL_SOCKET, SO_REUSEADDR,
              (const char*)&reuse, sizeof(reuse));

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(SYNC_SIGNALING_PORT);

    if (bind(signaling_socket_, (sockaddr*)&addr, sizeof(addr)) < 0) {
        cerr << "[SyncManager] Failed to bind signaling socket on port "
             << SYNC_SIGNALING_PORT << "\n";
        closesocket(signaling_socket_);
        signaling_socket_ = INVALID_SOCKET_VAL;
        return false;
    }

    // Enable broadcast (needed for SYNC_NOTIFY, SYNC_REQUEST, SYNC_META_REQUEST)
    int broadcast_enable = 1;
    setsockopt(signaling_socket_, SOL_SOCKET, SO_BROADCAST,
              (const char*)&broadcast_enable, sizeof(broadcast_enable));

    // Set a receive timeout so the loop can periodically check stop_requested_
    // even with no traffic arriving.
#ifdef _WIN32
    DWORD timeout_ms = 200;
    setsockopt(signaling_socket_, SOL_SOCKET, SO_RCVTIMEO,
              (const char*)&timeout_ms, sizeof(timeout_ms));
#else
    struct timeval tv;
    tv.tv_sec  = 0;
    tv.tv_usec = 200000; // 200ms
    setsockopt(signaling_socket_, SOL_SOCKET, SO_RCVTIMEO,
              (const char*)&tv, sizeof(tv));
#endif

    return true;
}

void SyncManager::closeSignalingSocket() {
    if (signaling_socket_ != INVALID_SOCKET_VAL) {
        closesocket(signaling_socket_);
        signaling_socket_ = INVALID_SOCKET_VAL;
    }
}

// ============================================================================
// Signaling loop
// ============================================================================

void SyncManager::signalingLoop() {
    if (!initSignalingSocket()) {
        cerr << "[SyncManager] Signaling loop could not start — socket init failed\n";
        return;
    }

    logSync("[STARTUP] Signaling loop listening on UDP :" +
            to_string(SYNC_SIGNALING_PORT));

    vector<uint8_t> buffer(MAX_PAYLOAD_SIZE + HEADER_SIZE);

    while (!stop_requested_.load()) {
        sockaddr_in sender_addr;
        memset(&sender_addr, 0, sizeof(sender_addr));
#ifdef _WIN32
        int addr_len = sizeof(sender_addr);
#else
        socklen_t addr_len = sizeof(sender_addr);
#endif

        int bytes = recvfrom(signaling_socket_, (char*)buffer.data(),
                             (int)buffer.size(), 0,
                             (sockaddr*)&sender_addr, &addr_len);

        if (bytes <= 0) {
            // Timeout or error — loop back and check stop_requested_
            continue;
        }

        if (bytes < (int)HEADER_SIZE) continue; // too short to be valid

        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &sender_addr.sin_addr, ip_str, sizeof(ip_str));
        string sender_ip(ip_str);

        // Validate CRC before parsing
        if (!Protocol::validateMessage(buffer.data(), bytes)) {
            continue; // corrupt/tampered message, silently drop
        }

        LANBoxHeader header;
        vector<uint8_t> payload;
        if (!Protocol::parseMessage(buffer.data(), bytes, header, payload)) {
            continue;
        }

        dispatchSyncMessage(header, payload, sender_ip);
    }

    closeSignalingSocket();
}

// ============================================================================
// Dispatch
// ============================================================================

void SyncManager::dispatchSyncMessage(const LANBoxHeader& header,
                                      const vector<uint8_t>& payload,
                                      const string& sender_ip) {
    MessageType type = static_cast<MessageType>(ntohs(header.message_type));

    // Ignore our own broadcasts (sent and received on same machine, e.g.
    // 255.255.255.255 broadcast looping back, or same-IP testing)
    string my_ip = Protocol::uint32ToIp(local_ip_);
    if (sender_ip == my_ip && local_ip_ != 0) {
        return;
    }

    switch (type) {
        case MessageType::SYNC_NOTIFY: {
            if (payload.size() < sizeof(SyncNotifyPayload)) return;
            SyncNotifyPayload p;
            memcpy(&p, payload.data(), sizeof(p));
            p.version   = ntohl(p.version);
            p.file_size = ntohll(p.file_size);
            onSyncNotify(p, sender_ip);
            break;
        }
        case MessageType::SYNC_REQUEST: {
            if (payload.size() < sizeof(SyncRequestPayload)) return;
            SyncRequestPayload p;
            memcpy(&p, payload.data(), sizeof(p));
            p.have_version = ntohl(p.have_version);
            p.need_version = ntohl(p.need_version);
            onSyncRequest(p, sender_ip);
            break;
        }
        case MessageType::SYNC_OFFER: {
            if (payload.size() < sizeof(SyncOfferPayload)) return;
            SyncOfferPayload p;
            memcpy(&p, payload.data(), sizeof(p));
            p.version          = ntohl(p.version);
            p.sender_tcp_port  = ntohs(p.sender_tcp_port);
            onSyncOffer(p, sender_ip);
            break;
        }
        case MessageType::SYNC_OFFER_ACCEPT: {
            if (payload.size() < sizeof(SyncOfferAcceptPayload)) return;
            SyncOfferAcceptPayload p;
            memcpy(&p, payload.data(), sizeof(p));
            p.version = ntohl(p.version);
            onSyncOfferAccept(p, sender_ip);
            break;
        }
        case MessageType::SYNC_DECLINE: {
            if (payload.size() < sizeof(SyncDeclinePayload)) return;
            SyncDeclinePayload p;
            memcpy(&p, payload.data(), sizeof(p));
            p.version = ntohl(p.version);
            onSyncDecline(p, sender_ip);
            break;
        }
        case MessageType::SYNC_DELETE_NOTIFY: {
            if (payload.size() < sizeof(SyncDeletePayload)) return;
            SyncDeletePayload p;
            memcpy(&p, payload.data(), sizeof(p));
            p.deleted_at          = ntohll(p.deleted_at);
            p.version_at_deletion = ntohl(p.version_at_deletion);
            onSyncDeleteNotify(p, sender_ip);
            break;
        }
        case MessageType::SYNC_RENAME_NOTIFY: {
            if (payload.size() < sizeof(SyncRenamePayload)) return;
            SyncRenamePayload p;
            memcpy(&p, payload.data(), sizeof(p));
            p.new_version = ntohl(p.new_version);
            p.renamed_at  = ntohll(p.renamed_at);
            onSyncRenameNotify(p, sender_ip);
            break;
        }
        case MessageType::SYNC_META_REQUEST: {
            if (payload.size() < sizeof(SyncMetaRequestPayload)) return;
            SyncMetaRequestPayload p;
            memcpy(&p, payload.data(), sizeof(p));
            onSyncMetaRequest(p, sender_ip);
            break;
        }
        case MessageType::SYNC_META_DELTA: {
            if (payload.size() < sizeof(SyncMetaDeltaPayload)) return;
            SyncMetaDeltaPayload fixed;
            memcpy(&fixed, payload.data(), sizeof(fixed));
            uint32_t json_len = ntohl(fixed.json_length);
            if (payload.size() < sizeof(SyncMetaDeltaPayload) + json_len) return;
            string json_data(
                reinterpret_cast<const char*>(payload.data() + sizeof(SyncMetaDeltaPayload)),
                json_len
            );
            onSyncMetaDelta(fixed, json_data, sender_ip);
            break;
        }
        case MessageType::SYNC_PAUSE_NOTIFY: {
            if (payload.size() < sizeof(SyncPauseNotifyPayload)) return;
            SyncPauseNotifyPayload p;
            memcpy(&p, payload.data(), sizeof(p));
            onSyncPauseNotify(p, sender_ip);
            break;
        }
        case MessageType::SYNC_PAUSE_ACK: {
            if (payload.size() < sizeof(SyncPauseAckPayload)) return;
            SyncPauseAckPayload p;
            memcpy(&p, payload.data(), sizeof(p));
            onSyncPauseAck(p, sender_ip);
            break;
        }
        case MessageType::SYNC_RESUME_NOTIFY: {
            if (payload.size() < sizeof(SyncResumeNotifyPayload)) return;
            SyncResumeNotifyPayload p;
            memcpy(&p, payload.data(), sizeof(p));
            onSyncResumeNotify(p, sender_ip);
            break;
        }
        case MessageType::GROUP_INVITE: {
            if (payload.size() < sizeof(GroupInvitePayload)) return;
            GroupInvitePayload fixed;
            memcpy(&fixed, payload.data(), sizeof(fixed));
            uint32_t json_len = ntohl(fixed.group_json_length);
            if (payload.size() < sizeof(GroupInvitePayload) + json_len) return;
            string group_json(
                reinterpret_cast<const char*>(payload.data() + sizeof(GroupInvitePayload)),
                json_len
            );
            onGroupInvite(fixed, group_json, sender_ip);
            break;
        }
        case MessageType::GROUP_INVITE_ACK: {
            if (payload.size() < sizeof(GroupInviteAckPayload)) return;
            GroupInviteAckPayload p;
            memcpy(&p, payload.data(), sizeof(p));
            onGroupInviteAck(p, sender_ip);
            break;
        }
        case MessageType::GROUP_JOIN_REQUEST: {
            if (payload.size() < sizeof(GroupJoinRequestPayload)) return;
            GroupJoinRequestPayload fixed;
            memcpy(&fixed, payload.data(), sizeof(fixed));
            uint32_t pk_len = ntohl(fixed.pubkey_length);
            if (payload.size() < sizeof(GroupJoinRequestPayload) + pk_len) return;
            string pubkey(
                reinterpret_cast<const char*>(payload.data() + sizeof(GroupJoinRequestPayload)),
                pk_len
            );
            onGroupJoinRequest(fixed, pubkey, sender_ip);
            break;
        }
        case MessageType::GROUP_JOIN_APPROVE: {
            if (payload.size() < sizeof(GroupJoinApprovePayload)) return;
            GroupJoinApprovePayload fixed;
            memcpy(&fixed, payload.data(), sizeof(fixed));
            uint32_t json_len = ntohl(fixed.group_json_length);
            if (payload.size() < sizeof(GroupJoinApprovePayload) + json_len) return;
            string group_json(
                reinterpret_cast<const char*>(payload.data() + sizeof(GroupJoinApprovePayload)),
                json_len
            );
            onGroupJoinApprove(fixed, group_json, sender_ip);
            break;
        }
        case MessageType::GROUP_JOIN_DENY: {
            if (payload.size() < sizeof(GroupJoinDenyPayload)) return;
            GroupJoinDenyPayload p;
            memcpy(&p, payload.data(), sizeof(p));
            onGroupJoinDeny(p, sender_ip);
            break;
        }
        case MessageType::GROUP_KICK: {
            if (payload.size() < sizeof(GroupKickPayload)) return;
            GroupKickPayload p;
            memcpy(&p, payload.data(), sizeof(p));
            onGroupKick(p, sender_ip);
            break;
        }
        default:
            // Not a Phase 4 message type (could be DISCOVERY_*/HEARTBEAT if
            // somehow received here, or unknown) — ignore.
            break;
    }
}

// ============================================================================
// Handler stubs — full implementations arrive in Steps 4e through 4l.
// For 4b, each stub just logs that dispatch correctly reached it, so we can
// verify end-to-end routing across two real machines before building behavior.
// ============================================================================

void SyncManager::onSyncNotify(const SyncNotifyPayload& p, const string& sender_ip) {
    logSync("[DISPATCH] SYNC_NOTIFY from " + sender_ip +
           " file=" + string(p.filename) + " version=" + to_string(p.version));
}

void SyncManager::onSyncRequest(const SyncRequestPayload& p, const string& sender_ip) {
    logSync("[DISPATCH] SYNC_REQUEST from " + sender_ip +
           " file=" + string(p.filename));
}

void SyncManager::onSyncOffer(const SyncOfferPayload& p, const string& sender_ip) {
    logSync("[DISPATCH] SYNC_OFFER from " + sender_ip +
           " file=" + string(p.filename));
}

void SyncManager::onSyncOfferAccept(const SyncOfferAcceptPayload& p, const string& sender_ip) {
    logSync("[DISPATCH] SYNC_OFFER_ACCEPT from " + sender_ip +
           " file=" + string(p.filename));
}

void SyncManager::onSyncDecline(const SyncDeclinePayload& p, const string& sender_ip) {
    logSync("[DISPATCH] SYNC_DECLINE from " + sender_ip +
           " file=" + string(p.filename));
}

void SyncManager::onSyncDeleteNotify(const SyncDeletePayload& p, const string& sender_ip) {
    logSync("[DISPATCH] SYNC_DELETE_NOTIFY from " + sender_ip +
           " file=" + string(p.filename));
}

void SyncManager::onSyncRenameNotify(const SyncRenamePayload& p, const string& sender_ip) {
    logSync("[DISPATCH] SYNC_RENAME_NOTIFY from " + sender_ip +
           " " + string(p.old_name) + " -> " + string(p.new_name));
}

void SyncManager::onSyncMetaRequest(const SyncMetaRequestPayload& p, const string& sender_ip) {
    logSync("[DISPATCH] SYNC_META_REQUEST from " + sender_ip);
}

void SyncManager::onSyncMetaDelta(const SyncMetaDeltaPayload& fixed,
                                  const string& json_data, const string& sender_ip) {
    logSync("[DISPATCH] SYNC_META_DELTA from " + sender_ip +
           " json_len=" + to_string(json_data.size()));
}

void SyncManager::onSyncPauseNotify(const SyncPauseNotifyPayload& p, const string& sender_ip) {
    logSync("[DISPATCH] SYNC_PAUSE_NOTIFY from " + sender_ip);
}

void SyncManager::onSyncPauseAck(const SyncPauseAckPayload& p, const string& sender_ip) {
    logSync("[DISPATCH] SYNC_PAUSE_ACK from " + sender_ip);
}

void SyncManager::onSyncResumeNotify(const SyncResumeNotifyPayload& p, const string& sender_ip) {
    logSync("[DISPATCH] SYNC_RESUME_NOTIFY from " + sender_ip);
}

void SyncManager::onGroupInvite(const GroupInvitePayload& fixed,
                                const string& group_json, const string& sender_ip) {
    logSync("[DISPATCH] GROUP_INVITE from " + sender_ip +
           " group=" + string(fixed.group_name));
}

void SyncManager::onGroupInviteAck(const GroupInviteAckPayload& p, const string& sender_ip) {
    logSync("[DISPATCH] GROUP_INVITE_ACK from " + sender_ip);
}

void SyncManager::onGroupJoinRequest(const GroupJoinRequestPayload& fixed,
                                     const string& pubkey_pem, const string& sender_ip) {
    logSync("[DISPATCH] GROUP_JOIN_REQUEST from " + sender_ip +
           " requester=" + string(fixed.requester_name));
}

void SyncManager::onGroupJoinApprove(const GroupJoinApprovePayload& fixed,
                                     const string& group_json, const string& sender_ip) {
    logSync("[DISPATCH] GROUP_JOIN_APPROVE from " + sender_ip);
}

void SyncManager::onGroupJoinDeny(const GroupJoinDenyPayload& p, const string& sender_ip) {
    logSync("[DISPATCH] GROUP_JOIN_DENY from " + sender_ip +
           " reason=" + string(p.reason));
}

void SyncManager::onGroupKick(const GroupKickPayload& p, const string& sender_ip) {
    logSync("[DISPATCH] GROUP_KICK from " + sender_ip);
}

// ============================================================================
// Transfer socket setup
// ============================================================================

bool SyncManager::initTransferSocket() {
    transfer_listen_socket_ = socket(AF_INET, SOCK_STREAM, 0);
    if (transfer_listen_socket_ == INVALID_SOCKET_VAL) {
        cerr << "[SyncManager] Failed to create transfer socket\n";
        return false;
    }

    int reuse = 1;
    setsockopt(transfer_listen_socket_, SOL_SOCKET, SO_REUSEADDR,
              (const char*)&reuse, sizeof(reuse));

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(SYNC_TRANSFER_PORT);

    if (bind(transfer_listen_socket_, (sockaddr*)&addr, sizeof(addr)) < 0) {
        cerr << "[SyncManager] Failed to bind transfer socket on port "
             << SYNC_TRANSFER_PORT << "\n";
        closesocket(transfer_listen_socket_);
        transfer_listen_socket_ = INVALID_SOCKET_VAL;
        return false;
    }

    if (listen(transfer_listen_socket_, 16) < 0) {
        cerr << "[SyncManager] Failed to listen on transfer socket\n";
        closesocket(transfer_listen_socket_);
        transfer_listen_socket_ = INVALID_SOCKET_VAL;
        return false;
    }

    // No SO_RCVTIMEO here — accept() timeout is handled via select() in
    // transferLoop() instead, since SO_RCVTIMEO on a listening socket's
    // accept() is unreliable on Windows.

    return true;
}

void SyncManager::closeTransferSocket() {
    if (transfer_listen_socket_ != INVALID_SOCKET_VAL) {
        closesocket(transfer_listen_socket_);
        transfer_listen_socket_ = INVALID_SOCKET_VAL;
    }
}

void SyncManager::reapFinishedTransferThreads() {
    lock_guard<mutex> lock(transfer_threads_mutex_);
    // We can't easily check "is this thread done" without joining, and
    // joining blocks. Since each handler thread is short-lived and detached
    // logically (we don't need their return value), we just join everything
    // accumulated so far whenever this is called from a safe point (stop).
    // During normal operation the vector simply grows; cleanup happens at
    // stopDaemon(). This avoids needing a more complex thread-pool here.
}

// ============================================================================
// Transfer loop — accept() loop, dispatches each connection to its own thread
// ============================================================================

void SyncManager::transferLoop() {
    if (!initTransferSocket()) {
        cerr << "[SyncManager] Transfer loop could not start — socket init failed\n";
        return;
    }

    logSync("[STARTUP] Transfer loop listening on TCP :" +
            to_string(SYNC_TRANSFER_PORT));

    while (!stop_requested_.load()) {
        // Use select() to wait up to 200ms for an incoming connection.
        // This gives reliable, cross-platform timeout behavior, unlike
        // SO_RCVTIMEO applied to accept() on Windows.
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(transfer_listen_socket_, &read_fds);

        struct timeval tv;
        tv.tv_sec  = 0;
        tv.tv_usec = 200000; // 200ms

        int sel = select((int)transfer_listen_socket_ + 1, &read_fds, nullptr, nullptr, &tv);

        if (sel <= 0) {
            // Timeout (sel == 0) or error (sel < 0) — loop back and
            // check stop_requested_.
            continue;
        }

        if (!FD_ISSET(transfer_listen_socket_, &read_fds)) {
            continue;
        }

        sockaddr_in client_addr;
        memset(&client_addr, 0, sizeof(client_addr));
#ifdef _WIN32
        int addr_len = sizeof(client_addr);
#else
        socklen_t addr_len = sizeof(client_addr);
#endif

        SocketType client_sock = accept(transfer_listen_socket_,
                                        (sockaddr*)&client_addr, &addr_len);

        if (client_sock == INVALID_SOCKET_VAL) {
            continue; // spurious wakeup or error, try again
        }

        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, sizeof(ip_str));
        string client_ip(ip_str);

        {
            lock_guard<mutex> lock(transfer_threads_mutex_);
            transfer_threads_.emplace_back(
                &SyncManager::handleIncomingConnection, this, client_sock, client_ip);
        }
    }

    closeTransferSocket();

    {
        lock_guard<mutex> lock(transfer_threads_mutex_);
        for (auto& t : transfer_threads_) {
            if (t.joinable()) t.join();
        }
        transfer_threads_.clear();
    }
}

// ============================================================================
// Per-connection dispatch — reads SyncTransferHeader, branches on type
// ============================================================================

void SyncManager::handleIncomingConnection(SocketType client_sock, string client_ip) {
    SyncTransferHeader header;
    memset(&header, 0, sizeof(header));

    int received = 0;
    while (received < (int)sizeof(header)) {
        int n = recv(client_sock,
                    reinterpret_cast<char*>(&header) + received,
                    (int)sizeof(header) - received, 0);
        if (n <= 0) {
            // Connection closed before header arrived — nothing to do
            closesocket(client_sock);
            return;
        }
        received += n;
    }

    if (header.transfer_type == 0) {
        receiveLegacyTransfer(client_sock, client_ip);
    } else if (header.transfer_type == 1) {
        string group_id(header.group_id, strnlen(header.group_id, sizeof(header.group_id)));
        string rel_path(header.relative_path,
                        strnlen(header.relative_path, sizeof(header.relative_path)));
        receiveGroupSyncTransfer(client_sock, group_id, rel_path, client_ip);
    } else {
        cerr << "[SyncManager] Unknown transfer_type " << (int)header.transfer_type
             << " from " << client_ip << " — closing connection\n";
        closesocket(client_sock);
    }
}

// ============================================================================
// Legacy transfer path (transfer_type == 0)
// Adapted from the original single-shot receiveFile() in main.cpp, now
// running per-connection inside the persistent server.
// ============================================================================

void SyncManager::receiveLegacyTransfer(SocketType client_sock, const string& client_ip) {
    auto recvAll = [&](void* buf, size_t len) -> bool {
        size_t got = 0;
        while (got < len) {
            int n = recv(client_sock, (char*)buf + got, (int)(len - got), 0);
            if (n <= 0) return false;
            got += n;
        }
        return true;
    };

    uint32_t nameLenNet;
    if (!recvAll(&nameLenNet, sizeof(nameLenNet))) { closesocket(client_sock); return; }
    uint32_t nameLen = ntohl(nameLenNet);
    if (nameLen == 0 || nameLen > 4096) { closesocket(client_sock); return; }

    string filename(nameLen, '\0');
    if (!recvAll(&filename[0], nameLen)) { closesocket(client_sock); return; }

    uint64_t sizeNet;
    if (!recvAll(&sizeNet, sizeof(sizeNet))) { closesocket(client_sock); return; }
#if defined(_WIN32)
    uint64_t fileSize = _byteswap_uint64(sizeNet);
#else
    uint64_t fileSize = be64toh(sizeNet);
#endif

    uint8_t is_encrypted;
    if (!recvAll(&is_encrypted, sizeof(is_encrypted))) { closesocket(client_sock); return; }

    vector<unsigned char> encrypted_aes_key;
    vector<unsigned char> iv;

    if (is_encrypted) {
        uint32_t key_len_net;
        if (!recvAll(&key_len_net, sizeof(key_len_net))) { closesocket(client_sock); return; }
        uint32_t key_len = ntohl(key_len_net);
        if (key_len == 0 || key_len > 4096) { closesocket(client_sock); return; }

        encrypted_aes_key.resize(key_len);
        if (!recvAll(encrypted_aes_key.data(), key_len)) { closesocket(client_sock); return; }

        iv.resize(16);
        if (!recvAll(iv.data(), 16)) { closesocket(client_sock); return; }
    }

    logSync("[LEGACY] Receiving '" + filename + "' (" + to_string(fileSize) +
           " bytes) from " + client_ip + (is_encrypted ? " [encrypted]" : ""));

    string temp_file = is_encrypted
                       ? filename.substr(0, filename.find_last_of('.')) + "_temp_enc" +
                         (filename.find_last_of('.') != string::npos
                          ? filename.substr(filename.find_last_of('.')) : "")
                       : filename;

    ofstream outfile(temp_file, ios::binary);
    if (!outfile.is_open()) {
        cerr << "[SyncManager] Cannot open output file: " << temp_file << "\n";
        closesocket(client_sock);
        return;
    }

    vector<char> buffer(TRANSFER_BUFFER_SIZE);
    uint64_t received_bytes = 0;

    while (received_bytes < fileSize) {
        int n = recv(client_sock, buffer.data(),
                    (int)min((uint64_t)TRANSFER_BUFFER_SIZE, fileSize - received_bytes), 0);
        if (n <= 0) break;
        outfile.write(buffer.data(), n);
        received_bytes += n;
    }
    outfile.close();
    closesocket(client_sock);

    if (received_bytes != fileSize) {
        logSync("[LEGACY] Transfer incomplete for '" + filename + "' from " +
                client_ip + " (" + to_string(received_bytes) + "/" +
                to_string(fileSize) + " bytes) — discarding");
        remove(temp_file.c_str());
        return;
    }

    if (is_encrypted) {
        try {
            if (Crypto::decryptFile(temp_file, filename, encrypted_aes_key, iv)) {
                remove(temp_file.c_str());
                logSync("[LEGACY] Received and decrypted '" + filename +
                       "' from " + client_ip);
            } else {
                logSync("[LEGACY] Decryption FAILED for '" + filename +
                       "' from " + client_ip);
            }
        } catch (const exception& e) {
            logSync("[LEGACY] Decryption error for '" + filename + "': " + e.what());
        }
    } else {
        logSync("[LEGACY] Received '" + filename + "' from " + client_ip);
    }
}

// ============================================================================
// Group sync transfer path (transfer_type == 1)
// For Step 4c: writes to .lanbox_recv_tmp_<filename> inside the group folder
// and stops there. Rename-on-completion, metadata update, and pending_ops
// retry-on-lock logic are added in Step 4g (the receive side of the offer/
// accept protocol), since that's the step that actually triggers transfers
// this way — 4c only proves the server can correctly route and write bytes.
// ============================================================================

void SyncManager::receiveGroupSyncTransfer(SocketType client_sock,
                                           const string& group_id,
                                           const string& relative_path,
                                           const string& client_ip) {
    auto recvAll = [&](void* buf, size_t len) -> bool {
        size_t got = 0;
        while (got < len) {
            int n = recv(client_sock, (char*)buf + got, (int)(len - got), 0);
            if (n <= 0) return false;
            got += n;
        }
        return true;
    };

    // Resolve group folder from our known group list
    string folder_path;
    {
        lock_guard<mutex> lock(groups_mutex_);
        for (const auto& g : known_groups_) {
            if (g.group_id == group_id) { folder_path = g.folder_path; break; }
        }
    }

    if (folder_path.empty()) {
        logSync("[SYNC-RECV] Unknown group_id '" + group_id + "' from " +
               client_ip + " — rejecting transfer");
        closesocket(client_sock);
        return;
    }

    // Same wire format as legacy from this point: nameLen+name, size,
    // is_encrypted, [key+iv], data. Filename here should match relative_path
    // but we read it anyway since the wire format always sends it.
    uint32_t nameLenNet;
    if (!recvAll(&nameLenNet, sizeof(nameLenNet))) { closesocket(client_sock); return; }
    uint32_t nameLen = ntohl(nameLenNet);
    if (nameLen == 0 || nameLen > 4096) { closesocket(client_sock); return; }

    string wire_filename(nameLen, '\0');
    if (!recvAll(&wire_filename[0], nameLen)) { closesocket(client_sock); return; }

    uint64_t sizeNet;
    if (!recvAll(&sizeNet, sizeof(sizeNet))) { closesocket(client_sock); return; }
#if defined(_WIN32)
    uint64_t fileSize = _byteswap_uint64(sizeNet);
#else
    uint64_t fileSize = be64toh(sizeNet);
#endif

    uint8_t is_encrypted;
    if (!recvAll(&is_encrypted, sizeof(is_encrypted))) { closesocket(client_sock); return; }

    vector<unsigned char> encrypted_aes_key;
    vector<unsigned char> iv;
    if (is_encrypted) {
        uint32_t key_len_net;
        if (!recvAll(&key_len_net, sizeof(key_len_net))) { closesocket(client_sock); return; }
        uint32_t key_len = ntohl(key_len_net);
        if (key_len == 0 || key_len > 4096) { closesocket(client_sock); return; }

        encrypted_aes_key.resize(key_len);
        if (!recvAll(encrypted_aes_key.data(), key_len)) { closesocket(client_sock); return; }

        iv.resize(16);
        if (!recvAll(iv.data(), 16)) { closesocket(client_sock); return; }
    }

    // Build the temp file path: <folder_path>/.lanbox_recv_tmp_<relative_path>
    // For nested paths, the prefix goes on the filename component only.
    string sep =
#ifdef _WIN32
        "\\";
#else
        "/";
#endif
    string dir_part, name_part = relative_path;
    size_t last_sep = relative_path.find_last_of("/\\");
    if (last_sep != string::npos) {
        dir_part  = relative_path.substr(0, last_sep + 1);
        name_part = relative_path.substr(last_sep + 1);
    }
    string tmp_path = folder_path + sep + dir_part + RECV_TMP_PREFIX + name_part;

    logSync("[SYNC-RECV] Receiving '" + relative_path + "' (" +
           to_string(fileSize) + " bytes) for group " + group_id +
           " from " + client_ip + (is_encrypted ? " [encrypted]" : ""));

    ofstream outfile(tmp_path, ios::binary);
    if (!outfile.is_open()) {
        cerr << "[SyncManager] Cannot open temp file: " << tmp_path << "\n";
        closesocket(client_sock);
        return;
    }

    vector<char> buffer(TRANSFER_BUFFER_SIZE);
    uint64_t received_bytes = 0;

    while (received_bytes < fileSize) {
        int n = recv(client_sock, buffer.data(),
                    (int)min((uint64_t)TRANSFER_BUFFER_SIZE, fileSize - received_bytes), 0);
        if (n <= 0) break;
        outfile.write(buffer.data(), n);
        received_bytes += n;
    }
    outfile.close();
    closesocket(client_sock);

    if (received_bytes != fileSize) {
        logSync("[SYNC-RECV] Transfer incomplete for '" + relative_path +
               "' from " + client_ip + " (" + to_string(received_bytes) + "/" +
               to_string(fileSize) + " bytes) — discarding temp file");
        remove(tmp_path.c_str());
        return;
    }

    // 4c stops here: temp file is fully written and verified by size.
    // Decryption, rename-to-final, metadata update, and pending_ops
    // integration are added in Step 4g.
    logSync("[SYNC-RECV] Temp file written: " + tmp_path +
           " (awaiting finalize logic from Step 4g)");

    // Stash encrypted_aes_key/iv handling note: for 4c we don't yet decrypt
    // group-sync transfers since finalize (decrypt + rename) belongs to 4g.
    // The encrypted bytes sit in tmp_path as-is until then.
    (void)encrypted_aes_key;
    (void)iv;
}

void SyncManager::ipcLoop() {
    // Unix socket / named pipe for CLI <-> daemon communication — Step 4n
    while (!stop_requested_.load()) {
        this_thread::sleep_for(chrono::milliseconds(200));
    }
}

void SyncManager::maintenanceLoop() {
    // Pending-ops retry, stale offer cleanup, log rotation — Step 4g/4k/4m
    while (!stop_requested_.load()) {
        this_thread::sleep_for(chrono::milliseconds(200));
    }
}

void SyncManager::watcherCallback(const WatchEvent& event) {
    // Dispatches to handleFile*/handleDir* — Step 4d+
    (void)event; // suppress unused warning until 4d
}