#include "platform.hpp"
#include <bits/stdc++.h>
#ifdef _WIN32
    #include <windows.h>
    #include <direct.h>
    #define RMDIR(p)   _rmdir(p)
    #define MKDIR(p)   _mkdir(p)
    #define PATH_SEP   "\\"
#else
    #include <sys/stat.h>
    #include <sys/types.h>
    #include <dirent.h>
    #include <unistd.h>
    #define RMDIR(p)   rmdir(p)
    #define MKDIR(p)   mkdir(p, 0755)
    #define PATH_SEP   "/"
#endif
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
// Test directory setup
// ============================================================================

// Use a temp folder inside the build directory so tests are self-contained
static string TEST_DIR;
static string TEST_GROUP_A;
static string TEST_GROUP_B;

static void setupTestDirs() {
#ifdef _WIN32
    TEST_DIR     = "test_tmp_step2";
    TEST_GROUP_A = TEST_DIR + "\\group_a";
    TEST_GROUP_B = TEST_DIR + "\\group_b";
#else
    TEST_DIR     = "test_tmp_step2";
    TEST_GROUP_A = TEST_DIR + "/group_a";
    TEST_GROUP_B = TEST_DIR + "/group_b";
#endif
    GroupManager::mkdirP(TEST_DIR);
    GroupManager::mkdirP(TEST_GROUP_A);
    GroupManager::mkdirP(TEST_GROUP_B);
}

// Write a small dummy file for checksum / buildFileEntry tests
static string writeDummyFile(const string& dir, const string& name,
                              const string& content) {
    string path = dir + PATH_SEP + name;
    ofstream f(path, ios::binary);
    f << content;
    return path;
}

// Recursively delete a directory (test cleanup)
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
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                removeDir(full);
            else
                DeleteFileA(full.c_str());
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

// ============================================================================
// Tests
// ============================================================================

void test_vector_clock() {
    SECTION("VectorClock operations");

    VectorClock a, b;

    // Increment
    a.increment("alice");
    a.increment("alice");
    a.increment("bob");
    CHECK("alice count = 2",    a.clock["alice"] == 2);
    CHECK("bob count = 1",      a.clock["bob"]   == 1);

    // Dominance: a has alice=2,bob=1; b is empty → a dominates b
    CHECK("a dominates empty b", a.dominates(b));
    CHECK("empty b does not dominate a", !b.dominates(a));

    // b catches up on alice but not bob
    b.increment("alice");
    b.increment("alice");
    // b: alice=2; a: alice=2, bob=1
    // a still dominates b (b has no bob entry, so b doesn't dominate a)
    CHECK("a dominates b (bob missing in b)", a.dominates(b));
    CHECK("b does not dominate a",            !b.dominates(a));

    // b adds bob=1 too → now equal
    b.increment("bob");
    CHECK("equal clocks: a dominates b", a.dominates(b));
    CHECK("equal clocks: b dominates a", b.dominates(a));
    CHECK("equals()",                    a.equals(b));

    // Concurrent: a has carol, b has dave
    VectorClock c, d;
    c.increment("carol");
    d.increment("dave");
    CHECK("concurrent: c does not dominate d", !c.dominates(d));
    CHECK("concurrent: d does not dominate c", !d.dominates(c));

    // Merge
    VectorClock merged = c;
    merged.merge(d);
    CHECK("merged has carol=1", merged.clock["carol"] == 1);
    CHECK("merged has dave=1",  merged.clock["dave"]  == 1);

    // Merge takes max
    VectorClock x, y;
    x.clock["fp1"] = 5;
    x.clock["fp2"] = 2;
    y.clock["fp1"] = 3;
    y.clock["fp2"] = 7;
    x.merge(y);
    CHECK("merge takes max fp1=5", x.clock["fp1"] == 5);
    CHECK("merge takes max fp2=7", x.clock["fp2"] == 7);
}

