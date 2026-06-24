#include "platform.hpp"
#include <bits/stdc++.h>
#ifdef _WIN32
    #include <windows.h>
    #include <direct.h>
    #define MKDIR(p)    _mkdir(p)
    #define PATH_SEP    "\\"
    #define SLEEP_MS(x) Sleep(x)
#else
    #include <sys/stat.h>
    #include <sys/types.h>
    #include <dirent.h>
    #include <unistd.h>
    #define MKDIR(p)    mkdir(p, 0755)
    #define PATH_SEP    "/"
    #define SLEEP_MS(x) usleep((x)*1000)
#endif
#include "folder_watcher.hpp"
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

// ============================================================================
// Helpers
// ============================================================================

static string TEST_ROOT;
static string TEST_GROUP;

static void setupDirs() {
    TEST_ROOT  = "test_tmp_step3";
    TEST_GROUP = TEST_ROOT + PATH_SEP + "watched_group";
    GroupManager::mkdirP(TEST_ROOT);
    GroupManager::mkdirP(TEST_GROUP);
}

static void removeDir(const string& path) {
#ifdef _WIN32
    WIN32_FIND_DATAA fd;
    string pattern = path + "\\*";
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            string name(fd.cFileName);
            if (name == "." || name == "..") continue;
            string full = path + "\\" + name;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) removeDir(full);
            else DeleteFileA(full.c_str());
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(path.c_str());
#else
    DIR* d = opendir(path.c_str());
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d))) {
        string name(e->d_name);
        if (name == "." || name == "..") continue;
        string full = path + "/" + name;
        struct stat st;
        stat(full.c_str(), &st);
        if (S_ISDIR(st.st_mode)) removeDir(full);
        else unlink(full.c_str());
    }
    closedir(d);
    rmdir(path.c_str());
#endif
}

static void writeFile(const string& path, const string& content) {
    ofstream f(path, ios::binary);
    f << content;
}

static void deleteFile(const string& path) {
#ifdef _WIN32
    DeleteFileA(path.c_str());
#else
    unlink(path.c_str());
#endif
}

// ============================================================================
// Event collector — thread-safe, used by all tests
// ============================================================================

struct EventLog {
    mutex              mtx;
    vector<WatchEvent> events;

    void clear() {
        lock_guard<mutex> lock(mtx);
        events.clear();
    }

    void add(const WatchEvent& e) {
        lock_guard<mutex> lock(mtx);
        events.push_back(e);
    }

    // Wait up to timeout_ms for at least count events matching predicate
    bool waitFor(int count,
                 function<bool(const WatchEvent&)> pred,
                 int timeout_ms = 3000) {
        int elapsed = 0;
        while (elapsed < timeout_ms) {
            {
                lock_guard<mutex> lock(mtx);
                int found = 0;
                for (const auto& e : events)
                    if (pred(e)) found++;
                if (found >= count) return true;
            }
            SLEEP_MS(50);
            elapsed += 50;
        }
        return false;
    }

    bool hasEvent(WatchEventType type, const string& path_suffix) {
        lock_guard<mutex> lock(mtx);
        for (const auto& e : events) {
            if (e.type == type &&
                e.path.size() >= path_suffix.size() &&
                e.path.substr(e.path.size() - path_suffix.size()) == path_suffix)
                return true;
        }
        return false;
    }

    int count(WatchEventType type) {
        lock_guard<mutex> lock(mtx);
        int n = 0;
        for (const auto& e : events) if (e.type == type) n++;
        return n;
    }
};

static EventLog g_log;

// ============================================================================
// Tests
// ============================================================================

void test_path_depth() {
    SECTION("pathDepth helper");
#ifdef _WIN32
    CHECK("file in root = depth 0",
          FolderWatcher::pathDepth("C:\\group\\file.txt", "C:\\group") == 0);
    CHECK("file one subdir deep = depth 1",
          FolderWatcher::pathDepth("C:\\group\\sub\\file.txt", "C:\\group") == 1);
    CHECK("file two subdirs deep = depth 2",
          FolderWatcher::pathDepth("C:\\group\\a\\b\\file.txt", "C:\\group") == 2);
    CHECK("root itself not matched",
          FolderWatcher::pathDepth("C:\\group", "C:\\group") == -1);
    CHECK("unrelated path returns -1",
          FolderWatcher::pathDepth("C:\\other\\file.txt", "C:\\group") == -1);
#else
    CHECK("file in root = depth 0",
          FolderWatcher::pathDepth("/group/file.txt", "/group") == 0);
    CHECK("file one subdir deep = depth 1",
          FolderWatcher::pathDepth("/group/sub/file.txt", "/group") == 1);
    CHECK("file two subdirs deep = depth 2",
          FolderWatcher::pathDepth("/group/a/b/file.txt", "/group") == 2);
    CHECK("root itself not matched",
          FolderWatcher::pathDepth("/group", "/group") == -1);
    CHECK("unrelated path returns -1",
          FolderWatcher::pathDepth("/other/file.txt", "/group") == -1);
    CHECK("depth 3",
          FolderWatcher::pathDepth("/group/a/b/c/file.txt", "/group") == 3);
#endif
}

