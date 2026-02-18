#include "Logger.hpp"
#include <stdarg.h>
#include <string.h>
#include <stdio.h>
#include "3ds.h"

Logger::Logger() {}

void Logger::Start() {
    LightEvent_Init(&event, ResetType::RESET_ONESHOT);

    s32 prio = 0;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);

    thread = threadCreate(LoggerThread, this, 0x300, prio + 1, -2, false);
}

void Logger::End() {
    run = false;
    LightEvent_Signal(&event);
    threadJoin(thread, U64_MAX);
}

Logger::~Logger() {
    if (run) End();
    threadFree(thread);
}

extern PrintConsole topScreenConsole, bottomScreenConsole;

void Logger::Raw(bool isTopScr, const char* fmt, ...) {
    va_list valist;
    char buffer[256];
    va_start(valist, fmt);
    int ret = vsnprintf(buffer, 255, fmt, valist);
    va_end(valist);
    if (ret >= 0) buffer[ret] = '\0';
    {
        CTRPluginFramework::Lock l(pendingLogsMutex);
        pendingLogs.push(PendingLog{.type = PendingLog::Type::RAW, .isTopScr = isTopScr, .string{buffer}});
    }
    LightEvent_Signal(&event);
}

void Logger::Info(const char* fmt, ...) {
    va_list valist;
    char buffer[256];
    va_start(valist, fmt);
    int ret = vsnprintf(buffer, 255, fmt, valist);
    va_end(valist);
    if (ret >= 0) buffer[ret] = '\0';
    {
        CTRPluginFramework::Lock l(pendingLogsMutex);
        pendingLogs.push(PendingLog{.type = PendingLog::Type::INFO, .isTopScr = (overrideTop ? false : true), .string{buffer}});
    }
    LightEvent_Signal(&event);
}

void Logger::Debug(const char* fmt, ...) {
    if (!debug_enable) return;
    va_list valist;
    char buffer[256];
    va_start(valist, fmt);
    int ret = vsnprintf(buffer, 255, fmt, valist);
    va_end(valist);
    if (ret >= 0) buffer[ret] = '\0';
    {
        CTRPluginFramework::Lock l(pendingLogsMutex);
        pendingLogs.push(PendingLog{.type = PendingLog::Type::DEBUG, .isTopScr = (overrideTop ? false : true), .string{buffer}});
    }
    LightEvent_Signal(&event);
}

void Logger::Warning(const char* fmt, ...) {
    va_list valist;
    char buffer[256];
    va_start(valist, fmt);
    int ret = vsnprintf(buffer, 255, fmt, valist);
    va_end(valist);
    if (ret >= 0) buffer[ret] = '\0';
    {
        CTRPluginFramework::Lock l(pendingLogsMutex);
        pendingLogs.push(PendingLog{.type = PendingLog::Type::WARNING, .isTopScr = (overrideTop ? false : true), .string{buffer}});
    }
    LightEvent_Signal(&event);
}

void Logger::Error(const char* fmt, ...) {
    va_list valist;
    char buffer[256];
    va_start(valist, fmt);
    int ret = vsnprintf(buffer, 255, fmt, valist);
    va_end(valist);
    if (ret >= 0) buffer[ret] = '\0';
    {
        CTRPluginFramework::Lock l(pendingLogsMutex);
        pendingLogs.push(PendingLog{.type = PendingLog::Type::ERROR, .isTopScr = (overrideTop ? false : true), .string{buffer}});
    }
    LightEvent_Signal(&event);
}

void Logger::Traffic(const char* fmt, ...) {
    va_list valist;
    char buffer[256];
    va_start(valist, fmt);
    int ret = vsnprintf(buffer, 255, fmt, valist);
    va_end(valist);
    if (ret >= 0) buffer[ret] = '\0';
    {
        CTRPluginFramework::Lock l(pendingLogsMutex);
        pendingLogs.push(PendingLog{.type = PendingLog::Type::TRAFFIC, .isTopScr = false, .string{buffer}});
    }
    LightEvent_Signal(&event);
}

void Logger::BrokenScreen() {
    overrideTop = !overrideTop;
}

void Logger::Handler() {
    bool currentIsTop = true;
    int back;
    while (true) {
        LightEvent_Wait(&event);
        if (!run) {
            break;
        }
        while (run) {
            PendingLog log;
            {
                CTRPluginFramework::Lock l(pendingLogsMutex);
                if (!pendingLogs.size())
                    break;
                log = pendingLogs.front();
                pendingLogs.pop();
            }
            if (log.isTopScr && !currentIsTop) {
                currentIsTop = true;
                consoleSelect(&topScreenConsole);
            }
            if (!log.isTopScr && currentIsTop) {
                currentIsTop = false;
                consoleSelect(&bottomScreenConsole);
            }
            switch (log.type)
            {
            case PendingLog::Type::RAW:
                printf("%s\n", log.string.c_str());
                break;
            case PendingLog::Type::DEBUG:
                printf("[D] %s\n", log.string.c_str());
                break;
            case PendingLog::Type::INFO:
                printf("[I] %s\n", log.string.c_str());
                break;
            case PendingLog::Type::WARNING:
                topScreenConsole.fg = 19;
                printf("[W] %s\n", log.string.c_str());
                topScreenConsole.fg = 0;
                break;
            case PendingLog::Type::ERROR:
                topScreenConsole.fg = 17;
                printf("[E] %s\n", log.string.c_str());
                topScreenConsole.fg = 0;
                break;
            case PendingLog::Type::TRAFFIC:
                back = bottomScreenConsole.cursorY;
                bottomScreenConsole.cursorY = 25;
                printf(log.string.c_str());
                bottomScreenConsole.cursorY = back;
                break;
            default:
                break;
            }
        }
    }
}

void Logger::LoggerThread(void* arg) {
    Logger* l = (Logger*)arg;
    l->Handler();
}
