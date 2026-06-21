#include "platform.hpp"
#include "group_manager.hpp"
#include <bits/stdc++.h>
#include <openssl/rand.h>
#include <openssl/evp.h>

#ifdef _WIN32
    #include <windows.h>
#else
    #include <sys/stat.h>
    #include <sys/types.h>
    #include <dirent.h>
    #include <unistd.h>
    #include <pwd.h>
#endif

using namespace std;
using json = nlohmann::json;

// ============================================================================
// MemberInfo JSON
// ============================================================================

void to_json(json& j, const MemberInfo& m) {
    j = json{
        {"name",        m.name},
        {"fingerprint", m.fingerprint},
        {"role",        m.role},
        {"joined_at",   m.joined_at}
    };
}

void from_json(const json& j, MemberInfo& m) {
    m.name        = j.value("name",        "");
    m.fingerprint = j.value("fingerprint", "");
    m.role        = j.value("role",        "member");
    m.joined_at   = j.value("joined_at",   0L);
}

// ============================================================================
// GroupSettings JSON
// ============================================================================

void to_json(json& j, const GroupSettings& s) {
    j = json{
        {"max_members",                  s.max_members},
        {"max_file_size_mb",             s.max_file_size_mb},
        {"max_total_size_per_member_mb", s.max_total_size_per_member_mb},
        {"max_subdirectory_depth",       s.max_subdirectory_depth}
    };
}

void from_json(const json& j, GroupSettings& s) {
    s.max_members                  = j.value("max_members",                  10);
    s.max_file_size_mb             = j.value("max_file_size_mb",             50);
    s.max_total_size_per_member_mb = j.value("max_total_size_per_member_mb", 500);
    s.max_subdirectory_depth       = j.value("max_subdirectory_depth",       5);
}

// ============================================================================
// GroupConfig JSON
// ============================================================================

void to_json(json& j, const GroupConfig& g) {
    j = json{
        {"group_id",             g.group_id},
        {"group_name",           g.group_name},
        {"folder_path",          g.folder_path},
        {"created_at",           g.created_at},
        {"creator_fingerprint",  g.creator_fingerprint},
        {"members",              g.members},
        {"settings",             g.settings}
    };
}

void from_json(const json& j, GroupConfig& g) {
    g.group_id            = j.value("group_id",            "");
    g.group_name          = j.value("group_name",          "");
    g.folder_path         = j.value("folder_path",         "");
    g.created_at          = j.value("created_at",          0L);
    g.creator_fingerprint = j.value("creator_fingerprint", "");
    if (j.contains("members"))
        g.members = j["members"].get<vector<MemberInfo>>();
    if (j.contains("settings"))
        g.settings = j["settings"].get<GroupSettings>();
}

// ============================================================================
// VectorClock JSON + operations
// ============================================================================

void to_json(json& j, const VectorClock& vc) {
    j = json::object();
    for (const auto& [fp, cnt] : vc.clock) {
        j[fp] = cnt;
    }
}

void from_json(const json& j, VectorClock& vc) {
    vc.clock.clear();
    for (auto it = j.begin(); it != j.end(); ++it) {
        vc.clock[it.key()] = it.value().get<uint32_t>();
    }
}

void VectorClock::increment(const string& fingerprint) {
    clock[fingerprint]++;
}

void VectorClock::merge(const VectorClock& other) {
    for (const auto& [fp, cnt] : other.clock) {
        auto it = clock.find(fp);
        if (it == clock.end() || it->second < cnt) {
            clock[fp] = cnt;
        }
    }
}

bool VectorClock::dominates(const VectorClock& other) const {
    // this dominates other if for every entry in other,
    // this has an equal or greater value
    for (const auto& [fp, cnt] : other.clock) {
        auto it = clock.find(fp);
        if (it == clock.end() || it->second < cnt) {
            return false;
        }
    }
    return true;
}

bool VectorClock::equals(const VectorClock& other) const {
    return clock == other.clock;
}

// ============================================================================
// FileEntry JSON
// ============================================================================