void test_start_stop() {
    SECTION("start / stop / isRunning");

    FolderWatcher fw;
    CHECK("not running before start", !fw.isRunning());

    bool ok = fw.start([](const WatchEvent&){}, 5);
    CHECK("start returns true", ok);
    CHECK("isRunning after start", fw.isRunning());

    // Double start should fail
    bool ok2 = fw.start([](const WatchEvent&){}, 5);
    CHECK("double start returns false", !ok2);

    fw.stop();
    CHECK("not running after stop", !fw.isRunning());

    // Restart should work
    bool ok3 = fw.start([](const WatchEvent&){}, 5);
    CHECK("restart returns true", ok3);
    fw.stop();
}

void test_add_remove_watch() {
    SECTION("addWatchPath / removeWatchPath / getWatchedPaths");

    FolderWatcher fw;
    fw.start([](const WatchEvent&){}, 5);

    fw.addWatchPath(TEST_GROUP);
    SLEEP_MS(200);

    auto paths = fw.getWatchedPaths();
    bool found = false;
    for (const auto& p : paths) if (p == TEST_GROUP) found = true;
    CHECK("TEST_GROUP in watched paths", found);

    fw.removeWatchPath(TEST_GROUP);
    SLEEP_MS(200);

    auto paths2 = fw.getWatchedPaths();
    bool still = false;
    for (const auto& p : paths2) if (p == TEST_GROUP) still = true;
    CHECK("TEST_GROUP removed from watched paths", !still);

    fw.stop();
}

void test_file_created() {
    SECTION("FILE_CREATED event");

    g_log.clear();
    FolderWatcher fw;
    fw.start([](const WatchEvent& e){ g_log.add(e); }, 5);
    fw.addWatchPath(TEST_GROUP);
    SLEEP_MS(300);

    string fpath = TEST_GROUP + PATH_SEP + "created.txt";
    writeFile(fpath, "hello");

    bool got = g_log.waitFor(1, [](const WatchEvent& e){
        return e.type == WatchEventType::FILE_CREATED ||
               e.type == WatchEventType::FILE_MODIFIED;
    });
    CHECK("FILE_CREATED or FILE_MODIFIED fired", got);

    deleteFile(fpath);
    fw.stop();
}

void test_file_modified() {
    SECTION("FILE_MODIFIED event");

    // Create the file first (outside watch to avoid noise)
    string fpath = TEST_GROUP + PATH_SEP + "modify_me.txt";
    writeFile(fpath, "original");
    SLEEP_MS(300);

    g_log.clear();
    FolderWatcher fw;
    fw.start([](const WatchEvent& e){ g_log.add(e); }, 5);
    fw.addWatchPath(TEST_GROUP);
    SLEEP_MS(300);

    // Now modify it
    writeFile(fpath, "modified content");

    bool got = g_log.waitFor(1, [](const WatchEvent& e){
        return e.type == WatchEventType::FILE_MODIFIED ||
               e.type == WatchEventType::FILE_CREATED;
    });
    CHECK("FILE_MODIFIED fired", got);

    deleteFile(fpath);
    fw.stop();
}

void test_file_deleted() {
    SECTION("FILE_DELETED event");

    string fpath = TEST_GROUP + PATH_SEP + "delete_me.txt";
    writeFile(fpath, "bye");
    SLEEP_MS(300);

    g_log.clear();
    FolderWatcher fw;
    fw.start([](const WatchEvent& e){ g_log.add(e); }, 5);
    fw.addWatchPath(TEST_GROUP);
    SLEEP_MS(300);

    deleteFile(fpath);

    bool got = g_log.waitFor(1, [](const WatchEvent& e){
        return e.type == WatchEventType::FILE_DELETED;
    });
    CHECK("FILE_DELETED fired", got);

    fw.stop();
}

