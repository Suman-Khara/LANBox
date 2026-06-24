#include "platform.hpp"
#include <bits/stdc++.h>

// Cross-platform 64-bit byte swap (same block used in protocol.cpp)
#ifdef _WIN32
    #include <winsock2.h>
    #ifndef htonll
        #define htonll(x) ((1==htonl(1)) ? (x) : ((uint64_t)htonl((x) & 0xFFFFFFFF) << 32) | htonl((x) >> 32))
        #define ntohll(x) ((1==ntohl(1)) ? (x) : ((uint64_t)ntohl((x) & 0xFFFFFFFF) << 32) | ntohl((x) >> 32))
    #endif
#else
    #include <arpa/inet.h>
    #ifndef htonll
        #define htonll(x) htobe64(x)
        #define ntohll(x) be64toh(x)
    #endif
#endif

#include "protocol.hpp"

using namespace std;

// ============================================================================
// Minimal test harness
// ============================================================================

static int tests_run    = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define CHECK(label, condition)                                         \
    do {                                                                \
        tests_run++;                                                    \
        if (condition) {                                                \
            cout << "  [PASS] " << label << "\n";                      \
            tests_passed++;                                             \
        } else {                                                        \
            cout << "  [FAIL] " << label << "\n";                      \
            tests_failed++;                                             \
        }                                                               \
    } while(0)

#define SECTION(name) \
    cout << "\n--- " << name << " ---\n";

// ============================================================================
// Helpers
// ============================================================================

// Parse the header out of a built message and return it host-byte-order
static LANBoxHeader extractHeader(const vector<uint8_t>& msg) {
    LANBoxHeader h;
    memcpy(&h, msg.data(), sizeof(h));
    // Convert to host byte order for easy comparison
    h.magic           = ntohl(h.magic);
    h.version         = ntohs(h.version);
    h.message_type    = ntohs(h.message_type);
    h.sequence_number = ntohl(h.sequence_number);
    h.payload_length  = ntohl(h.payload_length);
    h.sender_ip       = ntohl(h.sender_ip);
    // checksum left as-is (we test it via validateMessage)
    return h;
}

// ============================================================================
// Test groups
// ============================================================================

void test_constants() {
    SECTION("Constants");
    CHECK("LANBOX_MAGIC is 0x4C424F58",     LANBOX_MAGIC    == 0x4C424F58);
    CHECK("PROTOCOL_VERSION is 0x0001",     PROTOCOL_VERSION == 0x0001);
    CHECK("HEADER_SIZE is 32",              HEADER_SIZE      == 32);
    CHECK("MAX_PAYLOAD_SIZE is 65536",      MAX_PAYLOAD_SIZE == 65536);
    CHECK("LANBoxHeader is exactly 32 bytes", sizeof(LANBoxHeader) == 32);
}