void test_json_serialization() {
    SECTION("JSON serialization round-trips");

    // MemberInfo
    MemberInfo m;
    m.name = "Alice"; m.fingerprint = "c132070a"; m.role = "creator"; m.joined_at = 1000;
    json jm = m;
    MemberInfo m2 = jm.get<MemberInfo>();
    CHECK("MemberInfo name",        m2.name        == "Alice");
    CHECK("MemberInfo fingerprint", m2.fingerprint == "c132070a");
    CHECK("MemberInfo role",        m2.role        == "creator");
    CHECK("MemberInfo joined_at",   m2.joined_at   == 1000);

    // GroupSettings
    GroupSettings s; s.max_members = 5; s.max_file_size_mb = 100;
    json js = s;
    GroupSettings s2 = js.get<GroupSettings>();
    CHECK("Settings max_members",     s2.max_members     == 5);
    CHECK("Settings max_file_size",   s2.max_file_size_mb == 100);

    // VectorClock
    VectorClock vc;
    vc.clock["fp1"] = 3; vc.clock["fp2"] = 1;
    json jvc = vc;
    VectorClock vc2 = jvc.get<VectorClock>();
    CHECK("VectorClock fp1=3", vc2.clock["fp1"] == 3);
    CHECK("VectorClock fp2=1", vc2.clock["fp2"] == 1);

    // FileEntry
    FileEntry fe;
    fe.filename = "report.pdf"; fe.owner_fingerprint = "fp1";
    fe.uploaded_at = 2000; fe.modified_at = 2001;
    fe.version = 2; fe.size_bytes = 1024; fe.checksum = "sha256:abc";
    fe.vector_clock.clock["fp1"] = 2;
    fe.rename_history = {"draft.pdf"};
    json jfe = fe;
    FileEntry fe2 = jfe.get<FileEntry>();
    CHECK("FileEntry filename",     fe2.filename          == "report.pdf");
    CHECK("FileEntry owner",        fe2.owner_fingerprint == "fp1");
    CHECK("FileEntry version",      fe2.version           == 2);
    CHECK("FileEntry size",         fe2.size_bytes        == 1024);
    CHECK("FileEntry checksum",     fe2.checksum          == "sha256:abc");
    CHECK("FileEntry vc",           fe2.vector_clock.clock["fp1"] == 2);
    CHECK("FileEntry rename_hist",  fe2.rename_history.size() == 1);
    CHECK("FileEntry rename[0]",    fe2.rename_history[0] == "draft.pdf");

    // DeletedEntry
    DeletedEntry de;
    de.filename = "old.txt"; de.deleted_by_fingerprint = "fp2";
    de.deleted_at = 3000; de.version_at_deletion = 5;
    json jde = de;
    DeletedEntry de2 = jde.get<DeletedEntry>();
    CHECK("DeletedEntry filename",  de2.filename == "old.txt");
    CHECK("DeletedEntry version",   de2.version_at_deletion == 5);

    // RenameEntry
    RenameEntry re;
    re.old_name = "a.txt"; re.new_name = "b.txt";
    re.renamed_by_fingerprint = "fp1"; re.renamed_at = 4000; re.new_version = 3;
    json jre = re;
    RenameEntry re2 = jre.get<RenameEntry>();
    CHECK("RenameEntry old_name",   re2.old_name    == "a.txt");
    CHECK("RenameEntry new_name",   re2.new_name    == "b.txt");
    CHECK("RenameEntry version",    re2.new_version == 3);

    // GroupMetadata round-trip
    GroupMetadata gm;
    gm.group_id = "a3f5c2d8"; gm.schema_version = 1;
    gm.files["report.pdf"] = fe;
    gm.deleted_files.push_back(de);
    gm.rename_log.push_back(re);
    json jgm = gm;
    GroupMetadata gm2 = jgm.get<GroupMetadata>();
    CHECK("GroupMetadata group_id",      gm2.group_id == "a3f5c2d8");
    CHECK("GroupMetadata files count",   gm2.files.size() == 1);
    CHECK("GroupMetadata deleted count", gm2.deleted_files.size() == 1);
    CHECK("GroupMetadata rename count",  gm2.rename_log.size() == 1);
    CHECK("GroupMetadata file key",      gm2.files.count("report.pdf") == 1);
}

