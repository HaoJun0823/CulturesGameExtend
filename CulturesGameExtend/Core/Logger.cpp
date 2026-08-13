#include "pch.h"
#include "Logger.h"
#include "fs_compat.h"
#include <cstdio>
#include <cstdarg>
#include <mutex>
#include <fstream>

namespace {

std::mutex        g_logMutex;
std::ofstream     g_logFile;
LogLevel          g_minLevel = LogLevel::Info;
std::string       g_logPath;

// 独立 Warning 日志（GameWarnings.log）
std::mutex        g_warnMutex;
std::ofstream     g_warnFile;
std::string       g_warnPath;

const char* LevelName(LogLevel l) {
    switch (l) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Error: return "ERROR";
        default:              return "?????";
    }
}

// 线程安全的本地时间字符串 "YYYY-MM-DD HH:MM:SS"
std::string NowString() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

} // namespace

void LogInit(const std::string& logDir, const std::string& fileName, LogLevel minLevel) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    g_minLevel = minLevel;

    std::error_code ec;
    if (!logDir.empty()) {
        ge::fs::create_directories(logDir, ec);
    }

    g_logPath = logDir.empty() ? fileName : (logDir + "/" + fileName);

    // 覆盖模式打开：每次启动清空上次日志，只保留本次会话（游戏其他部分可能读日志）
    g_logFile.open(g_logPath, std::ios::out | std::ios::trunc);
    if (g_logFile.is_open()) {
        g_logFile << "---- [" << NowString() << "] CulturesGameExtend log session start ----\n";
        g_logFile.flush();
    }
}

void LogWrite(const char* category, LogLevel level, const char* fmt, ...) {
    if (level < g_minLevel) return;

    std::lock_guard<std::mutex> lock(g_logMutex);

    // 即使文件未打开也尝试输出到调试器，便于排查启动期问题
    char buffer[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    std::string line = "[" + NowString() + "] [" + LevelName(level) + "] "
                     + (category ? category : "") + " " + buffer + "\n";

    if (g_logFile.is_open()) {
        g_logFile << line;
        g_logFile.flush();
    }
    OutputDebugStringA(line.c_str());
}

// ===================================================================
// 独立 Warning 日志：记录游戏内 Warning! 开发者断言框（不再致命）。
// 与 g_logFile 并行，写到 logs/GameWarnings.log，同样 flush + OutputDebugString。
// ===================================================================
void WarnLogInit(const std::string& logDir, const std::string& fileName) {
    std::lock_guard<std::mutex> lock(g_warnMutex);
    std::error_code ec;
    if (!logDir.empty()) {
        ge::fs::create_directories(logDir, ec);
    }
    g_warnPath = logDir.empty() ? fileName : (logDir + "/" + fileName);
    g_warnFile.open(g_warnPath, std::ios::out | std::ios::trunc);
    if (g_warnFile.is_open()) {
        g_warnFile << "---- [" << NowString() << "] GameWarnings log session start ----\n";
        g_warnFile.flush();
    }
}

void WarnLogWrite(const char* fmt, ...) {
    if (!fmt) return;
    std::lock_guard<std::mutex> lock(g_warnMutex);

    char buffer[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    std::string line = "[" + NowString() + "] " + buffer + "\n";
    if (g_warnFile.is_open()) {
        g_warnFile << line;
        g_warnFile.flush();
    }
    OutputDebugStringA(line.c_str());
}
