// Simple logger with levels and timestamps
#pragma once

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>

namespace ve {

enum class LogLevel { Debug = 0, Info, Warn, Error };

class Logger {
 public:
  static void set_level(LogLevel lvl) { instance().level_ = lvl; }
  static LogLevel level() { return instance().level_; }

  template <typename... Args>
  static void log(LogLevel lvl, const char* tag, Args&&... args) {
    if (lvl < instance().level_) return;
    std::ostringstream oss;
    (oss << ... << std::forward<Args>(args));
    instance().write(lvl, tag, oss.str());
  }

 private:
  static Logger& instance() {
    static Logger inst;
    return inst;
  }

  void write(LogLevel lvl, const char* tag, const std::string& msg);

  LogLevel level_ =
#ifdef VE_DEBUG
      LogLevel::Debug
#else
      LogLevel::Info
#endif
      ;
  std::mutex mtx_;
};

}  // namespace ve

#define LOG_DEBUG(...) ::ve::Logger::log(::ve::LogLevel::Debug, "DEBUG", __VA_ARGS__)
#define LOG_INFO(...)  ::ve::Logger::log(::ve::LogLevel::Info,  " INFO", __VA_ARGS__)
#define LOG_WARN(...)  ::ve::Logger::log(::ve::LogLevel::Warn,  " WARN", __VA_ARGS__)
#define LOG_ERROR(...) ::ve::Logger::log(::ve::LogLevel::Error, "ERROR", __VA_ARGS__)
