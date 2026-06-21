#include "platform.hpp"
#include "folder_watcher.hpp"
#include <bits/stdc++.h>

#ifdef _WIN32
    #include <windows.h>
#else
    #include <sys/inotify.h>
    #include <sys/stat.h>
    #include <sys/types.h>
    #include <dirent.h>
    #include <unistd.h>
    #include <poll.h>
    #include <fcntl.h>
#endif

using namespace std;

// ============================================================================
// Common helpers
// ============================================================================

// Count path separators to determine depth relative to root.
// group_root depth = 0, direct children = 1, etc.
// Returns -1 if path is not under group_root.
int FolderWatcher::pathDepth(const string& path, const string& group_root) {
    // Normalize: strip trailing separators from root
    string root = group_root;
    while (!root.empty() && (root.back() == '/' || root.back() == '\\'))
        root.pop_back();

    if (path.size() <= root.size()) return -1;
    if (path.substr(0, root.size()) != root) return -1;

    char sep = path[root.size()];
    if (sep != '/' && sep != '\\') return -1;

    // Count separators after root — minus 1 because the last separator
    // is just before the filename itself, not a directory level.
    // Example: root=/a, path=/a/file.txt → 1 sep → depth 0 (file in root)
    //          root=/a, path=/a/sub/file.txt → 2 seps → depth 1 (one subdir)
    int seps = 0;
    for (size_t i = root.size(); i < path.size(); ++i) {
        if (path[i] == '/' || path[i] == '\\') seps++;
    }
    return seps - 1;  // subtract 1: last sep is filename separator
}

bool FolderWatcher::withinDepthLimit(const string& path,
                                     const string& group_root) const {
    int d = pathDepth(path, group_root);
    if (d < 0) return false;       // not under this root
    return d <= max_depth_;
}

void FolderWatcher::fireCallback(const WatchEvent& event) {
    if (!callback_) return;
    try {
        callback_(event);
    } catch (...) {
        // Never let user callback crash the watcher thread
    }
}

string FolderWatcher::resolveGroupRoot(const string& abs_path) const {
    // paths_mutex_ must be held by caller
    for (const auto& [root, _] : watched_roots_) {
        string r = root;
        while (!r.empty() && (r.back() == '/' || r.back() == '\\')) r.pop_back();
        if (abs_path.size() > r.size() &&
            abs_path.substr(0, r.size()) == r &&
            (abs_path[r.size()] == '/' || abs_path[r.size()] == '\\')) {
            return root;
        }
        if (abs_path == r) return root;
    }
    return "";
}

FolderWatcher::FolderWatcher() {}

FolderWatcher::~FolderWatcher() {
    if (running_.load()) stop();
}

bool FolderWatcher::start(Callback cb, int max_depth) {
    if (running_.load()) return false;
    callback_  = cb;
    max_depth_ = max_depth;

    if (!platformInit()) return false;

    running_.store(true);
    watcher_thread_ = thread(&FolderWatcher::watcherLoop, this);
    return true;
}

void FolderWatcher::stop() {
    if (!running_.load()) return;
    running_.store(false);
    platformCleanup();
    if (watcher_thread_.joinable()) watcher_thread_.join();
}

vector<string> FolderWatcher::getWatchedPaths() const {
    lock_guard<mutex> lock(paths_mutex_);
    vector<string> result;
    for (const auto& [root, _] : watched_roots_) result.push_back(root);
    return result;
}

bool FolderWatcher::addWatchPath(const string& folder_path) {
    lock_guard<mutex> lock(paths_mutex_);
    if (watched_roots_.count(folder_path)) return true; // already watching
    watched_roots_[folder_path] = true;
    bool ok = addWatchRecursive(folder_path, folder_path, 0);
#ifndef _WIN32
    // Wake poll() so it picks up the new inotify watches immediately
    char byte = 1;
    write(wake_pipe_[1], &byte, 1);
#else
    SetEvent(wake_event_);
#endif
    return ok;
}

