#pragma once
#include <cstdio>
#ifndef __ANDROID__
#include <format>   // on Android, platform/compat.h provides it
#endif
#include <string>

void Log_Open(const char* path);
// Path last passed to Log_Open (empty if logging not opened yet).
const std::string& Log_Path();
void Log_Write(int level, const std::string& s);
// Leave a marker for the next launch (crash / fatal). Safe to call from fault handlers.
void Log_MarkAbnormalExit(const char* reason);
// True once if the previous run left a marker; clears the file and returns its reason.
bool Log_TakeAbnormalExit(std::string& reason);

#define LOG_DEBUG(...) Log_Write(0, std::format(__VA_ARGS__))
#define LOG_INFO(...) Log_Write(1, std::format(__VA_ARGS__))
#define LOG_WARN(...) Log_Write(2, std::format(__VA_ARGS__))
#define LOG_ERROR(...) Log_Write(3, std::format(__VA_ARGS__))

[[noreturn]] void Fatal(const std::string& s);
#define FATAL(...) Fatal(std::format(__VA_ARGS__))
