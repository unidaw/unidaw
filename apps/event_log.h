#pragma once

#include <cstdint>
#include <ostream>
#include <sstream>
#include <string>

namespace daw {

// One structured event per line (JSONL), so engine behaviour can be queried
// rather than grepped. Goes to stderr by default; set DAW_EVENT_LOG=<path> to
// send it to a file instead, which keeps it separate from plain diagnostics.
//
// NOT real-time safe: it allocates and does IO. Call it from the UI/control
// threads, never from the audio callback.
//
//   DAW_EVENT("clip.version_mismatch")
//       .field("base", baseVersion)
//       .field("current", current)
//       .field("track", trackId);
class LogEvent {
 public:
  explicit LogEvent(const char* name);
  ~LogEvent();

  LogEvent(const LogEvent&) = delete;
  LogEvent& operator=(const LogEvent&) = delete;

  LogEvent& field(const char* key, uint64_t value);
  LogEvent& field(const char* key, uint32_t value);
  LogEvent& field(const char* key, int64_t value);
  LogEvent& field(const char* key, int value);
  LogEvent& field(const char* key, double value);
  LogEvent& field(const char* key, bool value);
  LogEvent& field(const char* key, const std::string& value);
  LogEvent& field(const char* key, const char* value);

 private:
  void writeKey(const char* key);

  std::ostringstream buffer_;
};

// ONE PLAIN DIAGNOSTIC LINE, serialized against DAW_EVENT and against every other
// diagnostic line.
//
// `std::cerr << a << b << std::endl` is several unsynchronized operations on one stream
// object. ThreadSanitizer reports it as a data race, and it is one: main writing
// "UI: command thread launched" while the thread it has just spawned writes "UI: command
// thread started" is two threads touching the same buffer with nothing between them.
// Interleaved output is the visible half; the race on the stream's internals is the rest.
//
// DAW_EVENT never had this problem because it builds its whole line in a local buffer and
// emits it with ONE locked fwrite. This is the same discipline for the unstructured lines,
// sharing the same mutex so a diagnostic can never land inside an event either.
//
// Always goes to stderr, even when DAW_EVENT_LOG sends structured events to a file — that
// separation is the point of the variable.
//
//   daw::LogLine() << "UI: command thread started" << std::endl;
//
// Written as a temporary: it flushes in its destructor, at the end of the full expression.
class LogLine {
 public:
  LogLine() = default;
  ~LogLine();

  LogLine(const LogLine&) = delete;
  LogLine& operator=(const LogLine&) = delete;

  template <typename T>
  LogLine& operator<<(const T& value) {
    buffer_ << value;
    return *this;
  }
  // Manipulators — std::endl above all, so existing call sites convert unchanged. On the
  // internal stringstream it inserts '\n' and flushes nothing, which is exactly right.
  LogLine& operator<<(std::ostream& (*manip)(std::ostream&)) {
    buffer_ << manip;
    return *this;
  }

 private:
  std::ostringstream buffer_;
};

// True when structured events are enabled (default on). Set DAW_EVENT_LOG_OFF=1
// to silence them without touching call sites.
bool eventLogEnabled();

#define DAW_EVENT(name) ::daw::LogEvent(name)

}  // namespace daw