void FolderWatcher::removeWatchPath(const string& folder_path) {
    lock_guard<mutex> lock(paths_mutex_);
    watched_roots_.erase(folder_path);
    removeWatchRecursive(folder_path);
#ifndef _WIN32
    char byte = 1;
    write(wake_pipe_[1], &byte, 1);
#else
    SetEvent(wake_event_);
#endif
}

// ============================================================================
// ██████  Linux (inotify) implementation
// ============================================================================
#ifndef _WIN32

long FolderWatcher::nowMs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

bool FolderWatcher::platformInit() {
    inotify_fd_ = inotify_init1(IN_NONBLOCK);
    if (inotify_fd_ < 0) {
        perror("inotify_init1");
        return false;
    }
    if (pipe2(wake_pipe_, O_NONBLOCK) < 0) {
        perror("pipe2");
        close(inotify_fd_);
        inotify_fd_ = -1;
        return false;
    }
    return true;
}

void FolderWatcher::platformCleanup() {
    // Write to wake pipe to unblock poll()
    char byte = 0;
    write(wake_pipe_[1], &byte, 1);
}

bool FolderWatcher::addWatchRecursive(const string& folder_path,
                                      const string& group_root,
                                      int current_depth) {
    // paths_mutex_ held by caller
    if (current_depth > max_depth_) return true; // silently skip — depth exceeded

    uint32_t mask = IN_CREATE | IN_CLOSE_WRITE | IN_DELETE |
                    IN_MOVED_FROM | IN_MOVED_TO | IN_DONT_FOLLOW;

    int wd = inotify_add_watch(inotify_fd_, folder_path.c_str(), mask);
    if (wd < 0) return false;

    wd_to_path_[wd]   = folder_path;
    wd_to_root_[wd]   = group_root;
    root_to_wds_[group_root].push_back(wd);

    // Recurse into subdirectories
    DIR* d = opendir(folder_path.c_str());
    if (!d) return true; // can't read dir, but watch was added

    struct dirent* entry;
    while ((entry = readdir(d)) != nullptr) {
        string name(entry->d_name);
        if (name == "." || name == "..") continue;
        if (entry->d_type != DT_DIR) continue;

        string subpath = folder_path + "/" + name;

        // Check depth limit
        if (current_depth + 1 > max_depth_) {
            cerr << "[FolderWatcher] WARN: " << subpath
                 << " exceeds depth limit (" << max_depth_
                 << "). Contents will not be synced.\n";
            continue;
        }

        addWatchRecursive(subpath, group_root, current_depth + 1);
    }
    closedir(d);
    return true;
}

void FolderWatcher::removeWatchRecursive(const string& group_root) {
    // paths_mutex_ held by caller
    auto it = root_to_wds_.find(group_root);
    if (it == root_to_wds_.end()) return;

    for (int wd : it->second) {
        inotify_rm_watch(inotify_fd_, wd);
        wd_to_path_.erase(wd);
        wd_to_root_.erase(wd);
    }
    root_to_wds_.erase(it);
}

void FolderWatcher::drainStaleRenames() {
    // paths_mutex_ held by caller
    long now = nowMs();
    auto it = pending_renames_.begin();
    while (it != pending_renames_.end()) {
        if (now - it->second.timestamp_ms > RENAME_COOKIE_TIMEOUT_MS) {
            // MOVED_FROM with no matching MOVED_TO — treat as delete
            WatchEvent ev;
            ev.type       = WatchEventType::FILE_DELETED;
            ev.path       = it->second.from_path;
            ev.group_root = it->second.group_root;
            ev.timestamp  = static_cast<long>(time(nullptr));
            fireCallback(ev);
            it = pending_renames_.erase(it);
        } else {
            ++it;
        }
    }
}