void to_json(json& j, const FileEntry& f) {
    j = json{
        {"filename",          f.filename},
        {"owner_fingerprint", f.owner_fingerprint},
        {"uploaded_at",       f.uploaded_at},
        {"modified_at",       f.modified_at},
        {"version",           f.version},
        {"size_bytes",        f.size_bytes},
        {"checksum",          f.checksum},
        {"vector_clock",      f.vector_clock},
        {"rename_history",    f.rename_history},
        {"chunk_size_bytes",  f.chunk_size_bytes}
    };
}

void from_json(const json& j, FileEntry& f) {
    f.filename          = j.value("filename",          "");
    f.owner_fingerprint = j.value("owner_fingerprint", "");
    f.uploaded_at       = j.value("uploaded_at",       0L);
    f.modified_at       = j.value("modified_at",       0L);
    f.version           = j.value("version",           1u);
    f.size_bytes        = j.value("size_bytes",        0ULL);
    f.checksum          = j.value("checksum",          "");
    f.chunk_size_bytes  = j.value("chunk_size_bytes",  65536u);
    if (j.contains("vector_clock"))
        f.vector_clock = j["vector_clock"].get<VectorClock>();
    if (j.contains("rename_history"))
        f.rename_history = j["rename_history"].get<vector<string>>();
}

// ============================================================================
// DeletedEntry JSON
// ============================================================================

void to_json(json& j, const DeletedEntry& d) {
    j = json{
        {"filename",                d.filename},
        {"deleted_by_fingerprint",  d.deleted_by_fingerprint},
        {"deleted_at",              d.deleted_at},
        {"version_at_deletion",     d.version_at_deletion}
    };
}

void from_json(const json& j, DeletedEntry& d) {
    d.filename                = j.value("filename",                "");
    d.deleted_by_fingerprint  = j.value("deleted_by_fingerprint",  "");
    d.deleted_at              = j.value("deleted_at",              0L);
    d.version_at_deletion     = j.value("version_at_deletion",     0u);
}

// ============================================================================
// RenameEntry JSON
// ============================================================================

void to_json(json& j, const RenameEntry& r) {
    j = json{
        {"old_name",                r.old_name},
        {"new_name",                r.new_name},
        {"renamed_by_fingerprint",  r.renamed_by_fingerprint},
        {"renamed_at",              r.renamed_at},
        {"new_version",             r.new_version}
    };
}

void from_json(const json& j, RenameEntry& r) {
    r.old_name               = j.value("old_name",               "");
    r.new_name               = j.value("new_name",               "");
    r.renamed_by_fingerprint = j.value("renamed_by_fingerprint", "");
    r.renamed_at             = j.value("renamed_at",             0L);
    r.new_version            = j.value("new_version",            1u);
}

// ============================================================================
// GroupMetadata JSON
// ============================================================================

void to_json(json& j, const GroupMetadata& m) {
    j = json{
        {"group_id",       m.group_id},
        {"schema_version", m.schema_version},
        {"deleted_files",  m.deleted_files},
        {"rename_log",     m.rename_log}
    };
    // files is a map — serialize as JSON object keyed by filename
    json files_obj = json::object();
    for (const auto& [name, entry] : m.files) {
        files_obj[name] = entry;
    }
    j["files"] = files_obj;
}

void from_json(const json& j, GroupMetadata& m) {
    m.group_id       = j.value("group_id",       "");
    m.schema_version = j.value("schema_version", 1);
    m.files.clear();
    if (j.contains("files") && j["files"].is_object()) {
        for (auto it = j["files"].begin(); it != j["files"].end(); ++it) {
            FileEntry entry = it.value().get<FileEntry>();
            entry.filename  = it.key();   // ensure filename field matches map key
            m.files[it.key()] = entry;
        }
    }
    if (j.contains("deleted_files"))
        m.deleted_files = j["deleted_files"].get<vector<DeletedEntry>>();
    if (j.contains("rename_log"))
        m.rename_log = j["rename_log"].get<vector<RenameEntry>>();
}

// ============================================================================
// Path helpers
// ============================================================================

