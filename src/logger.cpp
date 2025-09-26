// Implementation of simple logger
#include "logger.h"

namespace ve {

static const char* to_str(LogLevel lvl) {
  switch (lvl) {
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info:  return " INFO";
    case LogLevel::Warn:  return " WARN";
    case LogLevel::Error: return "ERROR";
  }
  return " UNK ";
}

void Logger::write(LogLevel lvl, const char* tag, const std::string& msg) {
  using namespace std::chrono;
  const auto now = system_clock::now();
  const auto tt = system_clock::to_time_t(now);
  const auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;

  std::lock_guard<std::mutex> lock(mtx_);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &tt);
#else
  localtime_r(&tt, &tm);
#endif
  std::cerr << std::put_time(&tm, "%H:%M:%S") << '.'
            << std::setfill('0') << std::setw(3) << ms.count()
            << " [" << to_str(lvl) << "] " << tag << ": " << msg << '\n';
}

}  // namespace ve
