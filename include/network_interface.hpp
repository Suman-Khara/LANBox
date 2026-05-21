#pragma once

// Forward declare std::string and std::vector to avoid including <string> early
#include <iosfwd>
#include <vector>
#include <cstdint>
#include <string>
class NetworkInterface {
private:
    std::string name;
    std::string ip_address;
    std::string subnet_mask;
    std::string broadcast_address;
    bool is_active;

public:
    NetworkInterface(const std::string& n, const std::string& ip, const std::string& mask, 
                    const std::string& broadcast, bool active);

    // Getters
    std::string getName() const;
    std::string getIP() const;
    std::string getSubnetMask() const;
    std::string getBroadcastAddress() const;
    bool isActive() const;

    // Print interface info
    void print() const;
};

class NetworkInterfaceManager {
public:
    static std::vector<NetworkInterface> enumerateInterfaces();
    static std::vector<NetworkInterface> getActiveInterfaces();
    static std::string calculateBroadcastAddress(const std::string& ip, const std::string& subnet_mask);
    static uint32_t ipToUint32(const std::string& ip);
    static std::string uint32ToIp(uint32_t ip);
    static void printInterfaces(const std::vector<NetworkInterface>& interfaces);
};