string GroupManager::getLanboxDir() {
#ifdef _WIN32
    char path[MAX_PATH];
    DWORD len = GetEnvironmentVariableA("USERPROFILE", path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        // Fallback: try HOMEDRIVE + HOMEPATH
        char drive[64] = {}, homepath[MAX_PATH] = {};
        GetEnvironmentVariableA("HOMEDRIVE", drive, sizeof(drive));
        GetEnvironmentVariableA("HOMEPATH",  homepath, sizeof(homepath));
        return string(drive) + string(homepath) + "\\.lanbox";
    }
    return string(path) + "\\.lanbox";
#else
    const char* home = getenv("HOME");
    if (!home) {
        struct passwd* pw = getpwuid(getuid());
        if (pw) home = pw->pw_dir;
    }
    return string(home ? home : "/tmp") + "/.lanbox";
#endif
}

string GroupManager::getGroupsDir() {
    return getLanboxDir() + 
#ifdef _WIN32
        "\\" + GROUPS_SUBDIR;
#else
        "/" + GROUPS_SUBDIR;
#endif
}

string GroupManager::getGroupRegistryPath(const string& group_id) {
    return getGroupsDir() +
#ifdef _WIN32
        "\\" + group_id + ".json";
#else
        "/" + group_id + ".json";
#endif
}

string GroupManager::getGroupConfigPath(const string& folder_path) {
    return folder_path +
#ifdef _WIN32
        "\\" + GROUP_CONFIG_FILENAME;
#else
        "/" + GROUP_CONFIG_FILENAME;
#endif
}

string GroupManager::getMetadataPath(const string& folder_path) {
    return folder_path +
#ifdef _WIN32
        "\\" + GROUP_METADATA_FILENAME;
#else
        "/" + GROUP_METADATA_FILENAME;
#endif
}

// ============================================================================
// Directory utilities
// ============================================================================

bool GroupManager::mkdirP(const string& path) {
#ifdef _WIN32
    // Walk the path creating each component
    string current;
    for (size_t i = 0; i < path.size(); ++i) {
        char c = path[i];
        current += c;
        if ((c == '\\' || c == '/') && current.size() > 1) {
            string dir = current.substr(0, current.size() - 1);
            CreateDirectoryA(dir.c_str(), nullptr);
            // Ignore error — directory may already exist
        }
    }
    return CreateDirectoryA(path.c_str(), nullptr) != 0
           || GetLastError() == ERROR_ALREADY_EXISTS;
#else
    // Walk and create each component
    string current;
    for (size_t i = 0; i < path.size(); ++i) {
        current += path[i];
        if (path[i] == '/' && current.size() > 1) {
            mkdir(current.c_str(), 0755);
        }
    }
    return mkdir(path.c_str(), 0755) == 0 || errno == EEXIST;
#endif
}

bool GroupManager::ensureLanboxDirs() {
    return mkdirP(getLanboxDir()) && mkdirP(getGroupsDir());
}

// ============================================================================
// Group ID generation
// ============================================================================

string GroupManager::generateGroupId() {
    unsigned char bytes[4];
    if (RAND_bytes(bytes, sizeof(bytes)) != 1) {
        // Fallback: use time + pid
        srand(static_cast<unsigned>(time(nullptr)));
        for (auto& b : bytes) b = static_cast<unsigned char>(rand() & 0xFF);
    }
    char hex[9];
    snprintf(hex, sizeof(hex), "%02x%02x%02x%02x",
             bytes[0], bytes[1], bytes[2], bytes[3]);
    return string(hex);
}

// ============================================================================
// Atomic JSON write helper (internal)
// ============================================================================

static bool atomicWriteJson(const string& path, const json& j) {
    string tmp = path + ".tmp";
    ofstream f(tmp);
    if (!f.is_open()) return false;
    f << j.dump(4);
    f.close();
    // Atomic rename
#ifdef _WIN32
    // On Windows, destination must not exist for rename to be atomic
    DeleteFileA(path.c_str());
    return MoveFileA(tmp.c_str(), path.c_str()) != 0;
#else
    return rename(tmp.c_str(), path.c_str()) == 0;
#endif
}

// ============================================================================
// Group create / load / save
// ============================================================================