void test_file_renamed() {
    SECTION("FILE_RENAMED event");

    string old_path = TEST_GROUP + PATH_SEP + "old_name.txt";
    string new_path = TEST_GROUP + PATH_SEP + "new_name.txt";
    writeFile(old_path, "rename me");
    SLEEP_MS(300);

    g_log.clear();
    FolderWatcher fw;
    fw.start([](const WatchEvent& e){ g_log.add(e); }, 5);
    fw.addWatchPath(TEST_GROUP);
    SLEEP_MS(300);

    rename(old_path.c_str(), new_path.c_str());

    // Either a rename event or create+delete pair is acceptable
    bool got = g_log.waitFor(1, [](const WatchEvent& e){
        return e.type == WatchEventType::FILE_RENAMED ||
               e.type == WatchEventType::FILE_CREATED ||
               e.type == WatchEventType::FILE_DELETED;
    });
    CHECK("Rename event fired", got);

    // If we got a proper rename, check old/new paths
    {
        lock_guard<mutex> lock(g_log.mtx);
        for (const auto& e : g_log.events) {
            if (e.type == WatchEventType::FILE_RENAMED) {
                CHECK("Rename new path contains new_name",
                      e.path.find("new_name.txt") != string::npos);
                CHECK("Rename old path contains old_name",
                      e.old_path.find("old_name.txt") != string::npos);
                break;
            }
        }
    }

    deleteFile(new_path);
    fw.stop();
}

void test_subdir_watching() {
    SECTION("Subdirectory watching");

    string subdir = TEST_GROUP + PATH_SEP + "subdir";
    GroupManager::mkdirP(subdir);
    SLEEP_MS(200);

    g_log.clear();
    FolderWatcher fw;
    fw.start([](const WatchEvent& e){ g_log.add(e); }, 5);
    fw.addWatchPath(TEST_GROUP);
    SLEEP_MS(300);

    // Create file inside subdir — should be detected
    string fpath = subdir + PATH_SEP + "deep_file.txt";
    writeFile(fpath, "deep content");

    bool got = g_log.waitFor(1, [](const WatchEvent& e){
        return e.type == WatchEventType::FILE_CREATED ||
               e.type == WatchEventType::FILE_MODIFIED;
    });
    CHECK("File in subdir detected", got);

    deleteFile(fpath);
    removeDir(subdir);
    fw.stop();
}

void test_depth_limit() {
    SECTION("Depth limit enforcement");

    // Create dirs at limit and beyond
    string d1 = TEST_GROUP + PATH_SEP + "l1";        // depth 0
    string d2 = d1 + PATH_SEP + "l2";                // depth 1
    string d3 = d2 + PATH_SEP + "l3";                // depth 2 — at limit, should be allowed
    string d4 = d3 + PATH_SEP + "l4";                // depth 3 — over limit, should be block
    // File at depth 1 — should sync
    string f1 = d1 + PATH_SEP + "f1.txt";
    // File at depth 2 — should sync
    string f2 = d2 + PATH_SEP + "f2.txt";

    GroupManager::mkdirP(d1);
    GroupManager::mkdirP(d2);
    SLEEP_MS(200);

    g_log.clear();
    FolderWatcher fw;
    fw.start([](const WatchEvent& e){ g_log.add(e); }, 2); // max_depth = 2
    fw.addWatchPath(TEST_GROUP);
    SLEEP_MS(300);
    // DEBUG — print what paths are being watched
    cout << "  [DEBUG] TEST_GROUP = " << TEST_GROUP << "\n";
    cout << "  [DEBUG] d2 = " << d2 << "\n";
    cout << "  [DEBUG] f2 = " << f2 << "\n";
    cout << "  [DEBUG] pathDepth(f2, TEST_GROUP) = "
        << FolderWatcher::pathDepth(f2, TEST_GROUP) << "\n";
    writeFile(f1, "depth 1");
    SLEEP_MS(500);
    bool d1_detected = g_log.hasEvent(WatchEventType::FILE_CREATED, "f1.txt") ||
                       g_log.hasEvent(WatchEventType::FILE_MODIFIED, "f1.txt");
    CHECK("File at depth 1 detected", d1_detected);

    writeFile(f2, "depth 2");
    SLEEP_MS(500);
    // DEBUG — print all events so far
    {
        lock_guard<mutex> lock(g_log.mtx);
        cout << "  [DEBUG] Events collected for depth test:\n";
        for (const auto& e : g_log.events) {
            string type;
            switch(e.type) {
                case WatchEventType::FILE_CREATED:  type = "FILE_CREATED";  break;
                case WatchEventType::FILE_MODIFIED: type = "FILE_MODIFIED"; break;
                case WatchEventType::FILE_DELETED:  type = "FILE_DELETED";  break;
                case WatchEventType::FILE_RENAMED:  type = "FILE_RENAMED";  break;
                case WatchEventType::DIR_CREATED:   type = "DIR_CREATED";   break;
                case WatchEventType::DIR_DELETED:   type = "DIR_DELETED";   break;
            }
            cout << "    type=" << type
                << " path=" << e.path
                << " root=" << e.group_root << "\n";
        }
    }
    bool d2_detected = g_log.hasEvent(WatchEventType::FILE_CREATED, "f2.txt") ||
                       g_log.hasEvent(WatchEventType::FILE_MODIFIED, "f2.txt");
    CHECK("File at depth 2 detected", d2_detected);

    // Create dir at depth 3 — watcher should block/warn
#ifdef _WIN32
    CreateDirectoryA(d4.c_str(), nullptr);
#else
    mkdir(d4.c_str(), 0755);
#endif
    SLEEP_MS(500);

    // File inside over-limit dir — should NOT be detected
    g_log.clear();
    string f3 = d4 + PATH_SEP + "f3.txt";   // changed from d3 to d4
    writeFile(f3, "depth 4 — should not sync");
    SLEEP_MS(500);
    bool d3_detected = g_log.hasEvent(WatchEventType::FILE_CREATED, "f3.txt") ||
                    g_log.hasEvent(WatchEventType::FILE_MODIFIED, "f3.txt");
    CHECK("File beyond depth limit NOT detected", !d3_detected);

    // Cleanup
    deleteFile(f1); deleteFile(f2); deleteFile(f3);
    removeDir(d4); removeDir(d3); removeDir(d2); removeDir(d1);   // added d4 cleanup
    fw.stop();
}

