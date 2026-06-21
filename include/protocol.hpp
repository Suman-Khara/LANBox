#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <openssl/sha.h>

using namespace std;

// ============================================================================
// Protocol constants
// ============================================================================

const uint32_t LANBOX_MAGIC    = 0x4C424F58;  // "LBOX"
const uint16_t PROTOCOL_VERSION = 0x0001;
const size_t   HEADER_SIZE      = 32;

// Raised from 8192 to 65536 to accommodate group JSON payloads in
// GROUP_JOIN_APPROVE, GROUP_INVITE, SYNC_META_DELTA etc.
// NOTE: The signaling receive buffer in sync_manager.cpp must also be 65536.
// The discovery listener in discovery.cpp reuses this constant — its buffer
// is already stack-allocated at 8192; update that buffer to 65536 as well.
const size_t MAX_PAYLOAD_SIZE  = 65536;

// ============================================================================
// Message types
// ============================================================================

enum class MessageType : uint16_t {

    // ── Phase 1-3 (existing) ────────────────────────────────────────────────
    DISCOVERY_REQUEST  = 0x0001,
    DISCOVERY_RESPONSE = 0x0002,
    HEARTBEAT          = 0x0003,

    FILE_TRANSFER_REQ  = 0x0100,
    FILE_CHUNK         = 0x0101,
    FILE_ACK           = 0x0102,

    PEER_INFO_REQUEST  = 0x0200,
    PEER_INFO_RESPONSE = 0x0201,

    // ── Phase 4: Sync signaling (UDP) ───────────────────────────────────────

    // File sync
    SYNC_NOTIFY        = 0x0300,  // "I have a new/updated file"
    SYNC_REQUEST       = 0x0301,  // "I need file X at version Y"
    SYNC_OFFER         = 0x0302,  // "I have file X, I can send it"
    SYNC_OFFER_ACCEPT  = 0x0303,  // "Yes please send it to me"
    SYNC_DECLINE       = 0x0304,  // "Offer no longer needed (already served)"
    SYNC_DELETE_NOTIFY = 0x0305,  // "File X was deleted"
    SYNC_RENAME_NOTIFY = 0x0306,  // "File X was renamed to Y"

    // Metadata sync
    SYNC_META_REQUEST  = 0x0307,  // "Send me your full metadata for this group"
    SYNC_META_DELTA    = 0x0308,  // "Here is a metadata patch (one file entry)"

    // Pause / resume coordination
    SYNC_PAUSE_NOTIFY  = 0x0309,  // "I am pausing sync (hard or soft)"
    SYNC_PAUSE_ACK     = 0x030A,  // "Pause acknowledged, cleaning up"
    SYNC_RESUME_NOTIFY = 0x030B,  // "I am resuming sync"

    // ── Phase 4: Group management (UDP) ─────────────────────────────────────
    GROUP_INVITE       = 0x0400,  // Admin sends invite to a specific peer
    GROUP_INVITE_ACK   = 0x0401,  // Peer accepts or rejects the invite
    GROUP_JOIN_REQUEST = 0x0402,  // Peer requests to join by group_id
    GROUP_JOIN_APPROVE = 0x0403,  // Admin approves join request
    GROUP_JOIN_DENY    = 0x0404,  // Admin denies join request
    GROUP_KICK         = 0x0405,  // Admin removes a member
};

// ============================================================================
// Header (existing, unchanged — 32 bytes)
// ============================================================================

struct __attribute__((packed)) LANBoxHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t message_type;
    uint32_t sequence_number;
    uint32_t payload_length;
    uint64_t timestamp;
    uint32_t sender_ip;
    uint32_t checksum;
};

static_assert(sizeof(LANBoxHeader) == HEADER_SIZE, "Header size must be 32 bytes");

// ============================================================================
// Existing payload structs (unchanged)
// ============================================================================

struct __attribute__((packed)) DiscoveryPayload {
    char     device_name[64];
    uint16_t tcp_port;
    uint16_t capabilities;
    char     public_key_hash[32];
    uint32_t public_key_length;
    uint32_t signature_length;
    uint32_t reserved;
};

struct __attribute__((packed)) HeartbeatPayload {
    char     device_name[64];
    uint32_t uptime_seconds;
    uint32_t reserved[3];
};

// ============================================================================
// Phase 4 payload structs
// ============================================================================
// Convention:
//   - Fixed-size fields use packed integers.
//   - Variable-length fields (public keys, JSON) are appended after the struct
//     in the payload buffer. The struct carries the length of each variable
//     section so the parser knows where each section starts.
//   - All multi-byte integers are network byte order (big-endian).
//   - group_id is always exactly 8 chars, null-padded.
//   - fingerprints are 32 hex chars (16 bytes of SHA256), null-terminated.
//   - filenames/paths are null-terminated, max lengths as shown.
// ============================================================================

