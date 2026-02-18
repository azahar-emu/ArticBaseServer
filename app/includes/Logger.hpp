#pragma once
#include "3ds.h"
#include "string"
#include "queue"
#include "CTRPluginFramework/System/Mutex.hpp"
#include "CTRPluginFramework/System/Lock.hpp"

class Logger {
public:
    Logger();
    void Start();
    void End();
    ~Logger();
    void Raw(bool isTopScr, const char* fmt, ...);
    void Info(const char* fmt, ...);
    void Debug(const char* fmt, ...);
    void Warning(const char* fmt, ...);
    void Error(const char* fmt, ...);
    void Traffic(const char* fmt, ...);
    void BrokenScreen(); // Usually some 3DSes have a broken top screen, important information
                        // should be displayed on the bottom screen instead.
    bool GetOverrideState() { return overrideTop; }

    void Wait();

    bool debug_enable = false;
private:
    struct PendingLog {
        enum class Type : u8 {
            RAW,
            DEBUG,
            INFO,
            WARNING,
            ERROR,
            TRAFFIC,
        };
        Type type;
        bool isTopScr = true;
        std::string string;
    };
    static void LoggerThread(void* arg);
    void Handler();
    Thread thread;
    LightEvent event;
    bool overrideTop = false;
    
    std::queue<PendingLog> pendingLogs;
    CTRPluginFramework::Mutex pendingLogsMutex;

    bool run = true;
};