void FolderWatcher::watcherLoop() {
    // Buffer sized for up to 64 events
    const size_t BUF_SIZE = 64 * (sizeof(struct inotify_event) + NAME_MAX + 1);
    vector<char> buf(BUF_SIZE);

    while (running_.load()) {
        // poll() with 100ms timeout — responsive to stop() and stale rename drain
        struct pollfd fds[2];
        fds[0].fd     = inotify_fd_;
        fds[0].events = POLLIN;
        fds[1].fd     = wake_pipe_[0];
        fds[1].events = POLLIN;

        int ret = poll(fds, 2, 100);

        if (!running_.load()) break;

        // Drain stale rename cookies periodically
        {
            lock_guard<mutex> lock(paths_mutex_);
            drainStaleRenames();
        }

        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }

        // Drain wake pipe
        if (fds[1].revents & POLLIN) {
            char tmp[64];
            read(wake_pipe_[0], tmp, sizeof(tmp));
        }

        if (!(fds[0].revents & POLLIN)) continue;

        // Read inotify events
        ssize_t len = read(inotify_fd_, buf.data(), buf.size());
        if (len <= 0) continue;

        ssize_t offset = 0;
        while (offset < len) {
            auto* ie = reinterpret_cast<struct inotify_event*>(buf.data() + offset);
            offset += sizeof(struct inotify_event) + ie->len;

            if (ie->len == 0) continue; // no filename
            string name(ie->name);
            if (name.empty()) continue;

            string folder_path, group_root;
            {
                lock_guard<mutex> lock(paths_mutex_);
                auto pit = wd_to_path_.find(ie->wd);
                auto rit = wd_to_root_.find(ie->wd);
                if (pit == wd_to_path_.end()) continue;
                folder_path = pit->second;
                group_root  = rit->second;
            }

            string full_path = folder_path + "/" + name;
            bool is_dir = (ie->mask & IN_ISDIR) != 0;

            // ── Directory created ──────────────────────────────────────────
            if ((ie->mask & IN_CREATE) && is_dir) {
                int depth = pathDepth(full_path, group_root);
                if (depth > max_depth_) {
                    // Over depth limit
                    // Try to remove if empty
                    if (rmdir(full_path.c_str()) == 0) {
                        cerr << "[FolderWatcher] WARN: " << full_path
                             << " blocked and removed (depth limit "
                             << max_depth_ << ").\n";
                    } else {
                        cerr << "[FolderWatcher] WARN: " << full_path
                             << " exceeds depth limit (" << max_depth_
                             << "). Contents will not be synced.\n";
                    }
                    continue;
                }
                // Within limit — add watch on the new subdir
                {
                    lock_guard<mutex> lock(paths_mutex_);
                    addWatchRecursive(full_path, group_root, depth);
                    // Wake pipe write not needed — we're already on watcher thread
                }
                WatchEvent ev;
                ev.type       = WatchEventType::DIR_CREATED;
                ev.path       = full_path;
                ev.group_root = group_root;
                ev.timestamp  = static_cast<long>(time(nullptr));
                fireCallback(ev);
                continue;
            }

            // ── Directory deleted ──────────────────────────────────────────
            if ((ie->mask & IN_DELETE) && is_dir) {
                // inotify auto-removes the watch when the dir disappears
                // Clean up our maps
                lock_guard<mutex> lock(paths_mutex_);
                for (auto it = wd_to_path_.begin(); it != wd_to_path_.end(); ) {
                    if (it->second == full_path) {
                        wd_to_root_.erase(it->first);
                        auto& wds = root_to_wds_[group_root];
                        wds.erase(remove(wds.begin(), wds.end(), it->first), wds.end());
                        it = wd_to_path_.erase(it);
                    } else ++it;
                }
                WatchEvent ev;
                ev.type       = WatchEventType::DIR_DELETED;
                ev.path       = full_path;
                ev.group_root = group_root;
                ev.timestamp  = static_cast<long>(time(nullptr));
                fireCallback(ev);
                continue;
            }

            // Skip events on directories that aren't file events
            if (is_dir) continue;

            // ── File created ───────────────────────────────────────────────
            if (ie->mask & IN_CREATE) {
                WatchEvent ev;
                ev.type       = WatchEventType::FILE_CREATED;
                ev.path       = full_path;
                ev.group_root = group_root;
                ev.timestamp  = static_cast<long>(time(nullptr));
                fireCallback(ev);
                continue;
            }

            // ── File written (close after write) ───────────────────────────
            if (ie->mask & IN_CLOSE_WRITE) {
                WatchEvent ev;
                ev.type       = WatchEventType::FILE_MODIFIED;
                ev.path       = full_path;
                ev.group_root = group_root;
                ev.timestamp  = static_cast<long>(time(nullptr));
                fireCallback(ev);
                continue;
            }

            // ── File deleted ───────────────────────────────────────────────
            if (ie->mask & IN_DELETE) {
                WatchEvent ev;
                ev.type       = WatchEventType::FILE_DELETED;
                ev.path       = full_path;
                ev.group_root = group_root;
                ev.timestamp  = static_cast<long>(time(nullptr));
                fireCallback(ev);
                continue;
            }

            // ── Rename: MOVED_FROM ─────────────────────────────────────────
            if (ie->mask & IN_MOVED_FROM) {
                lock_guard<mutex> lock(paths_mutex_);
                pending_renames_[ie->cookie] = {full_path, group_root, nowMs()};
                continue;
            }

            // ── Rename: MOVED_TO ──────────────────────────────────────────
            if (ie->mask & IN_MOVED_TO) {
                lock_guard<mutex> lock(paths_mutex_);
                auto it = pending_renames_.find(ie->cookie);
                if (it != pending_renames_.end()) {
                    // Paired rename
                    WatchEvent ev;
                    ev.type       = WatchEventType::FILE_RENAMED;
                    ev.path       = full_path;           // new name
                    ev.old_path   = it->second.from_path; // old name
                    ev.group_root = group_root;
                    ev.timestamp  = static_cast<long>(time(nullptr));
                    pending_renames_.erase(it);
                    fireCallback(ev);
                } else {
                    // MOVED_TO with no matching MOVED_FROM
                    // (file moved in from outside watched tree) — treat as create
                    WatchEvent ev;
                    ev.type       = WatchEventType::FILE_CREATED;
                    ev.path       = full_path;
                    ev.group_root = group_root;
                    ev.timestamp  = static_cast<long>(time(nullptr));
                    fireCallback(ev);
                }
                continue;
            }
        }
    }

    // Final cleanup
    if (inotify_fd_ >= 0) { close(inotify_fd_); inotify_fd_ = -1; }
    if (wake_pipe_[0] >= 0) { close(wake_pipe_[0]); wake_pipe_[0] = -1; }
    if (wake_pipe_[1] >= 0) { close(wake_pipe_[1]); wake_pipe_[1] = -1; }
}