void test_messageTypeToString() {
    SECTION("MessageType to string");
    // Existing
    CHECK("DISCOVERY_REQUEST",  Protocol::messageTypeToString(MessageType::DISCOVERY_REQUEST)  == "DISCOVERY_REQUEST");
    CHECK("HEARTBEAT",          Protocol::messageTypeToString(MessageType::HEARTBEAT)          == "HEARTBEAT");
    // Phase 4 sync
    CHECK("SYNC_NOTIFY",        Protocol::messageTypeToString(MessageType::SYNC_NOTIFY)        == "SYNC_NOTIFY");
    CHECK("SYNC_REQUEST",       Protocol::messageTypeToString(MessageType::SYNC_REQUEST)       == "SYNC_REQUEST");
    CHECK("SYNC_OFFER",         Protocol::messageTypeToString(MessageType::SYNC_OFFER)         == "SYNC_OFFER");
    CHECK("SYNC_OFFER_ACCEPT",  Protocol::messageTypeToString(MessageType::SYNC_OFFER_ACCEPT)  == "SYNC_OFFER_ACCEPT");
    CHECK("SYNC_DECLINE",       Protocol::messageTypeToString(MessageType::SYNC_DECLINE)       == "SYNC_DECLINE");
    CHECK("SYNC_DELETE_NOTIFY", Protocol::messageTypeToString(MessageType::SYNC_DELETE_NOTIFY) == "SYNC_DELETE_NOTIFY");
    CHECK("SYNC_RENAME_NOTIFY", Protocol::messageTypeToString(MessageType::SYNC_RENAME_NOTIFY) == "SYNC_RENAME_NOTIFY");
    CHECK("SYNC_META_REQUEST",  Protocol::messageTypeToString(MessageType::SYNC_META_REQUEST)  == "SYNC_META_REQUEST");
    CHECK("SYNC_META_DELTA",    Protocol::messageTypeToString(MessageType::SYNC_META_DELTA)    == "SYNC_META_DELTA");
    CHECK("SYNC_PAUSE_NOTIFY",  Protocol::messageTypeToString(MessageType::SYNC_PAUSE_NOTIFY)  == "SYNC_PAUSE_NOTIFY");
    CHECK("SYNC_PAUSE_ACK",     Protocol::messageTypeToString(MessageType::SYNC_PAUSE_ACK)     == "SYNC_PAUSE_ACK");
    CHECK("SYNC_RESUME_NOTIFY", Protocol::messageTypeToString(MessageType::SYNC_RESUME_NOTIFY) == "SYNC_RESUME_NOTIFY");
    // Phase 4 group
    CHECK("GROUP_INVITE",       Protocol::messageTypeToString(MessageType::GROUP_INVITE)       == "GROUP_INVITE");
    CHECK("GROUP_INVITE_ACK",   Protocol::messageTypeToString(MessageType::GROUP_INVITE_ACK)   == "GROUP_INVITE_ACK");
    CHECK("GROUP_JOIN_REQUEST", Protocol::messageTypeToString(MessageType::GROUP_JOIN_REQUEST)  == "GROUP_JOIN_REQUEST");
    CHECK("GROUP_JOIN_APPROVE", Protocol::messageTypeToString(MessageType::GROUP_JOIN_APPROVE) == "GROUP_JOIN_APPROVE");
    CHECK("GROUP_JOIN_DENY",    Protocol::messageTypeToString(MessageType::GROUP_JOIN_DENY)    == "GROUP_JOIN_DENY");
    CHECK("GROUP_KICK",         Protocol::messageTypeToString(MessageType::GROUP_KICK)         == "GROUP_KICK");
    CHECK("UNKNOWN type",       Protocol::messageTypeToString(static_cast<MessageType>(0xFFFF)) == "UNKNOWN");
}

void test_crc32() {
    SECTION("CRC32");
    // Known CRC32 values
    uint8_t empty[] = {};
    uint8_t hello[] = {'h','e','l','l','o'};
    uint8_t zeros[16] = {};

    uint32_t crc_hello = Protocol::calculateCRC32(hello, 5);
    uint32_t crc_empty = Protocol::calculateCRC32(empty, 0);

    // CRC32 of "hello" is a known constant
    CHECK("CRC32 of 'hello' is 0x3610A686",  crc_hello == 0x3610A686);
    // CRC32 of empty is 0x00000000
    CHECK("CRC32 of empty is 0x00000000",    crc_empty == 0x00000000);
    // CRC32 of zeros is deterministic
    uint32_t crc_zeros1 = Protocol::calculateCRC32(zeros, 16);
    uint32_t crc_zeros2 = Protocol::calculateCRC32(zeros, 16);
    CHECK("CRC32 is deterministic",          crc_zeros1 == crc_zeros2);
    // Different data produces different CRC
    uint8_t hello2[] = {'H','e','l','l','o'};
    CHECK("Different data → different CRC",  crc_hello != Protocol::calculateCRC32(hello2, 5));
}

