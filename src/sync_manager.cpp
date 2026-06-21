#include "platform.hpp"
#include "sync_manager.hpp"
#include "crypto.hpp"
#include "protocol.hpp"
#include <bits/stdc++.h>

#ifdef _WIN32
    #include <windows.h>
#else
    #include <unistd.h>
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

void SyncManager::signalingLoop() {
    // UDP receive loop for SYNC_*/GROUP_* messages — Step 4b+
    while (!stop_requested_.load()) {
        this_thread::sleep_for(chrono::milliseconds(200));
    }
}

void SyncManager::transferLoop() {
    // Persistent TCP server for incoming sync transfers — Step 4c+
    while (!stop_requested_.load()) {
        this_thread::sleep_for(chrono::milliseconds(200));
    }
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