// ============================================================================
// ██████  Windows (ReadDirectoryChangesW) implementation
// ============================================================================
#else

bool FolderWatcher::platformInit() {
    wake_event_ = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    return wake_event_ != INVALID_HANDLE_VALUE;
}

void FolderWatcher::platformCleanup() {
    // Signal wake event to unblock WaitForMultipleObjects
    if (wake_event_ != INVALID_HANDLE_VALUE)
        SetEvent(wake_event_);
}

bool FolderWatcher::addWatchRecursive(const string& folder_path,
                                      const string& group_root,
                                      int current_depth) {
    // paths_mutex_ held by caller
    // On Windows, ReadDirectoryChangesW handles recursion natively.
    // We only create one entry per group root.
    if (win_watches_.count(group_root)) return true;

    HANDLE h = CreateFileA(
        folder_path.c_str(),
        FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
        nullptr
    );
    if (h == INVALID_HANDLE_VALUE) return false;

    WinWatchEntry* entry  = new WinWatchEntry();
    entry->dir_handle     = h;
    entry->group_root     = group_root;
    entry->folder_path    = folder_path;
    entry->buffer.resize(65536);
    memset(&entry->overlapped, 0, sizeof(entry->overlapped));
    entry->overlapped.hEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);

    if (!rearmWatch(entry)) {
        destroyWinEntry(entry);
        return false;
    }

    win_watches_[group_root] = entry;
    return true;
}

void FolderWatcher::removeWatchRecursive(const string& group_root) {
    // paths_mutex_ held by caller
    auto it = win_watches_.find(group_root);
    if (it == win_watches_.end()) return;
    destroyWinEntry(it->second);
    win_watches_.erase(it);
}