void test_ip_helpers() {
    SECTION("IP helpers");
    CHECK("192.168.1.10 → uint32 → string round-trip",
          Protocol::uint32ToIp(Protocol::ipToUint32("192.168.1.10")) == "192.168.1.10");
    CHECK("10.0.0.1 round-trip",
          Protocol::uint32ToIp(Protocol::ipToUint32("10.0.0.1")) == "10.0.0.1");
    CHECK("255.255.255.255 round-trip",
          Protocol::uint32ToIp(Protocol::ipToUint32("255.255.255.255")) == "255.255.255.255");
    CHECK("0.0.0.0 round-trip",
          Protocol::uint32ToIp(Protocol::ipToUint32("0.0.0.0")) == "0.0.0.0");
    // Known value
    // 192.168.1.1 = 0xC0A80101
    CHECK("192.168.1.1 == 0xC0A80101",
          Protocol::ipToUint32("192.168.1.1") == 0xC0A80101);
}

void test_heartbeat() {
    SECTION("Heartbeat build + parse + validate");

    uint32_t sender_ip = Protocol::ipToUint32("192.168.1.10");
    vector<uint8_t> msg = Protocol::createHeartbeat("TestDevice", sender_ip, 42, 1);

    CHECK("Message not empty",            !msg.empty());
    CHECK("Min size (header + payload)",  msg.size() >= sizeof(LANBoxHeader));

    // Validate CRC
    CHECK("CRC validates",
          Protocol::validateMessage(msg.data(), msg.size()));

    // Parse
    LANBoxHeader header;
    vector<uint8_t> payload;
    bool parsed = Protocol::parseMessage(msg.data(), msg.size(), header, payload);
    CHECK("Parses successfully",          parsed);
    CHECK("Magic is correct",             ntohl(header.magic) == LANBOX_MAGIC);
    CHECK("Type is HEARTBEAT",
          ntohs(header.message_type) == static_cast<uint16_t>(MessageType::HEARTBEAT));
    CHECK("Sequence is 1",                ntohl(header.sequence_number) == 1);
    CHECK("Sender IP matches",            ntohl(header.sender_ip) == sender_ip);
    CHECK("Payload size == HeartbeatPayload",
          ntohl(header.payload_length) == sizeof(HeartbeatPayload));

    // Verify device name in payload
    HeartbeatPayload hb;
    memcpy(&hb, payload.data(), sizeof(hb));
    CHECK("Device name is 'TestDevice'",  string(hb.device_name) == "TestDevice");
    CHECK("Uptime is 42",                 ntohl(hb.uptime_seconds) == 42);

    // Tamper with message — CRC should fail
    vector<uint8_t> tampered = msg;
    tampered[sizeof(LANBoxHeader) + 2] ^= 0xFF;
    CHECK("Tampered message fails CRC",
          !Protocol::validateMessage(tampered.data(), tampered.size()));
}

void test_sync_notify() {
    SECTION("SYNC_NOTIFY build + parse");

    SyncNotifyPayload p;
    memset(&p, 0, sizeof(p));
    strncpy(p.group_id,  "a3f5c2d8", 8);
    strncpy(p.filename,  "report.pdf", sizeof(p.filename) - 1);
    p.version   = 3;
    p.file_size = 1048576;
    strncpy(p.checksum,  "sha256:abcdef1234567890", sizeof(p.checksum) - 1);
    strncpy(p.owner_fp,  "c132070a1b2c3d4e", sizeof(p.owner_fp) - 1);

    uint32_t ip  = Protocol::ipToUint32("192.168.1.10");
    auto msg = Protocol::createSyncNotify(ip, 7, p);

    CHECK("Not empty",       !msg.empty());
    CHECK("CRC validates",   Protocol::validateMessage(msg.data(), msg.size()));

    LANBoxHeader header;
    vector<uint8_t> payload;
    CHECK("Parses",          Protocol::parseMessage(msg.data(), msg.size(), header, payload));
    CHECK("Type is SYNC_NOTIFY",
          ntohs(header.message_type) == static_cast<uint16_t>(MessageType::SYNC_NOTIFY));
    CHECK("Sequence is 7",   ntohl(header.sequence_number) == 7);
    CHECK("Payload size == SyncNotifyPayload",
          ntohl(header.payload_length) == sizeof(SyncNotifyPayload));

    // Verify fields survived round-trip (after host-byte-order conversion)
    SyncNotifyPayload recv;
    memcpy(&recv, payload.data(), sizeof(recv));
    CHECK("group_id matches",  string(recv.group_id, 8) == "a3f5c2d8");
    CHECK("filename matches",  string(recv.filename) == "report.pdf");
    CHECK("version matches",   ntohl(recv.version) == 3);
    CHECK("file_size matches", ntohll(recv.file_size) == 1048576ULL);
    CHECK("owner_fp matches",  string(recv.owner_fp) == "c132070a1b2c3d4e");
}

