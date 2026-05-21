#include "platform.hpp"  // This MUST be first
#include "network_interface.hpp"

// NOW we can include standard library headers
#include <string>
#include <iostream>
#include <cstring>
#include <sstream>
#include <cstdint>
#ifdef _WIN32
    #include <iphlpapi.h>
    #pragma comment(lib, "iphlpapi.lib")
#else
    #include <sys/types.h>
    #include <sys/socket.h>
    #include <ifaddrs.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <net/if.h>
    #include <unistd.h>
#endif

using namespace std;

// ============================================================================
// NetworkInterface Implementation
// ============================================================================

NetworkInterface::NetworkInterface(const string& n, const string& ip, const string& mask, 
                                 const string& broadcast, bool active)
    : name(n), ip_address(ip), subnet_mask(mask), 
      broadcast_address(broadcast), is_active(active) {}

string NetworkInterface::getName() const { return name; }
string NetworkInterface::getIP() const { return ip_address; }
string NetworkInterface::getSubnetMask() const { return subnet_mask; }
string NetworkInterface::getBroadcastAddress() const { return broadcast_address; }
bool NetworkInterface::isActive() const { return is_active; }

void NetworkInterface::print() const {
    cout << "  Interface: " << name << "\n";
    cout << "    IP:        " << ip_address << "\n";
    cout << "    Netmask:   " << subnet_mask << "\n";
    cout << "    Broadcast: " << broadcast_address << "\n";
    cout << "    Status:    " << (is_active ? "ACTIVE" : "INACTIVE") << "\n";
}

// ============================================================================
// WINDOWS IMPLEMENTATION
// ============================================================================
#ifdef _WIN32

vector<NetworkInterface> NetworkInterfaceManager::enumerateInterfaces() {
    vector<NetworkInterface> interfaces;
    
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        cerr << "WSAStartup failed\n";
        return interfaces;
    }
    
    ULONG bufferSize = 0;
    if (GetAdaptersInfo(NULL, &bufferSize) != ERROR_BUFFER_OVERFLOW) {
        WSACleanup();
        return interfaces;
    }
    
    PIP_ADAPTER_INFO pAdapterInfo = (IP_ADAPTER_INFO*)malloc(bufferSize);
    if (pAdapterInfo == NULL) {
        WSACleanup();
        return interfaces;
    }
    
    if (GetAdaptersInfo(pAdapterInfo, &bufferSize) != NO_ERROR) {
        free(pAdapterInfo);
        WSACleanup();
        return interfaces;
    }
    
    PIP_ADAPTER_INFO pAdapter = pAdapterInfo;
    while (pAdapter != NULL) {
        if (pAdapter->Type == MIB_IF_TYPE_ETHERNET || 
            pAdapter->Type == IF_TYPE_IEEE80211) {
            
            string ip = pAdapter->IpAddressList.IpAddress.String;
            string mask = pAdapter->IpAddressList.IpMask.String;
            
            if (ip != "0.0.0.0" && ip != "127.0.0.1") {
                string broadcast = calculateBroadcastAddress(ip, mask);
                bool is_active = (ip != "0.0.0.0");
                
                // Use Description instead of AdapterName for user-friendly name
                string name = pAdapter->Description;  // Changed this line
                
                interfaces.emplace_back(
                    name,        // Use description (e.g., "Intel WiFi Adapter")
                    ip,
                    mask,
                    broadcast,
                    is_active
                );
            }
        }
        pAdapter = pAdapter->Next;
    }
    
    free(pAdapterInfo);
    WSACleanup();
    
    return interfaces;
}

// ============================================================================
// LINUX/MAC IMPLEMENTATION
// ============================================================================
#else

vector<NetworkInterface> NetworkInterfaceManager::enumerateInterfaces() {
    vector<NetworkInterface> interfaces;
    
    struct ifaddrs *ifaddr, *ifa;
    
    if (getifaddrs(&ifaddr) == -1) {
        perror("getifaddrs");
        return interfaces;
    }
    
    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL) {
            continue;
        }
        
        if (ifa->ifa_addr->sa_family == AF_INET) {
            struct sockaddr_in* addr_in = (struct sockaddr_in*)ifa->ifa_addr;
            struct sockaddr_in* netmask_in = (struct sockaddr_in*)ifa->ifa_netmask;
            struct sockaddr_in* broadcast_in = (struct sockaddr_in*)ifa->ifa_broadaddr;
            
            char ip_str[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &(addr_in->sin_addr), ip_str, INET_ADDRSTRLEN);
            string ip = ip_str;
            
            string netmask = "255.255.255.0";
            if (netmask_in != NULL) {
                char mask_str[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &(netmask_in->sin_addr), mask_str, INET_ADDRSTRLEN);
                netmask = mask_str;
            }
            
            string broadcast;
            if (broadcast_in != NULL) {
                char bcast_str[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &(broadcast_in->sin_addr), bcast_str, INET_ADDRSTRLEN);
                broadcast = bcast_str;
            } else {
                broadcast = calculateBroadcastAddress(ip, netmask);
            }
            
            if (ip != "127.0.0.1" && ip != "0.0.0.0") {
                bool is_active = (ifa->ifa_flags & IFF_UP) && 
                                (ifa->ifa_flags & IFF_RUNNING);
                
                interfaces.emplace_back(
                    ifa->ifa_name,
                    ip,
                    netmask,
                    broadcast,
                    is_active
                );
            }
        }
    }
    
    freeifaddrs(ifaddr);
    
    return interfaces;
}

#endif

// ============================================================================
// COMMON FUNCTIONS
// ============================================================================

vector<NetworkInterface> NetworkInterfaceManager::getActiveInterfaces() {
    vector<NetworkInterface> all = enumerateInterfaces();
    vector<NetworkInterface> active;
    
    for (const auto& iface : all) {
        if (iface.isActive()) {
            active.push_back(iface);
        }
    }
    
    return active;
}

string NetworkInterfaceManager::calculateBroadcastAddress(const string& ip, const string& subnet_mask) {
    uint32_t ip_uint = ipToUint32(ip);
    uint32_t mask_uint = ipToUint32(subnet_mask);
    
    uint32_t network = ip_uint & mask_uint;
    uint32_t host_part = ~mask_uint;
    uint32_t broadcast = network | host_part;
    
    return uint32ToIp(broadcast);
}

uint32_t NetworkInterfaceManager::ipToUint32(const string& ip) {
    uint32_t result = 0;
    istringstream iss(ip);
    string octet;
    int shift = 24;
    
    while (getline(iss, octet, '.') && shift >= 0) {
        uint32_t value = stoi(octet);
        result |= (value << shift);
        shift -= 8;
    }
    
    return result;
}

string NetworkInterfaceManager::uint32ToIp(uint32_t ip) {
    ostringstream oss;
    
    oss << ((ip >> 24) & 0xFF) << "."
        << ((ip >> 16) & 0xFF) << "."
        << ((ip >> 8) & 0xFF) << "."
        << (ip & 0xFF);
    
    return oss.str();
}

void NetworkInterfaceManager::printInterfaces(const vector<NetworkInterface>& interfaces) {
    if (interfaces.empty()) {
        cout << "No network interfaces found.\n";
        return;
    }
    
    cout << "\n=== Network Interfaces ===\n";
    cout << "Found " << interfaces.size() << " interface(s):\n\n";
    
    for (const auto& iface : interfaces) {
        iface.print();
        cout << "\n";
    }
}