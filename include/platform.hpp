#pragma once

// CRITICAL: These must be defined BEFORE any includes
#ifdef _WIN32
    #ifndef _HAS_STD_BYTE
        #define _HAS_STD_BYTE 0
    #endif
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
#endif

// Platform-specific socket includes
#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <windows.h>
    
    #pragma comment(lib, "ws2_32.lib")
    
    typedef SOCKET SocketType;
    #define INVALID_SOCKET_VAL INVALID_SOCKET
    #define SOCKET_ERROR_VAL SOCKET_ERROR
    
#else
    #include <sys/socket.h>
    #include <arpa/inet.h>
    #include <netinet/in.h>
    #include <unistd.h>
    
    typedef int SocketType;
    #define INVALID_SOCKET_VAL -1
    #define SOCKET_ERROR_VAL -1
    
    inline void closesocket(SocketType s) { close(s); }
#endif

namespace SocketUtils {
    inline bool initSockets() {
    #ifdef _WIN32
        WSADATA wsaData;
        return (WSAStartup(MAKEWORD(2, 2), &wsaData) == 0);
    #else
        return true;
    #endif
    }
    
    inline void cleanupSockets() {
    #ifdef _WIN32
        WSACleanup();
    #endif
    }
}