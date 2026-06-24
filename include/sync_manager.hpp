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
#include "protocol.hpp"

using json = nlohmann::json;
using namespace std;

const uint16_t SYNC_SIGNALING_PORT = 5003;
const uint16_t SYNC_TRANSFER_PORT = 5000;
const int      TRANSFER_BUFFER_SIZE = 64 * 1024; // 64 KB, matches existing wire format

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

// ── Signaling socket (UDP :5003) ───────────────────────────────────────
    SocketType signaling_socket_ = INVALID_SOCKET_VAL;
    bool initSignalingSocket();
    void closeSignalingSocket();

    // Dispatch an incoming parsed message to the correct handler
    void dispatchSyncMessage(const LANBoxHeader& header,
                             const vector<uint8_t>& payload,
                             const string& sender_ip);

    // ── Handler stubs (full bodies arrive in Steps 4e–4l) ───────────────────
    void onSyncNotify       (const SyncNotifyPayload& p, const string& sender_ip);
    void onSyncRequest      (const SyncRequestPayload& p, const string& sender_ip);
    void onSyncOffer        (const SyncOfferPayload& p, const string& sender_ip);
    void onSyncOfferAccept  (const SyncOfferAcceptPayload& p, const string& sender_ip);
    void onSyncDecline      (const SyncDeclinePayload& p, const string& sender_ip);
    void onSyncDeleteNotify (const SyncDeletePayload& p, const string& sender_ip);
    void onSyncRenameNotify (const SyncRenamePayload& p, const string& sender_ip);
    void onSyncMetaRequest  (const SyncMetaRequestPayload& p, const string& sender_ip);
    void onSyncMetaDelta    (const SyncMetaDeltaPayload& fixed, const string& json_data, const string& sender_ip);
    void onSyncPauseNotify  (const SyncPauseNotifyPayload& p, const string& sender_ip);
    void onSyncPauseAck     (const SyncPauseAckPayload& p, const string& sender_ip);
    void onSyncResumeNotify (const SyncResumeNotifyPayload& p, const string& sender_ip);
    void onGroupInvite      (const GroupInvitePayload& fixed, const string& group_json, const string& sender_ip);
    void onGroupInviteAck   (const GroupInviteAckPayload& p, const string& sender_ip);
    void onGroupJoinRequest (const GroupJoinRequestPayload& fixed, const string& pubkey_pem, const string& sender_ip);
    void onGroupJoinApprove (const GroupJoinApprovePayload& fixed, const string& group_json, const string& sender_ip);
    void onGroupJoinDeny    (const GroupJoinDenyPayload& p, const string& sender_ip);
    void onGroupKick        (const GroupKickPayload& p, const string& sender_ip);

// ── Transfer socket (TCP :5000) — persistent multi-connection server ────
    SocketType transfer_listen_socket_ = INVALID_SOCKET_VAL;
    bool initTransferSocket();
    void closeTransferSocket();

    // Spawned per accepted connection — runs on its own thread
    void handleIncomingConnection(SocketType client_sock, string client_ip);

    // Legacy path: transfer_type == 0 — adapts the original receiveFile()
    // wire format, but per-connection rather than single-shot.
    void receiveLegacyTransfer(SocketType client_sock, const string& client_ip);

    // Group sync path: transfer_type == 1 — writes to .lanbox_recv_tmp_<filename>
    // inside the resolved group folder. Rename-on-completion, metadata update,
    // and pending_ops integration arrive in Step 4g.
    void receiveGroupSyncTransfer(SocketType client_sock,
                                  const string& group_id,
                                  const string& relative_path,
                                  const string& client_ip);

    // Track active connection-handler threads so stopDaemon() can join them
    mutex              transfer_threads_mutex_;
    vector<thread>     transfer_threads_;
    void reapFinishedTransferThreads(); // called periodically to join completed threads
};