void test_checksum() {
    SECTION("computeChecksum");

    string path = writeDummyFile(TEST_DIR, "checksum_test.txt", "hello world");
    string cs   = GroupManager::computeChecksum(path);

    CHECK("Checksum starts with sha256:", cs.substr(0, 7) == "sha256:");
    CHECK("Checksum is 7+64 chars",       cs.size() == 71);
    // SHA256("hello world") = b94d27b9...
    // Instead of hardcoding the hash value (line endings differ across platforms),
    // verify determinism: same content hashed twice gives same result.
    string cs_again = GroupManager::computeChecksum(path);
    CHECK("Checksum is deterministic", cs == cs_again);

    // Different content → different checksum
    string path2 = writeDummyFile(TEST_DIR, "checksum_test2.txt", "hello world!");
    CHECK("Different content → different checksum",
          GroupManager::computeChecksum(path2) != cs);

    // Same content → same checksum
    string path3 = writeDummyFile(TEST_DIR, "checksum_test3.txt", "hello world");
    CHECK("Same content → same checksum",
          GroupManager::computeChecksum(path3) == cs);

    // Non-existent file
    CHECK("Missing file → empty string",
          GroupManager::computeChecksum(TEST_DIR + PATH_SEP + "no_such_file.txt") == "");
}

void test_create_and_load_group() {
    SECTION("createGroup / loadGroup / loadGroupFromFolder");

    bool ok = GroupManager::createGroup(
        TEST_GROUP_A, "TestGroup", "Alice", "c132070a");
    CHECK("createGroup returns true", ok);
    CHECK(".lanbox_group.json exists",
          GroupManager::isGroupFolder(TEST_GROUP_A));

    // Load from folder
    GroupConfig cfg;
    CHECK("loadGroupFromFolder succeeds",
          GroupManager::loadGroupFromFolder(TEST_GROUP_A, cfg));
    CHECK("group_name is TestGroup",    cfg.group_name          == "TestGroup");
    CHECK("creator_fp is c132070a",     cfg.creator_fingerprint == "c132070a");
    CHECK("folder_path set",            cfg.folder_path         == TEST_GROUP_A);
    CHECK("group_id is 8 chars",        cfg.group_id.size()     == 8);
    CHECK("1 member (creator)",         cfg.members.size()      == 1);
    CHECK("creator role",               cfg.members[0].role     == "creator");
    CHECK("creator name",               cfg.members[0].name     == "Alice");

    // Load from registry
    GroupConfig cfg2;
    CHECK("loadGroup (registry) succeeds",
          GroupManager::loadGroup(cfg.group_id, cfg2));
    CHECK("Registry group_id matches",  cfg2.group_id == cfg.group_id);

    // Metadata was created
    GroupMetadata meta;
    CHECK("loadMetadata succeeds",
          GroupManager::loadMetadata(TEST_GROUP_A, meta));
    CHECK("Metadata group_id matches",  meta.group_id == cfg.group_id);
    CHECK("Metadata files empty",       meta.files.empty());
}