// ── SYNC_NOTIFY ─────────────────────────────────────────────────────────────
// Broadcast when a file is created or modified.
// Receivers compare version against local metadata to decide whether to request.
struct __attribute__((packed)) SyncNotifyPayload {
    char     group_id[8];         // Group this file belongs to
    char     filename[256];       // Relative path within group folder
    uint32_t version;             // New version number
    uint64_t file_size;           // Bytes (of the actual file, not encrypted)
    char     checksum[72];        // "sha256:<64 hex chars>\0"
    char     owner_fp[33];        // Owner fingerprint (32 hex + null)
    uint8_t  reserved[3];         // Alignment padding
    // Total: 8+256+4+8+72+33+3 = 384 bytes
};

// ── SYNC_REQUEST ─────────────────────────────────────────────────────────────
// Broadcast when a device needs a file (or a newer version of one).
struct __attribute__((packed)) SyncRequestPayload {
    char     group_id[8];
    char     filename[256];
    uint32_t have_version;        // Local version (0 if not present at all)
    uint32_t need_version;        // Version being requested
    char     requester_fp[33];    // So offerers can verify group membership
    uint8_t  reserved[3];
    // Total: 8+256+4+4+33+3 = 308 bytes
};

// ── SYNC_OFFER ───────────────────────────────────────────────────────────────
// Sent directly (unicast) to the requester after receiving SYNC_REQUEST.
struct __attribute__((packed)) SyncOfferPayload {
    char     group_id[8];
    char     filename[256];
    uint32_t version;
    uint16_t sender_tcp_port;     // TCP port to connect to for the transfer
    char     offerer_fp[33];
    uint8_t  reserved[1];
    // Total: 8+256+4+2+33+1 = 304 bytes
};

// ── SYNC_OFFER_ACCEPT ────────────────────────────────────────────────────────
// Sent directly (unicast) back to the offerer.
struct __attribute__((packed)) SyncOfferAcceptPayload {
    char     group_id[8];
    char     filename[256];
    uint32_t version;
    char     accepter_fp[33];
    uint8_t  reserved[3];
    // Total: 8+256+4+33+3 = 304 bytes
};

// ── SYNC_DECLINE ─────────────────────────────────────────────────────────────
// Sent to an offerer whose offer arrived after the file was already accepted
// from another peer. Lets the offerer free its activeTransfers slot.
struct __attribute__((packed)) SyncDeclinePayload {
    char     group_id[8];
    char     filename[256];
    uint32_t version;
    uint8_t  reserved[4];
    // Total: 8+256+4+4 = 272 bytes
};

// ── SYNC_DELETE_NOTIFY ───────────────────────────────────────────────────────
// Broadcast when a file is deleted (by owner or admin).
struct __attribute__((packed)) SyncDeletePayload {
    char     group_id[8];
    char     filename[256];
    char     deleted_by_fp[33];   // Fingerprint of deleter (owner or admin)
    uint8_t  reserved[3];
    uint64_t deleted_at;          // Unix timestamp
    uint32_t version_at_deletion;
    uint8_t  padding[4];
    // Total: 8+256+33+3+8+4+4 = 316 bytes
};

// ── SYNC_RENAME_NOTIFY ───────────────────────────────────────────────────────
// Broadcast when a file is renamed (by owner only).
struct __attribute__((packed)) SyncRenamePayload {
    char     group_id[8];
    char     old_name[256];
    char     new_name[256];
    char     renamed_by_fp[33];
    uint8_t  reserved[3];
    uint32_t new_version;
    uint64_t renamed_at;
    // Total: 8+256+256+33+3+4+8 = 568 bytes
};

// ── SYNC_META_REQUEST ────────────────────────────────────────────────────────
// Broadcast when a device comes online or resumes sync.
// Any peer with the group responds with SYNC_META_DELTA (full metadata JSON).
struct __attribute__((packed)) SyncMetaRequestPayload {
    char     group_id[8];
    char     requester_fp[33];
    uint8_t  reserved[3];
    // Total: 8+33+3 = 44 bytes
};

// ── SYNC_META_DELTA ──────────────────────────────────────────────────────────
// Carries a JSON blob: either a single file entry (incremental update)
// or the full metadata JSON (response to SYNC_META_REQUEST).
// Variable section: json_length bytes of UTF-8 JSON appended after struct.
struct __attribute__((packed)) SyncMetaDeltaPayload {
    char     group_id[8];
    char     sender_fp[33];
    uint8_t  is_full_metadata;    // 1 = full metadata dump, 0 = single entry delta
    uint8_t  reserved[2];
    uint32_t json_length;         // Byte length of the JSON appended after this struct
    // Variable: json_length bytes of JSON follow
    // Total fixed: 8+33+1+2+4 = 48 bytes
};

