#pragma once
#include <string>

// 日志级别（数值越大越严重）
enum class LogLevel : int {
    Trace = 0,
    Debug = 1,
    Info  = 2,
    Warn  = 3,
    Error = 4,
    None  = 5, // 不输出任何日志
};

// 全局日志初始化。
// logDir  : 日志目录（如 "logs"），不存在时自动创建
// fileName: 日志文件名（如 "CulturesGameExtend.log"）
// minLevel: 最低输出级别，低于该级别的日志被过滤
void LogInit(const std::string& logDir, const std::string& fileName, LogLevel minLevel);

// 写一行日志。
// category: 分类前缀，通常为 Feature 名称（如 "[CulturesPatches]"）
void LogWrite(const char* category, LogLevel level, const char* fmt, ...);

// 便捷宏：自动带上分类前缀
#define LOG_TRACE(cat, ...) LogWrite(cat, LogLevel::Trace, __VA_ARGS__)
#define LOG_DEBUG(cat, ...) LogWrite(cat, LogLevel::Debug, __VA_ARGS__)
#define LOG_INFO(cat,  ...) LogWrite(cat, LogLevel::Info,  __VA_ARGS__)
#define LOG_WARN(cat,  ...) LogWrite(cat, LogLevel::Warn,  __VA_ARGS__)
#define LOG_ERROR(cat, ...) LogWrite(cat, LogLevel::Error, __VA_ARGS__)
