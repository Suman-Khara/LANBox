#pragma once

#include <string>
#include <vector>
#include <map>
#include <cstdint>
#include "json.hpp"

using json = nlohmann::json;
using namespace std;

// ============================================================================
// Constants
// ============================================================================

const string GROUP_CONFIG_FILENAME   = ".lanbox_group.json";
const string GROUP_METADATA_FILENAME = ".lanbox_metadata.json";
const string GROUP_META_TMP_SUFFIX   = ".lanbox_metadata.json.tmp";
const string LANBOX_DIR              = ".lanbox";  // under home dir
const string GROUPS_SUBDIR           = "groups";   // ~/.lanbox/groups/
const string SYNC_LOG_FILENAME       = "sync.log";
const string STATE_FILENAME          = "state.json";
const string PID_FILENAME            = "lanbox.pid";

// Filenames the folder watcher must always ignore
const vector<string> IGNORED_FILENAMES = {
    ".lanbox_group.json",
    ".lanbox_metadata.json",
    ".lanbox_metadata.json.tmp",
};

// Temp file prefixes (also ignored by watcher)
const string RECV_TMP_PREFIX = ".lanbox_recv_tmp_";
const string SEND_TMP_PREFIX = ".lanbox_send_tmp_";

// ============================================================================
// MemberInfo
// ============================================================================

struct MemberInfo {
    string name;
    string fingerprint;
    string role;        // "creator" | "admin" | "member"
    long   joined_at;
};

void to_json(json& j, const MemberInfo& m);
void from_json(const json& j, MemberInfo& m);

// ============================================================================
// GroupSettings
// ============================================================================

struct GroupSettings {
    int max_members                  = 10;
    int max_file_size_mb             = 50;
    int max_total_size_per_member_mb = 500;
    int max_subdirectory_depth       = 5;   // max 20, enforced by watcher + startup scan
};

void to_json(json& j, const GroupSettings& s);
void from_json(const json& j, GroupSettings& s);

// ============================================================================
// GroupConfig  (.lanbox_group.json)
// ============================================================================

struct GroupConfig {
    string             group_id;
    string             group_name;
    string             folder_path;
    long               created_at;
    string             creator_fingerprint;
    vector<MemberInfo> members;
    GroupSettings      settings;
};

void to_json(json& j, const GroupConfig& g);
void from_json(const json& j, GroupConfig& g);

// ============================================================================
// VectorClock
// ============================================================================

struct VectorClock {
    map<string, uint32_t> clock;   // fingerprint -> increment count

    // Increment this device's entry
    void increment(const string& fingerprint);

    // Merge another clock into this one (take max of each entry)
    void merge(const VectorClock& other);

    // Returns true if THIS clock is strictly greater than or equal to other
    // on every entry (i.e. this dominates or equals other)
    bool dominates(const VectorClock& other) const;

    // Returns true if both clocks are identical
    bool equals(const VectorClock& other) const;
};

void to_json(json& j, const VectorClock& vc);
void from_json(const json& j, VectorClock& vc);

// ============================================================================
// FileEntry  (one entry in GroupMetadata::files)
// ============================================================================

struct FileEntry {
    string      filename;
    string      owner_fingerprint;
    long        uploaded_at;
    long        modified_at;
    uint32_t    version;
    uint64_t    size_bytes;
    string      checksum;           // "sha256:<64 hex chars>"
    VectorClock vector_clock;
    vector<string> rename_history;  // Previous names, oldest first
    uint32_t    chunk_size_bytes = 65536;
};

void to_json(json& j, const FileEntry& f);
void from_json(const json& j, FileEntry& f);

// ============================================================================
// DeletedEntry  (entries in GroupMetadata::deleted_files)
// ============================================================================

struct DeletedEntry {
    string   filename;
    string   deleted_by_fingerprint;
    long     deleted_at;
    uint32_t version_at_deletion;
};

void to_json(json& j, const DeletedEntry& d);
void from_json(const json& j, DeletedEntry& d);

// ============================================================================
// RenameEntry  (entries in GroupMetadata::rename_log)
// ============================================================================

struct RenameEntry {
    string   old_name;
    string   new_name;
    string   renamed_by_fingerprint;
    long     renamed_at;
    uint32_t new_version;
};

void to_json(json& j, const RenameEntry& r);
void from_json(const json& j, RenameEntry& r);

// ============================================================================
// GroupMetadata  (.lanbox_metadata.json)
// ============================================================================

struct GroupMetadata {
    string                   group_id;
    int                      schema_version = 1;
    map<string, FileEntry>   files;          // key = filename (relative path)
    vector<DeletedEntry>     deleted_files;
    vector<RenameEntry>      rename_log;
};

void to_json(json& j, const GroupMetadata& m);
void from_json(const json& j, GroupMetadata& m);

// ============================================================================
// GroupManager
// ============================================================================

class GroupManager {
public:

    // ── Path helpers ──────────────────────────────────────────────────────

    // Returns ~/.lanbox  (cross-platform)
    static string getLanboxDir();

    // Returns ~/.lanbox/groups/
    static string getGroupsDir();