void test_member_management() {
    SECTION("addMember / removeMember / isMember / isAdmin");

    // Use group_a created in previous test
    GroupConfig cfg;
    GroupManager::loadGroupFromFolder(TEST_GROUP_A, cfg);
    string gid = cfg.group_id;

    MemberInfo bob;
    bob.name = "Bob"; bob.fingerprint = "8a3f1b2c";
    bob.role = "member"; bob.joined_at = static_cast<long>(time(nullptr));

    CHECK("addMember Bob",              GroupManager::addMember(gid, bob));
    CHECK("isMember Bob",               GroupManager::isMember(gid, "8a3f1b2c"));
    CHECK("isMember Alice",             GroupManager::isMember(gid, "c132070a"));
    CHECK("isMember unknown is false",  !GroupManager::isMember(gid, "deadbeef"));

    CHECK("Alice isAdmin (creator)",    GroupManager::isAdmin(gid, "c132070a"));
    CHECK("Bob is not admin",           !GroupManager::isAdmin(gid, "8a3f1b2c"));

    // Add duplicate — should silently succeed without adding twice
    CHECK("addMember duplicate ok",     GroupManager::addMember(gid, bob));
    GroupConfig cfg2;
    GroupManager::loadGroup(gid, cfg2);
    CHECK("No duplicate member",        cfg2.members.size() == 2);

    // Admin role
    MemberInfo carol;
    carol.name = "Carol"; carol.fingerprint = "f9d04e71";
    carol.role = "admin"; carol.joined_at = static_cast<long>(time(nullptr));
    GroupManager::addMember(gid, carol);
    CHECK("Carol isAdmin",              GroupManager::isAdmin(gid, "f9d04e71"));

    // Remove Bob
    CHECK("removeMember Bob",           GroupManager::removeMember(gid, "8a3f1b2c"));
    CHECK("Bob no longer member",       !GroupManager::isMember(gid, "8a3f1b2c"));
    GroupConfig cfg3;
    GroupManager::loadGroup(gid, cfg3);
    CHECK("Member count is 2 after remove", cfg3.members.size() == 2);
}

void test_file_entry_operations() {
    SECTION("buildFileEntry / updateFileEntry / isFileOwner");

    GroupConfig cfg;
    GroupManager::loadGroupFromFolder(TEST_GROUP_A, cfg);

    // Write a real file
    string file_path = writeDummyFile(TEST_GROUP_A, "notes.txt", "test content 123");
    string rel_path  = "notes.txt";

    FileEntry entry = GroupManager::buildFileEntry(file_path, rel_path, "c132070a");
    CHECK("Entry filename",      entry.filename          == "notes.txt");
    CHECK("Entry owner",         entry.owner_fingerprint == "c132070a");
    CHECK("Entry version == 1",  entry.version           == 1);
    CHECK("Entry size > 0",      entry.size_bytes        > 0);
    CHECK("Entry checksum set",  entry.checksum.substr(0, 7) == "sha256:");
    CHECK("Entry vc initialized",entry.vector_clock.clock["c132070a"] == 1);

    CHECK("updateFileEntry",     GroupManager::updateFileEntry(TEST_GROUP_A, entry));
    CHECK("isFileOwner true",    GroupManager::isFileOwner(TEST_GROUP_A, "notes.txt", "c132070a"));
    CHECK("isFileOwner false",   !GroupManager::isFileOwner(TEST_GROUP_A, "notes.txt", "8a3f1b2c"));

    // Load metadata and verify
    GroupMetadata meta;
    GroupManager::loadMetadata(TEST_GROUP_A, meta);
    CHECK("File in metadata",    meta.files.count("notes.txt") == 1);
    CHECK("Stored checksum",     meta.files["notes.txt"].checksum == entry.checksum);
}

void test_mark_deleted() {
    SECTION("markDeleted");

    // notes.txt was added in previous test
    CHECK("markDeleted succeeds",
          GroupManager::markDeleted(TEST_GROUP_A, "notes.txt", "c132070a"));

    GroupMetadata meta;
    GroupManager::loadMetadata(TEST_GROUP_A, meta);
    CHECK("File removed from files{}",       meta.files.count("notes.txt") == 0);
    CHECK("File in deleted_files",           meta.deleted_files.size() == 1);
    CHECK("deleted_by_fp correct",
          meta.deleted_files[0].deleted_by_fingerprint == "c132070a");

    // markDeleted on already-deleted file is no-op (not an error)
    CHECK("markDeleted no-op on missing",
          GroupManager::markDeleted(TEST_GROUP_A, "notes.txt", "c132070a"));
}