bool GroupManager::createGroup(const string& folder_path,
                               const string& group_name,
                               const string& creator_name,
                               const string& creator_fingerprint) {
    if (!mkdirP(folder_path))      return false;
    if (!ensureLanboxDirs())       return false;

    GroupConfig cfg;
    cfg.group_id            = generateGroupId();
    cfg.group_name          = group_name;
    cfg.folder_path         = folder_path;
    cfg.created_at          = static_cast<long>(time(nullptr));
    cfg.creator_fingerprint = creator_fingerprint;

    MemberInfo creator;
    creator.name        = creator_name;
    creator.fingerprint = creator_fingerprint;
    creator.role        = "creator";
    creator.joined_at   = cfg.created_at;
    cfg.members.push_back(creator);

    // Write .lanbox_group.json into the folder
    if (!atomicWriteJson(getGroupConfigPath(folder_path), json(cfg))) return false;

    // Write empty .lanbox_metadata.json
    GroupMetadata meta;
    meta.group_id = cfg.group_id;
    if (!atomicWriteJson(getMetadataPath(folder_path), json(meta)))  return false;

    // Save to registry
    if (!atomicWriteJson(getGroupRegistryPath(cfg.group_id), json(cfg))) return false;

    return true;
}

bool GroupManager::loadGroup(const string& group_id, GroupConfig& out) {
    string path = getGroupRegistryPath(group_id);
    ifstream f(path);
    if (!f.is_open()) return false;
    try {
        json j; f >> j;
        out = j.get<GroupConfig>();
        return true;
    } catch (...) {
        return false;
    }
}

bool GroupManager::loadGroupFromFolder(const string& folder_path, GroupConfig& out) {
    string path = getGroupConfigPath(folder_path);
    ifstream f(path);
    if (!f.is_open()) return false;
    try {
        json j; f >> j;
        out = j.get<GroupConfig>();
        return true;
    } catch (...) {
        return false;
    }
}

bool GroupManager::saveGroup(const GroupConfig& cfg) {
    json j = cfg;
    return atomicWriteJson(getGroupConfigPath(cfg.folder_path), j)
        && atomicWriteJson(getGroupRegistryPath(cfg.group_id), j);
}

