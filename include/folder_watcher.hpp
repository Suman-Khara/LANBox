#pragma once
#include "platform.hpp"
#include <string>
#include <vector>
#include <functional>
#include <atomic>
#include <mutex>
#include <thread>
#include <map>

using namespace std;

// ============================================================================
// Watch event types
// ============================================================================

enum class WatchEventType {
    FILE_CREATED,
    FILE_MODIFIED,
    FILE_DELETED,
    FILE_RENAMED,   // old_path populated
    DIR_CREATED,    // informational — SyncManager uses this to know a subdir appeared
    DIR_DELETED,    // informational
};

// ============================================================================
// WatchEvent
// ============================================================================

struct WatchEvent {
    WatchEventType  type;
    string          path;       // Full absolute path (new path for renames)
    string          old_path;   // Only populated for FILE_RENAMED
    string          group_root; // The watched group folder this event belongs to
    long            timestamp;
};

// ============================================================================
// FolderWatcher
// ============================================================================
// Single background thread watching all registered group folders.
// Events are delivered via callback on the watcher thread — callback must
// be fast and non-blocking (SyncManager queues events internally).
//
// Depth limiting:
//   max_depth is enforced on both platforms. On Linux, subdirs deeper than
//   max_depth are never given inotify watches. On Windows, ReadDirectoryChangesW
//   watches the full subtree natively, so depth is enforced by filtering events
//   whose path depth exceeds group_root depth + max_depth before firing callback.
//
// Thread safety:
//   addWatchPath / removeWatchPath are safe to call from any thread while
//   the watcher is running. Internally synchronized via paths_mutex_.
// ============================================================================

class FolderWatcher {
public:
    using Callback = function<void(const WatchEvent&)>;

    FolderWatcher();
    ~FolderWatcher();

    // Start the watcher thread. Must be called before addWatchPath.
    // cb: callback invoked for every event (on the watcher thread).
    // max_depth: maximum subdirectory depth to watch (1 = top-level only).
    bool start(Callback cb, int max_depth = 5);

    // Stop the watcher thread. Blocks until the thread exits.
    void stop();

    bool isRunning() const { return running_.load(); }

    // Add a group folder to be watched (recursive up to max_depth).
    // Safe to call while running.
    bool addWatchPath(const string& folder_path);

    // Remove a group folder and all its subdir watches.
    // Safe to call while running.
    void removeWatchPath(const string& folder_path);

    // Returns all currently watched root paths
    vector<string> getWatchedPaths() const;
    
    // Returns the depth of path relative to group_root.
    // group_root itself = depth 0, direct children = depth 1, etc.
    // Returns -1 if path is not under group_root.
    static int pathDepth(const string& path, const string& group_root);

private:
    // ── Platform-specific internals ────────────────────────────────────────

    bool platformInit();
    void platformCleanup();
    void watcherLoop();

    bool addWatchRecursive(const string& folder_path,
                           const string& group_root,
                           int current_depth);

    void removeWatchRecursive(const string& group_root);

    string resolveGroupRoot(const string& abs_path) const;

    void fireCallback(const WatchEvent& event);


    // Returns true if path is within the allowed depth for its group root.
    bool withinDepthLimit(const string& path, const string& group_root) const;

#ifdef _WIN32
    // ── Windows state ──────────────────────────────────────────────────────
    // Each watched root gets its own HANDLE + overlapped buffer.
    // ReadDirectoryChangesW with bWatchSubtree=TRUE handles recursion natively.
    // Depth limiting is applied by filtering events in watcherLoop before
    // firing the callback.
    struct WinWatchEntry {
        HANDLE          dir_handle;
        OVERLAPPED      overlapped;
        vector<uint8_t> buffer;         // ReadDirectoryChangesW output buffer
        string          group_root;     // Which group this entry belongs to
        string          folder_path;    // Absolute path of watched dir
    };

    // Map: folder_path → WinWatchEntry*
    map<string, WinWatchEntry*> win_watches_;   // guarded by paths_mutex_

    // Event used to wake up WaitForMultipleObjects when paths change or stop
    HANDLE wake_event_ = INVALID_HANDLE_VALUE;

    // Issue a new ReadDirectoryChangesW call on an entry (re-arm after event)
    bool rearmWatch(WinWatchEntry* entry);

    // Cleanup and delete a single WinWatchEntry
    void destroyWinEntry(WinWatchEntry* entry);

#else
    // ── Linux (inotify) state ──────────────────────────────────────────────
    int inotify_fd_ = -1;

    // pipe used to wake the poll() call when paths change or stop
    int wake_pipe_[2] = {-1, -1};

    // Map: inotify watch descriptor → absolute folder path
    map<int, string> wd_to_path_;      // guarded by paths_mutex_

    // Map: inotify watch descriptor → group_root
    map<int, string> wd_to_root_;      // guarded by paths_mutex_

    // Map: group_root → list of watch descriptors it owns
    map<string, vector<int>> root_to_wds_;  // guarded by paths_mutex_

    // Cookie map for pairing IN_MOVED_FROM / IN_MOVED_TO rename events
    struct PendingRename {
        string from_path;
        string group_root;
        long   timestamp_ms;  // milliseconds since epoch
    };
    map<uint32_t, PendingRename> pending_renames_;  // guarded by paths_mutex_

    // Drain stale rename cookies older than RENAME_COOKIE_TIMEOUT_MS.
    void drainStaleRenames();
    static const int RENAME_COOKIE_TIMEOUT_MS = 200;

    // Get current time in milliseconds (for rename cookie timeout)
    static long nowMs();
#endif

    // ── Common state ───────────────────────────────────────────────────────
    Callback            callback_;
    int                 max_depth_  = 5;
    atomic<bool>        running_    {false};
    thread              watcher_thread_;
    mutable mutex       paths_mutex_;

    // Map: group_root → true (tracks which roots are registered)
    map<string, bool>   watched_roots_;    // guarded by paths_mutex_
};