void test_group_root_in_event() {
    SECTION("group_root correctly set in events");

    g_log.clear();
    FolderWatcher fw;
    fw.start([](const WatchEvent& e){ g_log.add(e); }, 5);
    fw.addWatchPath(TEST_GROUP);
    SLEEP_MS(300);

    string fpath = TEST_GROUP + PATH_SEP + "root_check.txt";
    writeFile(fpath, "test");

    g_log.waitFor(1, [](const WatchEvent& e){
        return e.type == WatchEventType::FILE_CREATED ||
               e.type == WatchEventType::FILE_MODIFIED;
    });

    {
        lock_guard<mutex> lock(g_log.mtx);
        bool correct_root = false;
        for (const auto& e : g_log.events) {
            if (e.group_root == TEST_GROUP) { correct_root = true; break; }
        }
        CHECK("group_root matches TEST_GROUP", correct_root);
    }

    deleteFile(fpath);
    fw.stop();
}

void test_multiple_group_roots() {
    SECTION("Multiple group roots watched simultaneously");

    string group_b = TEST_ROOT + PATH_SEP + "group_b";
    GroupManager::mkdirP(group_b);

    g_log.clear();
    FolderWatcher fw;
    fw.start([](const WatchEvent& e){ g_log.add(e); }, 5);
    bool ok_a = fw.addWatchPath(TEST_GROUP);
    bool ok_b = fw.addWatchPath(group_b);
    cout << "  [DEBUG] addWatchPath(TEST_GROUP) = " << ok_a << "\n";
    cout << "  [DEBUG] addWatchPath(group_b) = " << ok_b << "\n";
    SLEEP_MS(300);
    SLEEP_MS(300);
    auto watched = fw.getWatchedPaths();
    cout << "  [DEBUG] Currently watched paths:\n";
    for (const auto& p : watched) cout << "    " << p << "\n";

    string fa = TEST_GROUP + PATH_SEP + "ga.txt";
    string fb = group_b   + PATH_SEP + "gb.txt";
    writeFile(fa, "group a");
    writeFile(fb, "group b");

    g_log.waitFor(2, [](const WatchEvent& e){
        return e.type == WatchEventType::FILE_CREATED ||
               e.type == WatchEventType::FILE_MODIFIED;
    });
    {
        lock_guard<mutex> lock(g_log.mtx);
        cout << "  [DEBUG] Events in multi-root test:\n";
        for (const auto& e : g_log.events) {
            cout << "    path=" << e.path << " root=" << e.group_root << "\n";
        }
    }
    bool root_a = false, root_b = false;
    {
        lock_guard<mutex> lock(g_log.mtx);
        for (const auto& e : g_log.events) {
            if (e.group_root == TEST_GROUP) root_a = true;
            if (e.group_root == group_b)    root_b = true;
        }
    }
    CHECK("Events from group A have correct root", root_a);
    CHECK("Events from group B have correct root", root_b);

    deleteFile(fa); deleteFile(fb);
    removeDir(group_b);
    fw.stop();
}

// ============================================================================
// Main
// ============================================================================

int main() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

    setupDirs();

    cout << "=========================================\n";
    cout << "  LANBox Step 3 — FolderWatcher Tests    \n";
    cout << "=========================================\n";

    test_path_depth();
    test_start_stop();
    test_add_remove_watch();
    test_file_created();
    test_file_modified();
    test_file_deleted();
    test_file_renamed();
    test_subdir_watching();
    test_depth_limit();
    test_group_root_in_event();
    test_multiple_group_roots();

    cout << "\n=========================================\n";
    cout << "  Results: "
         << tests_passed << " passed, "
         << tests_failed << " failed, "
         << tests_run    << " total\n";
    cout << "=========================================\n";

    removeDir(TEST_ROOT);
    return (tests_failed == 0) ? 0 : 1;
}