bool FolderWatcher::rearmWatch(WinWatchEntry* entry) {
    DWORD notify_filter =
        FILE_NOTIFY_CHANGE_FILE_NAME  |
        FILE_NOTIFY_CHANGE_DIR_NAME   |
        FILE_NOTIFY_CHANGE_LAST_WRITE |
        FILE_NOTIFY_CHANGE_SIZE;

    return ReadDirectoryChangesW(
        entry->dir_handle,
        entry->buffer.data(),
        static_cast<DWORD>(entry->buffer.size()),
        TRUE,   // bWatchSubtree
        notify_filter,
        nullptr,
        &entry->overlapped,
        nullptr
    ) != 0;
}

void FolderWatcher::destroyWinEntry(WinWatchEntry* entry) {
    if (entry->overlapped.hEvent != INVALID_HANDLE_VALUE)
        CloseHandle(entry->overlapped.hEvent);
    if (entry->dir_handle != INVALID_HANDLE_VALUE)
        CloseHandle(entry->dir_handle);
    delete entry;
}

// Compute absolute path from a Windows FILE_NOTIFY_INFORMATION filename
// (which is a relative wide-char path from the watched root)
static string wideRelToAbs(const string& root,
                            const WCHAR* wname, DWORD name_len_bytes) {
    int len = WideCharToMultiByte(CP_UTF8, 0, wname,
                                  name_len_bytes / sizeof(WCHAR),
                                  nullptr, 0, nullptr, nullptr);
    if (len <= 0) return "";
    string rel(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wname,
                        name_len_bytes / sizeof(WCHAR),
                        &rel[0], len, nullptr, nullptr);
    // Keep backslashes — stay consistent with Windows paths
    // Replace forward slashes with backslashes for consistency
    for (char& c : rel) if (c == '/') c = '\\';
    return root + "\\" + rel;
}