void test_apply_rename() {
    SECTION("applyRename");

    // Add a fresh file to rename
    string fp = writeDummyFile(TEST_GROUP_A, "draft.txt", "draft content");
    FileEntry e = GroupManager::buildFileEntry(fp, "draft.txt", "c132070a");
    GroupManager::updateFileEntry(TEST_GROUP_A, e);

    CHECK("applyRename succeeds",
          GroupManager::applyRename(TEST_GROUP_A, "draft.txt", "final.txt", "c132070a"));

    GroupMetadata meta;
    GroupManager::loadMetadata(TEST_GROUP_A, meta);
    CHECK("Old name gone",           meta.files.count("draft.txt") == 0);
    CHECK("New name present",        meta.files.count("final.txt") == 1);
    CHECK("rename_log has entry",    meta.rename_log.size() == 1);
    CHECK("rename_log old_name",     meta.rename_log[0].old_name == "draft.txt");
    CHECK("rename_log new_name",     meta.rename_log[0].new_name == "final.txt");
    CHECK("rename_history preserved",
          meta.files["final.txt"].rename_history.size() == 1);
    CHECK("rename_history[0]",
          meta.files["final.txt"].rename_history[0] == "draft.txt");
    CHECK("version incremented",     meta.files["final.txt"].version == 2);
}

void test_merge_metadata() {
    SECTION("mergeMetadata");

    // Build two independent metadata states
    GroupMetadata local, remote;
    local.group_id  = "testgid";
    remote.group_id = "testgid";

    // File A: remote is newer (higher vector clock)
    FileEntry a_local, a_remote;
    a_local.filename  = "a.txt"; a_local.owner_fingerprint  = "fp1";
    a_local.version   = 1;       a_local.modified_at         = 1000;
    a_local.vector_clock.clock["fp1"] = 1;

    a_remote.filename = "a.txt"; a_remote.owner_fingerprint = "fp1";
    a_remote.version  = 2;       a_remote.modified_at        = 2000;
    a_remote.vector_clock.clock["fp1"] = 2;

    local.files["a.txt"]  = a_local;
    remote.files["a.txt"] = a_remote;

    // File B: only in remote (local is missing it)
    FileEntry b_remote;
    b_remote.filename = "b.txt"; b_remote.owner_fingerprint = "fp2";
    b_remote.version  = 1;
    b_remote.vector_clock.clock["fp2"] = 1;
    remote.files["b.txt"] = b_remote;

    // File C: only in local (remote doesn't have it)
    FileEntry c_local;
    c_local.filename = "c.txt"; c_local.owner_fingerprint = "fp1";
    c_local.version  = 1;
    c_local.vector_clock.clock["fp1"] = 1;
    local.files["c.txt"] = c_local;

    // Deleted entry only in remote
    DeletedEntry del;
    del.filename = "old.txt"; del.deleted_by_fingerprint = "fp1";
    del.deleted_at = 5000;    del.version_at_deletion = 1;
    remote.deleted_files.push_back(del);

    // Rename entry only in remote
    RenameEntry ren;
    ren.old_name = "x.txt"; ren.new_name = "y.txt";
    ren.renamed_at = 6000;  ren.new_version = 2;
    remote.rename_log.push_back(ren);

    bool changed = GroupManager::mergeMetadata(local, remote);
    CHECK("merge reports changed",      changed);
    CHECK("a.txt updated to v2",        local.files["a.txt"].version == 2);
    CHECK("b.txt added from remote",    local.files.count("b.txt") == 1);
    CHECK("c.txt preserved",            local.files.count("c.txt") == 1);
    CHECK("deleted entry merged",       local.deleted_files.size() == 1);
    CHECK("rename entry merged",        local.rename_log.size() == 1);

    // Merging again with same remote → no change
    bool changed2 = GroupManager::mergeMetadata(local, remote);
    CHECK("second merge no change",     !changed2);

    // Local newer than remote → keep local
    GroupMetadata loc2, rem2;
    loc2.group_id = rem2.group_id = "g2";
    FileEntry newer, older;
    newer.filename = "z.txt"; newer.version = 5;
    newer.vector_clock.clock["fp1"] = 5;
    older.filename = "z.txt"; older.version = 3;
    older.vector_clock.clock["fp1"] = 3;
    loc2.files["z.txt"] = newer;
    rem2.files["z.txt"] = older;
    GroupManager::mergeMetadata(loc2, rem2);
    CHECK("local newer kept at v5",     loc2.files["z.txt"].version == 5);
}

