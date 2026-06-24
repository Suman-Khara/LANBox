#include "platform.hpp"
#include <bits/stdc++.h>
#ifdef _WIN32
    #include <windows.h>
#else
    #include <unistd.h>
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

// ============================================================================
// Tests
// ============================================================================

void test_initial_state() {
    SECTION("Initial state before any start()");

    Config cfg("test_tmp_step4a_lanbox.json");
    SyncManager sm(cfg);

    CHECK("daemon not running initially", !sm.isDaemonRunning());
    CHECK("sync state is PAUSED initially", sm.getSyncState() == SyncState::PAUSED);
}

void test_daemon_start_stop() {
    SECTION("startDaemon / stopDaemon lifecycle");

    Config cfg("test_tmp_step4a_lanbox.json");
    SyncManager sm(cfg);

    bool started = sm.startDaemon();
    CHECK("startDaemon returns true", started);
    CHECK("isDaemonRunning true after start", sm.isDaemonRunning());

    // Double start should fail gracefully
    bool started2 = sm.startDaemon();
    CHECK("double startDaemon returns false", !started2);

    sm.stopDaemon();
    CHECK("isDaemonRunning false after stop", !sm.isDaemonRunning());

    // Restart should work
    bool started3 = sm.startDaemon();
    CHECK("restart after stop returns true", started3);
    sm.stopDaemon();
}

void test_sync_start_pause() {
    SECTION("startSync / pauseSync");

    Config cfg("test_tmp_step4a_lanbox.json");
    SyncManager sm(cfg);
    sm.startDaemon();

    // Daemon starts with sync PAUSED by default (no prior state, or prior
    // state explicitly false) unless a previous test left it ACTIVE —
    // so we don't assert the initial value here, just exercise the transitions.

    bool ok1 = sm.startSync();
    CHECK("startSync returns true", ok1);
    CHECK("sync state ACTIVE after startSync", sm.getSyncState() == SyncState::ACTIVE);

    // Idempotent — calling again while already active should still return true
    bool ok2 = sm.startSync();
    CHECK("second startSync still returns true", ok2);
    CHECK("still ACTIVE", sm.getSyncState() == SyncState::ACTIVE);

    bool ok3 = sm.pauseSync(false);
    CHECK("pauseSync (soft) returns true", ok3);
    CHECK("sync state PAUSED after pauseSync", sm.getSyncState() == SyncState::PAUSED);

    // Idempotent pause
    bool ok4 = sm.pauseSync(false);
    CHECK("second pauseSync still returns true", ok4);

    sm.stopDaemon();
}

void test_state_persistence() {
    SECTION("State persists across SyncManager instances");

    string state_file = GroupManager::getLanboxDir() +
#ifdef _WIN32
        "\\state.json";
#else
        "/state.json";
#endif
    // Clean slate
    remove(state_file.c_str());

    {
        Config cfg("test_tmp_step4a_lanbox.json");
        SyncManager sm(cfg);
        sm.startDaemon();
        CHECK("sync starts PAUSED with no prior state file",
              sm.getSyncState() == SyncState::PAUSED);

        sm.startSync();
        CHECK("sync is ACTIVE", sm.getSyncState() == SyncState::ACTIVE);

        sm.stopDaemon(); // should persist sync_active=true... 
        // wait: stopDaemon does an inline soft pause first, which sets
        // sync_active=false before saving. So after a clean stopDaemon(),
        // the persisted state should be PAUSED, not ACTIVE.
    }

    // Verify the state file reflects PAUSED (since stopDaemon soft-pauses first)
    ifstream f(state_file);
    CHECK("state.json exists after stopDaemon", f.is_open());
    if (f.is_open()) {
        json j; f >> j;
        bool sync_active = j.value("sync_active", true);
        CHECK("persisted sync_active is false (soft-paused on stop)",
              sync_active == false);
    }

    // New instance should start PAUSED (matching what was persisted)
    {
        Config cfg("test_tmp_step4a_lanbox.json");
        SyncManager sm(cfg);
        sm.startDaemon();
        CHECK("new instance restores PAUSED state",
              sm.getSyncState() == SyncState::PAUSED);
        sm.stopDaemon();
    }
}

void test_state_persistence_active_via_crash_simulation() {
    SECTION("Simulated 'left active' state restores to ACTIVE on next start");

    // Manually write a state.json with sync_active=true to simulate
    // a scenario where the daemon didn't get a clean shutdown
    // (e.g. killed without calling stopDaemon, so the soft-pause-on-stop
    // never ran and the last good state was ACTIVE).
    string state_file = GroupManager::getLanboxDir() +
#ifdef _WIN32
        "\\state.json";
#else
        "/state.json";
#endif
    json j;
    j["sync_active"]       = true;
    j["daemon_started_at"] = 0;
    ofstream f(state_file);
    f << j.dump(4);
    f.close();

    Config cfg("test_tmp_step4a_lanbox.json");
    SyncManager sm(cfg);
    sm.startDaemon();

    CHECK("daemon restores ACTIVE from persisted state",
          sm.getSyncState() == SyncState::ACTIVE);

    sm.stopDaemon();
}

void test_get_status_json() {
    SECTION("getStatusJson");

    Config cfg("test_tmp_step4a_lanbox.json");
    SyncManager sm(cfg);
    sm.startDaemon();

    string status = sm.getStatusJson();
    CHECK("status not empty", !status.empty());

    json j = json::parse(status);
    CHECK("status has daemon_running field", j.contains("daemon_running"));
    CHECK("status has sync_state field",      j.contains("sync_state"));
    CHECK("status has group_count field",     j.contains("group_count"));
    CHECK("daemon_running is true",            j["daemon_running"] == true);

    sm.stopDaemon();
}

void test_threads_actually_exit() {
    SECTION("Threads exit cleanly on stop (no hang)");

    Config cfg("test_tmp_step4a_lanbox.json");
    SyncManager sm(cfg);

    sm.startDaemon();
    sleepMs(300); // let threads spin up

    auto start_time = chrono::steady_clock::now();
    sm.stopDaemon();
    auto elapsed = chrono::duration_cast<chrono::milliseconds>(
                       chrono::steady_clock::now() - start_time).count();

    CHECK("stopDaemon returns reasonably quickly (<2000ms)", elapsed < 2000);
    CHECK("daemon reports not running after stop", !sm.isDaemonRunning());
}

// ============================================================================
// Main
// ============================================================================

int main() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

    cout << "=========================================\n";
    cout << "  LANBox Step 4a — SyncManager Skeleton  \n";
    cout << "=========================================\n";

    test_initial_state();
    test_daemon_start_stop();
    test_sync_start_pause();
    test_state_persistence();
    test_state_persistence_active_via_crash_simulation();
    test_get_status_json();
    test_threads_actually_exit();

    cout << "\n=========================================\n";
    cout << "  Results: "
         << tests_passed << " passed, "
         << tests_failed << " failed, "
         << tests_run    << " total\n";
    cout << "=========================================\n";

    // Cleanup
    remove("test_tmp_step4a_lanbox.json");

    return (tests_failed == 0) ? 0 : 1;
}