vector<GroupConfig> GroupManager::listAllGroups() {
    vector<GroupConfig> result;
    string dir = getGroupsDir();

#ifdef _WIN32
    WIN32_FIND_DATAA fd;
    string pattern = dir + "\\*.json";
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return result;
    do {
        string path = dir + "\\" + fd.cFileName;
        ifstream f(path);
        if (!f.is_open()) continue;
        try {
            json j; f >> j;
            result.push_back(j.get<GroupConfig>());
        } catch (...) {}
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR* d = opendir(dir.c_str());
    if (!d) return result;
    struct dirent* entry;
    while ((entry = readdir(d)) != nullptr) {
        string name(entry->d_name);
        if (name.size() < 5) continue;
        if (name.substr(name.size() - 5) != ".json") continue;
        string path = dir + "/" + name;
        ifstream f(path);
        if (!f.is_open()) continue;
        try {
            json j; f >> j;
            result.push_back(j.get<GroupConfig>());
        } catch (...) {}
    }
    closedir(d);
#endif
    return result;
}

bool GroupManager::isGroupFolder(const string& folder_path) {
    ifstream f(getGroupConfigPath(folder_path));
    return f.is_open();
}

// ============================================================================
// Member management
// ============================================================================

bool GroupManager::addMember(const string& group_id, const MemberInfo& member) {
    GroupConfig cfg;
    if (!loadGroup(group_id, cfg)) return false;
    // Check for duplicate fingerprint
    for (const auto& m : cfg.members) {
        if (m.fingerprint == member.fingerprint) return true; // already member
    }
    cfg.members.push_back(member);
    return saveGroup(cfg);
}

bool GroupManager::removeMember(const string& group_id, const string& fingerprint) {
    GroupConfig cfg;
    if (!loadGroup(group_id, cfg)) return false;
    auto& mv = cfg.members;
    mv.erase(remove_if(mv.begin(), mv.end(),
        [&](const MemberInfo& m){ return m.fingerprint == fingerprint; }), mv.end());
    return saveGroup(cfg);
}

bool GroupManager::isMember(const string& group_id, const string& fingerprint) {
    GroupConfig cfg;
    if (!loadGroup(group_id, cfg)) return false;
    for (const auto& m : cfg.members) {
        if (m.fingerprint == fingerprint) return true;
    }
    return false;
}

bool GroupManager::isAdmin(const string& group_id, const string& fingerprint) {
    GroupConfig cfg;
    if (!loadGroup(group_id, cfg)) return false;
    for (const auto& m : cfg.members) {
        if (m.fingerprint == fingerprint) {
            return m.role == "creator" || m.role == "admin";
        }
    }
    return false;
}

bool GroupManager::isFileOwner(const string& folder_path,
                               const string& filename,
                               const string& fingerprint) {
    GroupMetadata meta;
    if (!loadMetadata(folder_path, meta)) return false;
    auto it = meta.files.find(filename);
    if (it == meta.files.end()) return false;
    return it->second.owner_fingerprint == fingerprint;
}

// ============================================================================
// Metadata load / save
// ============================================================================

bool GroupManager::loadMetadata(const string& folder_path, GroupMetadata& out) {
    string path = getMetadataPath(folder_path);
    ifstream f(path);
    if (!f.is_open()) return false;
    try {
        json j; f >> j;
        out = j.get<GroupMetadata>();
        return true;
    } catch (...) {
        return false;
    }
}

bool GroupManager::saveMetadata(const string& folder_path, const GroupMetadata& meta) {
    return atomicWriteJson(getMetadataPath(folder_path), json(meta));
}

// ============================================================================
// File entry operations
// ============================================================================

string GroupManager::computeChecksum(const string& file_path) {
    ifstream f(file_path, ios::binary);
    if (!f.is_open()) return "";

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return "";

    if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1) {
        EVP_MD_CTX_free(ctx);
        return "";
    }

    char buf[65536];
    while (f.read(buf, sizeof(buf)) || f.gcount() > 0) {
        if (EVP_DigestUpdate(ctx, buf, static_cast<size_t>(f.gcount())) != 1) {
            EVP_MD_CTX_free(ctx);
            return "";
        }
    }

    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int  hash_len = 0;
    if (EVP_DigestFinal_ex(ctx, hash, &hash_len) != 1) {
        EVP_MD_CTX_free(ctx);
        return "";
    }
    EVP_MD_CTX_free(ctx);

    char hex[EVP_MAX_MD_SIZE * 2 + 1];
    for (unsigned int i = 0; i < hash_len; ++i) {
        snprintf(hex + i * 2, 3, "%02x", hash[i]);
    }
    return "sha256:" + string(hex);
}

FileEntry GroupManager::buildFileEntry(const string& file_path,
                                       const string& relative_path,
                                       const string& owner_fingerprint) {
    FileEntry entry;
    entry.filename          = relative_path;
    entry.owner_fingerprint = owner_fingerprint;
    entry.version           = 1;
    entry.chunk_size_bytes  = 65536;
    entry.rename_history    = {};

    long now = static_cast<long>(time(nullptr));
    entry.uploaded_at = now;
    entry.modified_at = now;

    // File size
    ifstream f(file_path, ios::binary | ios::ate);
    entry.size_bytes = f.is_open()
                       ? static_cast<uint64_t>(f.tellg())
                       : 0;
    f.close();

    entry.checksum = computeChecksum(file_path);

    // Init vector clock: owner has count 1
    entry.vector_clock.clock[owner_fingerprint] = 1;

    return entry;
}

bool GroupManager::updateFileEntry(const string& folder_path, const FileEntry& entry) {
    GroupMetadata meta;
    if (!loadMetadata(folder_path, meta)) return false;
    meta.files[entry.filename] = entry;
    return saveMetadata(folder_path, meta);
}

bool GroupManager::markDeleted(const string& folder_path,
                               const string& filename,
                               const string& deleter_fingerprint) {
    GroupMetadata meta;
    if (!loadMetadata(folder_path, meta)) return false;

    auto it = meta.files.find(filename);
    if (it == meta.files.end()) return true; // already gone, no-op

    DeletedEntry del;
    del.filename               = filename;
    del.deleted_by_fingerprint = deleter_fingerprint;
    del.deleted_at             = static_cast<long>(time(nullptr));
    del.version_at_deletion    = it->second.version;

    meta.deleted_files.push_back(del);
    meta.files.erase(it);

    return saveMetadata(folder_path, meta);
}