void test_sync_request() {
    SECTION("SYNC_REQUEST build + parse");

    SyncRequestPayload p;
    memset(&p, 0, sizeof(p));
    strncpy(p.group_id,       "a3f5c2d8", 8);
    strncpy(p.filename,       "notes.txt", sizeof(p.filename) - 1);
    p.have_version = 0;
    p.need_version = 2;
    strncpy(p.requester_fp,   "8a3f1b2c", sizeof(p.requester_fp) - 1);

    uint32_t ip = Protocol::ipToUint32("192.168.1.11");
    auto msg = Protocol::createSyncRequest(ip, 99, p);

    CHECK("CRC validates",    Protocol::validateMessage(msg.data(), msg.size()));

    LANBoxHeader header;
    vector<uint8_t> payload;
    Protocol::parseMessage(msg.data(), msg.size(), header, payload);
    CHECK("Type is SYNC_REQUEST",
          ntohs(header.message_type) == static_cast<uint16_t>(MessageType::SYNC_REQUEST));

    SyncRequestPayload recv;
    memcpy(&recv, payload.data(), sizeof(recv));
    CHECK("have_version == 0", ntohl(recv.have_version) == 0);
    CHECK("need_version == 2", ntohl(recv.need_version) == 2);
    CHECK("filename matches",  string(recv.filename) == "notes.txt");
}

void test_sync_meta_delta_variable_payload() {
    SECTION("SYNC_META_DELTA (variable JSON payload) build + parse");

    string json = R"({"filename":"report.pdf","version":3,"checksum":"sha256:abc"})";

    SyncMetaDeltaPayload fixed;
    memset(&fixed, 0, sizeof(fixed));
    strncpy(fixed.group_id,   "a3f5c2d8", 8);
    strncpy(fixed.sender_fp,  "c132070a", sizeof(fixed.sender_fp) - 1);
    fixed.is_full_metadata = 0;
    // json_length will be set by the builder

    uint32_t ip = Protocol::ipToUint32("192.168.1.10");
    auto msg = Protocol::createSyncMetaDelta(ip, 5, fixed, json);

    CHECK("CRC validates",    Protocol::validateMessage(msg.data(), msg.size()));

    LANBoxHeader header;
    vector<uint8_t> payload;
    Protocol::parseMessage(msg.data(), msg.size(), header, payload);
    CHECK("Type is SYNC_META_DELTA",
          ntohs(header.message_type) == static_cast<uint16_t>(MessageType::SYNC_META_DELTA));

    // Payload = fixed struct + JSON
    size_t expected_payload = sizeof(SyncMetaDeltaPayload) + json.size();
    CHECK("Payload length = struct + JSON",
          ntohl(header.payload_length) == expected_payload);

    // Extract fixed struct
    SyncMetaDeltaPayload recv_fixed;
    memcpy(&recv_fixed, payload.data(), sizeof(recv_fixed));
    CHECK("json_length matches",
          ntohl(recv_fixed.json_length) == json.size());
    CHECK("is_full_metadata == 0", recv_fixed.is_full_metadata == 0);

    // Extract JSON
    string recv_json(
        reinterpret_cast<const char*>(payload.data() + sizeof(SyncMetaDeltaPayload)),
        ntohl(recv_fixed.json_length)
    );
    CHECK("JSON content matches", recv_json == json);
}

