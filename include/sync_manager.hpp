#pragma once
#include <string>
#include <vector>
#include <map>
#include <set>
#include <thread>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include "config.hpp"
#include "group_manager.hpp"
#include "folder_watcher.hpp"
#include "json.hpp"

using json = nlohmann::json;
using namespace std;

// ============================================================================
// Sync engine state
// ============================================================================

enum class SyncState {
    PAUSED,
    ACTIVE
};

// Pending operation types for the Windows file-lock deferral queue (Step 4g+)
enum class PendingOpType {
    SWAP,    // .lanbox_recv_tmp_X → X
    DELETE_OP,
    RENAME_OP
};

struct PendingOp {
    PendingOpType type;
    string        group_id;
    string        folder_path;
    string        filename;       // for SWAP/DELETE; new name for RENAME
    string        old_filename;   // only used for RENAME_OP
    string        tmp_path;       // only used for SWAP
    long          first_deferred_at;
    bool          warned_once;
    bool          warned_long;    // separate flag for the 5-minute follow-up warning
};

// ============================================================================
// Persisted state — ~/.lanbox/state.json
// ============================================================================

struct PersistedState {
    bool sync_active      = false;
    long daemon_started_at = 0;
};

void to_json(json& j, const PersistedState& s);
void from_json(const json& j, PersistedState& s);

// ============================================================================
// SyncManager
// ============================================================================

class SyncManager {
public:
    explicit SyncManager(Config& cfg);
    ~SyncManager();

    // ── Daemon lifecycle (called once, from `lanbox start` / `lanbox stop`) ──

    // Starts discovery-independent sync infrastructure:
    //   signaling thread, transfer thread, ipc thread, maintenance thread.
    // Does NOT start the folder watcher — that's controlled separately by
    // startSync()/pauseSync() since sync state is independent of daemon state.
    // Restores sync_active from ~/.lanbox/state.json and calls startSync()
    // automatically if it was active when the daemon last stopped.
    bool startDaemon();

    // Stops everything: if sync is active, performs an inline soft pause first,
    // then stops all threads, saves final state.
    void stopDaemon();

    bool isDaemonRunning() const { return daemon_running_.load(); }

    // ── Sync engine control (called from `lanbox sync start/pause/resume`) ──

    // Starts/resumes syncing: starts the folder watcher, cleans stale temp
    // files, runs local diff + remote catch-up. (Catch-up logic: Step 4j)
    bool startSync();

    // Soft pause: stop accepting new transfers, let current ones finish,
    // then stop the watcher. (Full implementation: Step 4k)
    bool pauseSync(bool hard = false);

    SyncState getSyncState() const { return sync_state_.load(); }

    // ── Status / introspection ──────────────────────────────────────────────

    // Returns a JSON status blob for `lanbox status` / `lanbox sync status`.
    // (Full implementation: Step 4n — stub for now returns basic info.)
    string getStatusJson() const;

private:
    // ── State persistence ───────────────────────────────────────────────────

    string statePath() const;          // ~/.lanbox/state.json
    bool   loadState(PersistedState& out) const;
    bool   saveState(const PersistedState& s) const;

    // ── Thread loops (bodies are stubs in 4a, filled in subsequent steps) ───

    void signalingLoop();      // Step 4b+
    void transferLoop();       // Step 4c+
    void ipcLoop();             // Step 4n
    void maintenanceLoop();     // Step 4g/4k — pending ops retry, log rotation, etc.

    // Folder watcher callback — dispatches to handleFile*/handleDir* (Step 4d+)
    void watcherCallback(const WatchEvent& event);

    // ── Helpers used across many steps ──────────────────────────────────────

    // Resolves which group a given absolute path belongs to, using the
    // currently loaded group list. Returns "" if not found.
    string resolveGroupForPath(const string& abs_path) const;

    // Loads (or refreshes) the in-memory list of groups this device belongs to.
    void refreshGroupList();

    string myFingerprint() const;
    string myName() const;
    long   nowSeconds() const;

    // ── Logging (full implementation: Step 4m) ──────────────────────────────
    void logSync(const string& message);

    // ── Construction-time references ────────────────────────────────────────
    Config&        cfg_;
    GroupManager    gm_;            // stateless helper class — value member is fine

    // ── Watcher ──────────────────────────────────────────────────────────────
    FolderWatcher   watcher_;

    // ── Thread handles ───────────────────────────────────────────────────────
    thread          signaling_thread_;
    thread          transfer_thread_;
    thread          ipc_thread_;
    thread          maintenance_thread_;

    // ── Lifecycle flags ──────────────────────────────────────────────────────
    atomic<bool>     daemon_running_{false};
    atomic<SyncState> sync_state_{SyncState::PAUSED};

    // Signals threads to stop — separate from daemon_running_ so we can
    // distinguish "shutting down" from "never started"
    atomic<bool>     stop_requested_{false};

    // ── Group cache ───────────────────────────────────────────────────────────
    mutable mutex          groups_mutex_;
    vector<GroupConfig>    known_groups_;     // refreshed via refreshGroupList()

    // ── Pending ops queue (Windows file-lock deferral, Step 4g+) ─────────────
    mutex                  pending_ops_mutex_;
    vector<PendingOp>      pending_ops_;

    // ── Sequence counter for outgoing protocol messages ───────────────────────
    atomic<uint32_t>       sequence_counter_{1};
    uint32_t nextSequence() { return sequence_counter_.fetch_add(1); }

    // ── Network identity (cached from cfg_/Crypto at startup) ─────────────────
    string          device_name_;
    uint32_t        local_ip_ = 0;
};