bool GroupManager::applyRename(const string& folder_path,
                               const string& old_name,
                               const string& new_name,
                               const string& renamer_fingerprint) {
    GroupMetadata meta;
    if (!loadMetadata(folder_path, meta)) return false;

    auto it = meta.files.find(old_name);
    if (it == meta.files.end()) return false;

    FileEntry entry = it->second;
    entry.rename_history.push_back(old_name);   // preserve history
    entry.filename    = new_name;
    entry.modified_at = static_cast<long>(time(nullptr));
    entry.version++;
    entry.vector_clock.increment(renamer_fingerprint);

    RenameEntry re;
    re.old_name               = old_name;
    re.new_name               = new_name;
    re.renamed_by_fingerprint = renamer_fingerprint;
    re.renamed_at             = entry.modified_at;
    re.new_version            = entry.version;

    meta.files.erase(it);
    meta.files[new_name] = entry;
    meta.rename_log.push_back(re);

    return saveMetadata(folder_path, meta);
}

// ============================================================================
// Metadata merge
// ============================================================================

bool GroupManager::mergeMetadata(GroupMetadata& local, const GroupMetadata& remote) {
    bool changed = false;

    // ── Merge file entries ──────────────────────────────────────────────────
    for (const auto& [name, remote_entry] : remote.files) {
        auto it = local.files.find(name);

        if (it == local.files.end()) {
            // Local doesn't have this file at all
            local.files[name] = remote_entry;
            changed = true;
            continue;
        }

        FileEntry& local_entry = it->second;
        bool remote_dom = remote_entry.vector_clock.dominates(local_entry.vector_clock);
        bool local_dom  = local_entry.vector_clock.dominates(remote_entry.vector_clock);

        if (remote_dom && !local_dom) {
            // Remote is strictly newer
            local_entry = remote_entry;
            changed = true;
        } else if (!local_dom && !remote_dom) {
            // Concurrent write — tiebreak
            if (remote_entry.version > local_entry.version ||
               (remote_entry.version == local_entry.version &&
                remote_entry.modified_at > local_entry.modified_at)) {
                local_entry = remote_entry;
                changed = true;
            }
        }
        // else: local dominates or equal — keep local, no change
    }

    // ── Merge deleted_files (union) ─────────────────────────────────────────
    for (const auto& remote_del : remote.deleted_files) {
        bool found = false;
        for (const auto& local_del : local.deleted_files) {
            if (local_del.filename == remote_del.filename &&
                local_del.deleted_at == remote_del.deleted_at) {
                found = true;
                break;
            }
        }
        if (!found) {
            local.deleted_files.push_back(remote_del);
            // Also remove from files{} if present with older version
            auto fit = local.files.find(remote_del.filename);
            if (fit != local.files.end() &&
                fit->second.version <= remote_del.version_at_deletion) {
                local.files.erase(fit);
            }
            changed = true;
        }
    }

    // ── Merge rename_log (union, keyed by old+new+renamed_at) ──────────────
    for (const auto& remote_re : remote.rename_log) {
        bool found = false;
        for (const auto& local_re : local.rename_log) {
            if (local_re.old_name   == remote_re.old_name &&
                local_re.new_name   == remote_re.new_name &&
                local_re.renamed_at == remote_re.renamed_at) {
                found = true;
                break;
            }
        }
        if (!found) {
            local.rename_log.push_back(remote_re);
            changed = true;
        }
    }

    return changed;
}

// ============================================================================
// JSON serialization helpers
// ============================================================================

string GroupManager::fileEntryToJson(const FileEntry& entry) {
    return json(entry).dump(4);
}

string GroupManager::metadataToJson(const GroupMetadata& meta) {
    return json(meta).dump(4);
}

bool GroupManager::fileEntryFromJson(const string& json_str, FileEntry& out) {
    try {
        json j = json::parse(json_str);
        out = j.get<FileEntry>();
        return true;
    } catch (...) {
        return false;
    }
}

bool GroupManager::metadataFromJson(const string& json_str, GroupMetadata& out) {
    try {
        json j = json::parse(json_str);
        out = j.get<GroupMetadata>();
        return true;
    } catch (...) {
        return false;
    }
}