    // Returns ~/.lanbox/groups/<group_id>.json
    static string getGroupRegistryPath(const string& group_id);

    // Returns <folder_path>/.lanbox_group.json
    static string getGroupConfigPath(const string& folder_path);

    // Returns <folder_path>/.lanbox_metadata.json
    static string getMetadataPath(const string& folder_path);

    // ── Directory utilities ───────────────────────────────────────────────

    // Create directory and all parents. Returns true on success or if exists.
    static bool mkdirP(const string& path);

    // Ensure ~/.lanbox/ and ~/.lanbox/groups/ exist
    static bool ensureLanboxDirs();

    // ── Group ID ──────────────────────────────────────────────────────────

    // Generate a random 8-character hex group ID using OpenSSL RAND_bytes
    static string generateGroupId();

    // ── Group create / load / save ────────────────────────────────────────

    // Create a new group:
    //   - creates folder_path if it doesn't exist
    //   - writes .lanbox_group.json into folder_path
    //   - writes empty .lanbox_metadata.json into folder_path
    //   - saves a copy to ~/.lanbox/groups/<id>.json
    // Returns true on success.
    static bool createGroup(const string& folder_path,
                            const string& group_name,
                            const string& creator_name,
                            const string& creator_fingerprint);

    // Load group config from ~/.lanbox/groups/<group_id>.json
    static bool loadGroup(const string& group_id, GroupConfig& out);

    // Load group config directly from <folder_path>/.lanbox_group.json
    static bool loadGroupFromFolder(const string& folder_path, GroupConfig& out);

    // Save group config to BOTH:
    //   <cfg.folder_path>/.lanbox_group.json
    //   ~/.lanbox/groups/<cfg.group_id>.json
    // Uses atomic write (tmp → rename).
    static bool saveGroup(const GroupConfig& cfg);

    // Returns all groups this device is registered in
    // (reads all .json files from ~/.lanbox/groups/)
    static vector<GroupConfig> listAllGroups();

    // Returns true if folder_path contains a .lanbox_group.json
    static bool isGroupFolder(const string& folder_path);

    // ── Member management ─────────────────────────────────────────────────

    static bool addMember(const string& group_id, const MemberInfo& member);
    static bool removeMember(const string& group_id, const string& fingerprint);
    static bool isMember(const string& group_id, const string& fingerprint);

    // Returns true if fingerprint has role "creator" or "admin"
    static bool isAdmin(const string& group_id, const string& fingerprint);

    // Returns true if fingerprint is the owner of filename in this group's metadata
    static bool isFileOwner(const string& folder_path,
                            const string& filename,
                            const string& fingerprint);

    // ── Metadata load / save ──────────────────────────────────────────────

    static bool loadMetadata(const string& folder_path, GroupMetadata& out);

    // Atomic write: writes to .tmp then renames to final
    static bool saveMetadata(const string& folder_path, const GroupMetadata& meta);

    // ── File entry operations ─────────────────────────────────────────────

    // Compute SHA256 of a file on disk. Returns "sha256:<hex>" or "" on error.
    static string computeChecksum(const string& file_path);

    // Build a fresh FileEntry for a file on disk (owner = owner_fingerprint, version = 1)
    static FileEntry buildFileEntry(const string& file_path,
                                    const string& relative_path,
                                    const string& owner_fingerprint);

    // Add or replace a file entry in the metadata, then save atomically.
    static bool updateFileEntry(const string& folder_path, const FileEntry& entry);

    // Mark a file as deleted: move from files{} to deleted_files[], save.
    // Does nothing if filename is not in files{}.
    static bool markDeleted(const string& folder_path,
                            const string& filename,
                            const string& deleter_fingerprint);

    // Add a rename event: move entry key, append to rename_log, save.
    static bool applyRename(const string& folder_path,
                            const string& old_name,
                            const string& new_name,
                            const string& renamer_fingerprint);

    // ── Metadata merge (vector clock) ─────────────────────────────────────

    // Merge remote into local in-place.
    // Rules per file entry:
    //   remote dominates local  → take remote
    //   local dominates remote  → keep local
    //   concurrent (neither dominates) → higher version wins;
    //                                    tie → higher modified_at wins
    // Deleted entries and rename log entries are union-merged.
    // Returns true if local was modified.
    static bool mergeMetadata(GroupMetadata& local, const GroupMetadata& remote);

    // ── JSON serialization helpers ────────────────────────────────────────

    // Serialize a single FileEntry to JSON string (for SYNC_META_DELTA)
    static string fileEntryToJson(const FileEntry& entry);

    // Serialize full GroupMetadata to JSON string (for SYNC_META_REQUEST response)
    static string metadataToJson(const GroupMetadata& meta);

    // Parse a JSON string into a FileEntry (for applying a received delta)
    // Returns false if parsing fails.
    static bool fileEntryFromJson(const string& json_str, FileEntry& out);

    // Parse a JSON string into a GroupMetadata (for applying a full metadata dump)
    static bool metadataFromJson(const string& json_str, GroupMetadata& out);
};