// ── SYNC_PAUSE_NOTIFY ────────────────────────────────────────────────────────
// Sent to all online group members when this device pauses sync.
struct __attribute__((packed)) SyncPauseNotifyPayload {
    char     group_id[8];         // Empty (all zeros) means "all groups"
    char     sender_fp[33];
    uint8_t  is_hard_pause;       // 1 = hard (abort transfers), 0 = soft (finish first)
    uint8_t  reserved[2];
    // Total: 8+33+1+2 = 44 bytes
};

// ── SYNC_PAUSE_ACK ───────────────────────────────────────────────────────────
// Sent back to the pausing device to confirm cleanup is done.
struct __attribute__((packed)) SyncPauseAckPayload {
    char     group_id[8];
    char     acker_fp[33];
    uint8_t  reserved[3];
    // Total: 8+33+3 = 44 bytes
};

// ── SYNC_RESUME_NOTIFY ───────────────────────────────────────────────────────
// Broadcast when this device resumes sync (informational — peers can proactively
// send any SYNC_NOTIFYs for files this device might have missed).
struct __attribute__((packed)) SyncResumeNotifyPayload {
    char     group_id[8];         // Empty = all groups
    char     sender_fp[33];
    uint8_t  reserved[3];
    // Total: 8+33+3 = 44 bytes
};

// ── GROUP_INVITE ─────────────────────────────────────────────────────────────
// Sent by admin directly (unicast) to a peer's IP.
// Variable section: group_json_length bytes of .lanbox_group.json content.
struct __attribute__((packed)) GroupInvitePayload {
    char     group_id[8];
    char     group_name[64];
    char     inviter_fp[33];
    char     invitee_fp[33];
    uint8_t  reserved[2];
    uint32_t group_json_length;   // Byte length of group JSON appended after struct
    // Variable: group_json_length bytes of JSON follow
    // Total fixed: 8+64+33+33+2+4 = 144 bytes
};

// ── GROUP_INVITE_ACK ─────────────────────────────────────────────────────────
// Sent by invitee back to inviter.
struct __attribute__((packed)) GroupInviteAckPayload {
    char     group_id[8];
    char     invitee_fp[33];
    uint8_t  accepted;            // 1 = accepted, 0 = rejected
    uint8_t  reserved[2];
    // Total: 8+33+1+2 = 44 bytes
};

// ── GROUP_JOIN_REQUEST ───────────────────────────────────────────────────────
// Broadcast by a peer that knows the group_id and wants to join.
// Variable section: pubkey_length bytes of PEM public key appended after struct.
struct __attribute__((packed)) GroupJoinRequestPayload {
    char     group_id[8];
    char     requester_name[64];
    char     requester_fp[33];
    uint8_t  reserved[3];
    uint32_t pubkey_length;       // Byte length of PEM public key appended after struct
    // Variable: pubkey_length bytes of PEM public key follow
    // Total fixed: 8+64+33+3+4 = 112 bytes
};

// ── GROUP_JOIN_APPROVE ───────────────────────────────────────────────────────
// Sent by admin directly (unicast) to the requesting peer.
// Variable section: group_json_length bytes of full .lanbox_group.json.
struct __attribute__((packed)) GroupJoinApprovePayload {
    char     group_id[8];
    char     approved_fp[33];
    uint8_t  reserved[3];
    uint32_t group_json_length;
    // Variable: group_json_length bytes of JSON follow
    // Total fixed: 8+33+3+4 = 48 bytes
};

// ── GROUP_JOIN_DENY ──────────────────────────────────────────────────────────
// Sent by admin to the requesting peer.
struct __attribute__((packed)) GroupJoinDenyPayload {
    char     group_id[8];
    char     reason[128];         // Human-readable reason
    // Total: 8+128 = 136 bytes
};

// ── GROUP_KICK ───────────────────────────────────────────────────────────────
// Broadcast to all group members when a member is removed.
struct __attribute__((packed)) GroupKickPayload {
    char     group_id[8];
    char     kicked_fp[33];
    char     kicked_by_fp[33];
    uint8_t  reserved[2];
    // Total: 8+33+33+2 = 76 bytes
};

// ============================================================================
// Sync transfer header
// Prepended to every TCP file transfer in Phase 4.
// Backward compatible: transfer_type=0 means legacy manual send,
// and the receiver falls through to the existing wire format.
// ============================================================================

struct __attribute__((packed)) SyncTransferHeader {
    uint8_t  transfer_type;       // 0 = legacy, 1 = group sync
    char     group_id[8];         // Meaningful only when transfer_type == 1
    char     relative_path[256];  // Path relative to group folder root
    // Total: 1+8+256 = 265 bytes
};

