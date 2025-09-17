#pragma once
#include "config.hpp"
#include <thread>
#include <atomic>

using namespace std;

class Discovery {
private:
    static inline thread senderThread;
    static inline thread listenerThread;
    static inline thread cleanupThread;
    static inline atomic<bool> running{false};

public:
    static void start(Config& cfg, int discovery_port = 5001, int interval_sec = 5, int timeout_sec = 60);
    static void stop();
    static bool isRunning() { return running.load(); }
};
