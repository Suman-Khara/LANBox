#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace std;

// Protocol constants
const uint32_t LANBOX_MAGIC = 0x4C424F58;  // "LBOX" in hex
const uint16_t PROTOCOL_VERSION = 0x0001;   // Version 1
const size_t HEADER_SIZE = 32;              // Fixed header size
const size_t MAX_PAYLOAD_SIZE = 8192;       // 8KB max payload

// Message types
enum class MessageType : uint16_t {
    DISCOVERY_REQUEST  = 0x0001,  // Broadcast "I'm here"
    DISCOVERY_RESPONSE = 0x0002,  // Reply to discovery
    HEARTBEAT          = 0x0003,  // Periodic keepalive
    
    // Future types (for later phases)
    FILE_TRANSFER_REQ  = 0x0100,
    FILE_CHUNK         = 0x0101,
    FILE_ACK           = 0x0102,
    
    PEER_INFO_REQUEST  = 0x0200,
    PEER_INFO_RESPONSE = 0x0201,
};

// Protocol header structure (exactly 32 bytes)
// __attribute__((packed)) ensures no padding bytes
struct __attribute__((packed)) LANBoxHeader {
    uint32_t magic;              // Must be LANBOX_MAGIC (4 bytes)
    uint16_t version;            // Protocol version (2 bytes)
    uint16_t message_type;       // MessageType enum (2 bytes)
    uint32_t sequence_number;    // For ordering/deduplication (4 bytes)
    uint32_t payload_length;     // Size of payload after header (4 bytes)
    uint64_t timestamp;          // Unix timestamp in seconds (8 bytes)
    uint32_t sender_ip;          // Sender's IP as uint32 (4 bytes)
    uint32_t checksum;           // CRC32 of entire message (4 bytes)
    
    // Total: 4+2+2+4+4+8+4+4 = 32 bytes
};

// Verify size at compile time
static_assert(sizeof(LANBoxHeader) == HEADER_SIZE, "Header size must be 32 bytes");

// Discovery request payload
struct __attribute__((packed)) DiscoveryPayload {
    char device_name[64];        // Hostname (64 bytes)
    uint16_t tcp_port;           // Port for file transfers (2 bytes)
    uint16_t capabilities;       // Feature flags (2 bytes)
    uint32_t reserved;           // For future use (4 bytes)
    // Total: 72 bytes
};

// Heartbeat payload
struct __attribute__((packed)) HeartbeatPayload {
    char device_name[64];        // Hostname (64 bytes)
    uint32_t uptime_seconds;     // How long device is online (4 bytes)
    uint32_t reserved[3];        // For future use (12 bytes)
    // Total: 80 bytes
};

// Protocol utilities class
class Protocol {
public:
    // CRC32 calculation for integrity checking
    static uint32_t calculateCRC32(const uint8_t* data, size_t length);
    
    // Create a discovery request message
    static vector<uint8_t> createDiscoveryRequest(
        const string& device_name,
        uint32_t sender_ip,
        uint16_t tcp_port,
        uint32_t sequence
    );
    
    // Create a heartbeat message
    static vector<uint8_t> createHeartbeat(
        const string& device_name,
        uint32_t sender_ip,
        uint32_t uptime_seconds,
        uint32_t sequence
    );
    
    // Parse incoming message
    static bool parseMessage(
        const uint8_t* data,
        size_t length,
        LANBoxHeader& header,
        vector<uint8_t>& payload
    );
    
    // Validate message integrity
    static bool validateMessage(const uint8_t* data, size_t length);
    
    // Helper: Convert IP string to uint32
    static uint32_t ipToUint32(const string& ip);
    
    // Helper: Convert uint32 to IP string
    static string uint32ToIp(uint32_t ip);
    
    // Helper: Get message type as string
    static string messageTypeToString(MessageType type);
};