void FolderWatcher::watcherLoop() {
    // Pending rename: store the old name until FILE_ACTION_RENAMED_NEW_NAME arrives
    string pending_rename_old;
    string pending_rename_root;

    while (running_.load()) {
        // Collect all watch event handles + wake event
        vector<HANDLE> handles;
        vector<WinWatchEntry*> entries;

        {
            lock_guard<mutex> lock(paths_mutex_);
            for (auto& [root, entry] : win_watches_) {
                handles.push_back(entry->overlapped.hEvent);
                entries.push_back(entry);
            }
        }
        handles.push_back(wake_event_);

        if (handles.empty()) {
            // No watches yet — sleep briefly and retry
            this_thread::sleep_for(chrono::milliseconds(100));
            continue;
        }

        DWORD count = static_cast<DWORD>(handles.size());
        DWORD result = WaitForMultipleObjects(count, handles.data(), FALSE, 100);

        if (!running_.load()) break;

        // Wake event or timeout
        if (result == WAIT_TIMEOUT ||
            result == WAIT_OBJECT_0 + (count - 1)) {
            continue;
        }

        if (result < WAIT_OBJECT_0 || result >= WAIT_OBJECT_0 + count - 1)
            continue;

        DWORD idx = result - WAIT_OBJECT_0;
        WinWatchEntry* entry = entries[idx];

        DWORD bytes_returned = 0;
        if (!GetOverlappedResult(entry->dir_handle, &entry->overlapped,
                                 &bytes_returned, FALSE)) {
            rearmWatch(entry);
            continue;
        }
        if (bytes_returned == 0) {
            rearmWatch(entry);
            continue;
        }

        // Process FILE_NOTIFY_INFORMATION records
        DWORD offset = 0;
        while (offset < bytes_returned) {
            auto* fni = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(
                            entry->buffer.data() + offset);

            string full_path = wideRelToAbs(
                entry->folder_path, fni->FileName, fni->FileNameLength);

            if (!full_path.empty()) {
                // Determine if this is a directory
                DWORD attrs = GetFileAttributesA(full_path.c_str());
                bool is_dir = (attrs != INVALID_FILE_ATTRIBUTES) &&
                              (attrs & FILE_ATTRIBUTE_DIRECTORY);

                // Depth check for directories
                int depth = pathDepth(full_path, entry->group_root);

                switch (fni->Action) {
                    case FILE_ACTION_ADDED: {
                        if (is_dir) {
                            if (depth > max_depth_) {
                                // Over limit
                                if (RemoveDirectoryA(full_path.c_str())) {
                                    cerr << "[FolderWatcher] WARN: " << full_path
                                         << " blocked and removed (depth limit "
                                         << max_depth_ << ").\n";
                                } else {
                                    cerr << "[FolderWatcher] WARN: " << full_path
                                         << " exceeds depth limit (" << max_depth_
                                         << "). Contents will not be synced.\n";
                                }
                            } else {
                                WatchEvent ev;
                                ev.type       = WatchEventType::DIR_CREATED;
                                ev.path       = full_path;
                                ev.group_root = entry->group_root;
                                ev.timestamp  = static_cast<long>(time(nullptr));
                                fireCallback(ev);
                            }
                        } else {
                            if (withinDepthLimit(full_path, entry->group_root)) {
                                WatchEvent ev;
                                ev.type       = WatchEventType::FILE_CREATED;
                                ev.path       = full_path;
                                ev.group_root = entry->group_root;
                                ev.timestamp  = static_cast<long>(time(nullptr));
                                fireCallback(ev);
                            }
                        }
                        break;
                    }
                    case FILE_ACTION_REMOVED: {
                        if (!is_dir &&
                            withinDepthLimit(full_path, entry->group_root)) {
                            WatchEvent ev;
                            ev.type       = WatchEventType::FILE_DELETED;
                            ev.path       = full_path;
                            ev.group_root = entry->group_root;
                            ev.timestamp  = static_cast<long>(time(nullptr));
                            fireCallback(ev);
                        } else if (is_dir) {
                            WatchEvent ev;
                            ev.type       = WatchEventType::DIR_DELETED;
                            ev.path       = full_path;
                            ev.group_root = entry->group_root;
                            ev.timestamp  = static_cast<long>(time(nullptr));
                            fireCallback(ev);
                        }
                        break;
                    }
                    case FILE_ACTION_MODIFIED: {
                        if (!is_dir &&
                            withinDepthLimit(full_path, entry->group_root)) {
                            WatchEvent ev;
                            ev.type       = WatchEventType::FILE_MODIFIED;
                            ev.path       = full_path;
                            ev.group_root = entry->group_root;
                            ev.timestamp  = static_cast<long>(time(nullptr));
                            fireCallback(ev);
                        }
                        break;
                    }
                    case FILE_ACTION_RENAMED_OLD_NAME: {
                        // Store old name — next record will be NEW_NAME
                        pending_rename_old  = full_path;
                        pending_rename_root = entry->group_root;
                        break;
                    }
                    case FILE_ACTION_RENAMED_NEW_NAME: {
                        if (!pending_rename_old.empty() &&
                            withinDepthLimit(full_path, entry->group_root)) {
                            WatchEvent ev;
                            ev.type       = WatchEventType::FILE_RENAMED;
                            ev.path       = full_path;
                            ev.old_path   = pending_rename_old;
                            ev.group_root = entry->group_root;
                            ev.timestamp  = static_cast<long>(time(nullptr));
                            fireCallback(ev);
                        }
                        pending_rename_old.clear();
                        pending_rename_root.clear();
                        break;
                    }
                }
            }

            if (fni->NextEntryOffset == 0) break;
            offset += fni->NextEntryOffset;
        }

        // Re-arm the watch for next event
        rearmWatch(entry);
    }

    // Cleanup all watch entries
    lock_guard<mutex> lock(paths_mutex_);
    for (auto& [root, entry] : win_watches_) destroyWinEntry(entry);
    win_watches_.clear();
    if (wake_event_ != INVALID_HANDLE_VALUE) {
        CloseHandle(wake_event_);
        wake_event_ = INVALID_HANDLE_VALUE;
    }
}

#endif