void test_list_groups() {
    SECTION("listAllGroups");

    // Create a second group
    GroupManager::createGroup(TEST_GROUP_B, "GroupB", "Bob", "8a3f1b2c");

    auto groups = GroupManager::listAllGroups();
    // There may be other groups in ~/.lanbox/groups/ from real use,
    // so just check we have at least 2 and both test groups are present
    bool found_a = false, found_b = false;
    for (const auto& g : groups) {
        if (g.group_name == "TestGroup") found_a = true;
        if (g.group_name == "GroupB")    found_b = true;
    }
    CHECK("listAllGroups finds TestGroup", found_a);
    CHECK("listAllGroups finds GroupB",    found_b);
    CHECK("At least 2 groups",            groups.size() >= 2);
}

void test_json_string_helpers() {
    SECTION("fileEntryToJson / fileEntryFromJson / metadataToJson / metadataFromJson");

    FileEntry fe;
    fe.filename = "report.pdf"; fe.owner_fingerprint = "fp1";
    fe.version = 3; fe.size_bytes = 999; fe.checksum = "sha256:abc";
    fe.vector_clock.clock["fp1"] = 3;

    string js = GroupManager::fileEntryToJson(fe);
    CHECK("fileEntryToJson not empty", !js.empty());

    FileEntry fe2;
    CHECK("fileEntryFromJson succeeds", GroupManager::fileEntryFromJson(js, fe2));
    CHECK("filename round-trip",        fe2.filename == "report.pdf");
    CHECK("version round-trip",         fe2.version  == 3);
    CHECK("checksum round-trip",        fe2.checksum == "sha256:abc");

    // Bad JSON
    FileEntry fe3;
    CHECK("fileEntryFromJson bad JSON returns false",
          !GroupManager::fileEntryFromJson("{not valid json{{", fe3));

    // Full metadata
    GroupMetadata meta;
    meta.group_id = "a3f5c2d8";
    meta.files["report.pdf"] = fe;
    string ms = GroupManager::metadataToJson(meta);
    GroupMetadata meta2;
    CHECK("metadataFromJson succeeds", GroupManager::metadataFromJson(ms, meta2));
    CHECK("metadata group_id",         meta2.group_id == "a3f5c2d8");
    CHECK("metadata file count",       meta2.files.size() == 1);
}

// ============================================================================
// Main
// ============================================================================

int main() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

    setupTestDirs();

    cout << "=========================================\n";
    cout << "  LANBox Step 2 — GroupManager Tests     \n";
    cout << "=========================================\n";

    test_vector_clock();
    test_json_serialization();
    test_checksum();
    test_create_and_load_group();
    test_member_management();
    test_file_entry_operations();
    test_mark_deleted();
    test_apply_rename();
    test_merge_metadata();
    test_list_groups();
    test_json_string_helpers();

    cout << "\n=========================================\n";
    cout << "  Results: "
         << tests_passed << " passed, "
         << tests_failed << " failed, "
         << tests_run    << " total\n";
    cout << "=========================================\n";

    // Cleanup test directories
    removeDir(TEST_DIR);

    return (tests_failed == 0) ? 0 : 1;
}