void test_group_join_request_variable_payload() {
    SECTION("GROUP_JOIN_REQUEST (variable pubkey payload) build + parse");

    string fake_pubkey = "-----BEGIN PUBLIC KEY-----\nMIIBIjANBgkqhkiG\n-----END PUBLIC KEY-----\n";

    GroupJoinRequestPayload fixed;
    memset(&fixed, 0, sizeof(fixed));
    strncpy(fixed.group_id,        "a3f5c2d8", 8);
    strncpy(fixed.requester_name,  "Bob", sizeof(fixed.requester_name) - 1);
    strncpy(fixed.requester_fp,    "8a3f1b2c", sizeof(fixed.requester_fp) - 1);

    uint32_t ip = Protocol::ipToUint32("192.168.1.11");
    auto msg = Protocol::createGroupJoinRequest(ip, 12, fixed, fake_pubkey);

    CHECK("CRC validates",    Protocol::validateMessage(msg.data(), msg.size()));

    LANBoxHeader header;
    vector<uint8_t> payload;
    Protocol::parseMessage(msg.data(), msg.size(), header, payload);
    CHECK("Type is GROUP_JOIN_REQUEST",
          ntohs(header.message_type) == static_cast<uint16_t>(MessageType::GROUP_JOIN_REQUEST));

    GroupJoinRequestPayload recv;
    memcpy(&recv, payload.data(), sizeof(recv));
    uint32_t pk_len = ntohl(recv.pubkey_length);
    CHECK("pubkey_length matches",    pk_len == fake_pubkey.size());
    CHECK("requester_name matches",   string(recv.requester_name) == "Bob");

    string recv_pubkey(
        reinterpret_cast<const char*>(payload.data() + sizeof(GroupJoinRequestPayload)),
        pk_len
    );
    CHECK("Pubkey content matches",   recv_pubkey == fake_pubkey);
}

void test_sync_transfer_header_size() {
    SECTION("SyncTransferHeader struct");
    // 1 (transfer_type) + 8 (group_id) + 256 (relative_path) = 265
    CHECK("SyncTransferHeader is 265 bytes", sizeof(SyncTransferHeader) == 265);
}

void test_parse_rejects_bad_messages() {
    SECTION("parseMessage / validateMessage reject bad input");

    // Too short
    vector<uint8_t> too_short(10, 0);
    LANBoxHeader h; vector<uint8_t> pl;
    CHECK("Too short fails parse",
          !Protocol::parseMessage(too_short.data(), too_short.size(), h, pl));

    // Wrong magic
    auto good = Protocol::createHeartbeat("X", 0, 0, 0);
    vector<uint8_t> bad_magic = good;
    bad_magic[0] = 0xDE; bad_magic[1] = 0xAD;
    CHECK("Wrong magic fails parse",
          !Protocol::parseMessage(bad_magic.data(), bad_magic.size(), h, pl));

    // Corrupt payload (CRC mismatch)
    vector<uint8_t> corrupt = good;
    corrupt[sizeof(LANBoxHeader) + 1] ^= 0xFF;
    CHECK("Corrupt payload fails validate",
          !Protocol::validateMessage(corrupt.data(), corrupt.size()));
}

// ============================================================================
// Main
// ============================================================================

int main() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

    cout << "========================================\n";
    cout << "  LANBox Step 1 — Protocol Layer Tests  \n";
    cout << "========================================\n";

    test_constants();
    test_messageTypeToString();
    test_crc32();
    test_ip_helpers();
    test_heartbeat();
    test_sync_notify();
    test_sync_request();
    test_sync_meta_delta_variable_payload();
    test_group_join_request_variable_payload();
    test_sync_transfer_header_size();
    test_parse_rejects_bad_messages();

    cout << "\n========================================\n";
    cout << "  Results: "
         << tests_passed << " passed, "
         << tests_failed << " failed, "
         << tests_run    << " total\n";
    cout << "========================================\n";

    return (tests_failed == 0) ? 0 : 1;
}