// ============================================================================
// Protocol utilities class
// ============================================================================

class Protocol {
public:
    // ── Existing utilities (unchanged) ──────────────────────────────────────
    static uint32_t calculateCRC32(const uint8_t* data, size_t length);

    static vector<uint8_t> createDiscoveryRequest(
        const string& device_name,
        uint32_t sender_ip,
        uint16_t tcp_port,
        uint32_t sequence,
        const string& public_key = ""
    );

    static vector<uint8_t> createHeartbeat(
        const string& device_name,
        uint32_t sender_ip,
        uint32_t uptime_seconds,
        uint32_t sequence
    );

    static bool parseMessage(
        const uint8_t* data,
        size_t length,
        LANBoxHeader& header,
        vector<uint8_t>& payload
    );

    static bool validateMessage(const uint8_t* data, size_t length);

    static uint32_t ipToUint32(const string& ip);
    static string   uint32ToIp(uint32_t ip);
    static string   messageTypeToString(MessageType type);

    // ── Phase 4 message builders ─────────────────────────────────────────────
    // All builders follow the same pattern as createDiscoveryRequest:
    //   fill header + payload struct → copy into buffer → compute CRC → return.

    static vector<uint8_t> createSyncNotify(
        uint32_t sender_ip,
        uint32_t sequence,
        const SyncNotifyPayload& p
    );

    static vector<uint8_t> createSyncRequest(
        uint32_t sender_ip,
        uint32_t sequence,
        const SyncRequestPayload& p
    );

    static vector<uint8_t> createSyncOffer(
        uint32_t sender_ip,
        uint32_t sequence,
        const SyncOfferPayload& p
    );

    static vector<uint8_t> createSyncOfferAccept(
        uint32_t sender_ip,
        uint32_t sequence,
        const SyncOfferAcceptPayload& p
    );

    static vector<uint8_t> createSyncDecline(
        uint32_t sender_ip,
        uint32_t sequence,
        const SyncDeclinePayload& p
    );

    static vector<uint8_t> createSyncDeleteNotify(
        uint32_t sender_ip,
        uint32_t sequence,
        const SyncDeletePayload& p
    );

    static vector<uint8_t> createSyncRenameNotify(
        uint32_t sender_ip,
        uint32_t sequence,
        const SyncRenamePayload& p
    );

    static vector<uint8_t> createSyncMetaRequest(
        uint32_t sender_ip,
        uint32_t sequence,
        const SyncMetaRequestPayload& p
    );

    // json_data: the JSON string to embed (full metadata or single entry delta)
    static vector<uint8_t> createSyncMetaDelta(
        uint32_t sender_ip,
        uint32_t sequence,
        const SyncMetaDeltaPayload& fixed,
        const string& json_data
    );

    static vector<uint8_t> createSyncPauseNotify(
        uint32_t sender_ip,
        uint32_t sequence,
        const SyncPauseNotifyPayload& p
    );

    static vector<uint8_t> createSyncPauseAck(
        uint32_t sender_ip,
        uint32_t sequence,
        const SyncPauseAckPayload& p
    );

    static vector<uint8_t> createSyncResumeNotify(
        uint32_t sender_ip,
        uint32_t sequence,
        const SyncResumeNotifyPayload& p
    );

    // group_json: full .lanbox_group.json content
    static vector<uint8_t> createGroupInvite(
        uint32_t sender_ip,
        uint32_t sequence,
        const GroupInvitePayload& fixed,
        const string& group_json
    );

    static vector<uint8_t> createGroupInviteAck(
        uint32_t sender_ip,
        uint32_t sequence,
        const GroupInviteAckPayload& p
    );

    // pubkey_pem: PEM public key of the requester
    static vector<uint8_t> createGroupJoinRequest(
        uint32_t sender_ip,
        uint32_t sequence,
        const GroupJoinRequestPayload& fixed,
        const string& pubkey_pem
    );

    // group_json: full .lanbox_group.json content
    static vector<uint8_t> createGroupJoinApprove(
        uint32_t sender_ip,
        uint32_t sequence,
        const GroupJoinApprovePayload& fixed,
        const string& group_json
    );

    static vector<uint8_t> createGroupJoinDeny(
        uint32_t sender_ip,
        uint32_t sequence,
        const GroupJoinDenyPayload& p
    );

    static vector<uint8_t> createGroupKick(
        uint32_t sender_ip,
        uint32_t sequence,
        const GroupKickPayload& p
    );

private:
    // Internal helper used by all builders:
    // wraps a payload buffer in a LANBoxHeader and computes CRC.
    static vector<uint8_t> buildMessage(
        MessageType type,
        uint32_t sender_ip,
        uint32_t sequence,
        const uint8_t* payload,
        size_t payload_size
    );
};