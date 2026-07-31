#include "apps/event_log.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <mutex>

namespace daw {
namespace {

std::mutex& sinkMutex() {
  static std::mutex mutex;
  return mutex;
}

// Opened once on first use; nullptr means "write to stderr".
std::FILE* sinkFile() {
  static std::FILE* file = []() -> std::FILE* {
    const char* path = std::getenv("DAW_EVENT_LOG");
    if (path == nullptr || *path == '\0') {
      return nullptr;
    }
    std::FILE* handle = std::fopen(path, "a");
    if (handle == nullptr) {
      std::cerr << "event_log: cannot open " << path << ", using stderr"
                << std::endl;
    }
    return handle;
  }();
  return file;
}

uint64_t nowMillis() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

std::string escape(const std::string& value) {
  std::string result;
  result.reserve(value.size() + 8);
  for (const char c : value) {
    switch (c) {
      case '"': result += "\\\""; break;
      case '\\': result += "\\\\"; break;
      case '\n': result += "\\n"; break;
      case '\r': result += "\\r"; break;
      case '\t': result += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[7];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          result += buf;
        } else {
          result += c;
        }
        break;
    }
  }
  return result;
}

}  // namespace

bool eventLogEnabled() {
  static const bool enabled = [] {
    const char* off = std::getenv("DAW_EVENT_LOG_OFF");
    return off == nullptr || *off == '\0' || *off == '0';
  }();
  return enabled;
}

LogEvent::LogEvent(const char* name) {
  if (!eventLogEnabled()) {
    return;
  }
  buffer_ << "{\"ts_ms\":" << nowMillis() << ",\"event\":\""
          << escape(name ? name : "") << "\"";
}

LogEvent::~LogEvent() {
  if (!eventLogEnabled()) {
    return;
  }
  buffer_ << "}\n";
  const std::string line = buffer_.str();
  std::lock_guard<std::mutex> lock(sinkMutex());
  if (std::FILE* file = sinkFile()) {
    std::fwrite(line.data(), 1, line.size(), file);
    std::fflush(file);
  } else {
    std::fwrite(line.data(), 1, line.size(), stderr);
  }
}

LogLine::~LogLine() {
  std::string line = buffer_.str();
  if (line.empty()) {
    return;
  }
  // Call sites end with std::endl, which has already put the '\n' in the buffer. One that
  // does not still gets a whole line — the point is that a line is written in one call, so
  // "the newline is somebody else's job" would reintroduce exactly the interleaving this
  // class exists to stop.
  if (line.back() != '\n') {
    line.push_back('\n');
  }
  // The same mutex LogEvent uses, so a diagnostic and a structured event can never land
  // inside one another when both are going to stderr.
  std::lock_guard<std::mutex> lock(sinkMutex());
  std::fwrite(line.data(), 1, line.size(), stderr);
}

void LogEvent::writeKey(const char* key) {
  buffer_ << ",\"" << escape(key ? key : "") << "\":";
}

LogEvent& LogEvent::field(const char* key, uint64_t value) {
  if (eventLogEnabled()) {
    writeKey(key);
    buffer_ << value;
  }
  return *this;
}

LogEvent& LogEvent::field(const char* key, uint32_t value) {
  return field(key, static_cast<uint64_t>(value));
}

LogEvent& LogEvent::field(const char* key, int64_t value) {
  if (eventLogEnabled()) {
    writeKey(key);
    buffer_ << value;
  }
  return *this;
}

LogEvent& LogEvent::field(const char* key, int value) {
  return field(key, static_cast<int64_t>(value));
}

LogEvent& LogEvent::field(const char* key, double value) {
  if (eventLogEnabled()) {
    writeKey(key);
    buffer_ << value;
  }
  return *this;
}

LogEvent& LogEvent::field(const char* key, bool value) {
  if (eventLogEnabled()) {
    writeKey(key);
    buffer_ << (value ? "true" : "false");
  }
  return *this;
}

LogEvent& LogEvent::field(const char* key, const std::string& value) {
  if (eventLogEnabled()) {
    writeKey(key);
    buffer_ << "\"" << escape(value) << "\"";
  }
  return *this;
}

LogEvent& LogEvent::field(const char* key, const char* value) {
  return field(key, std::string(value ? value : ""));
}

}  // namespace daw
