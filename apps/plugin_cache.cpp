#include "apps/plugin_cache.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <fstream>
#include <sstream>

#include <fcntl.h>
#include <unistd.h>

namespace daw {
namespace {

class JsonCursor {
 public:
  explicit JsonCursor(const std::string& input) : input_(input) {}

  void skipWhitespace() {
    while (pos_ < input_.size()) {
      const char c = input_[pos_];
      if (c == ' ' || c == '\n' || c == '\t' || c == '\r') {
        ++pos_;
      } else {
        break;
      }
    }
  }

  bool consume(char expected) {
    skipWhitespace();
    if (pos_ < input_.size() && input_[pos_] == expected) {
      ++pos_;
      return true;
    }
    return false;
  }

  bool peek(char expected) {
    skipWhitespace();
    return pos_ < input_.size() && input_[pos_] == expected;
  }

  std::string parseString() {
    skipWhitespace();
    if (pos_ >= input_.size() || input_[pos_] != '"') {
      return {};
    }
    ++pos_;
    std::string out;
    while (pos_ < input_.size()) {
      const char c = input_[pos_++];
      if (c == '"') {
        break;
      }
      if (c == '\\' && pos_ < input_.size()) {
        const char esc = input_[pos_++];
        switch (esc) {
          case '"':
            out.push_back('"');
            break;
          case '\\':
            out.push_back('\\');
            break;
          case '/':
            out.push_back('/');
            break;
          case 'b':
            out.push_back('\b');
            break;
          case 'f':
            out.push_back('\f');
            break;
          case 'n':
            out.push_back('\n');
            break;
          case 'r':
            out.push_back('\r');
            break;
          case 't':
            out.push_back('\t');
            break;
          case 'u':
            pos_ += 4;
            break;
          default:
            out.push_back(esc);
            break;
        }
      } else {
        out.push_back(c);
      }
    }
    return out;
  }

  bool parseBool(bool& value) {
    skipWhitespace();
    if (input_.compare(pos_, 4, "true") == 0) {
      pos_ += 4;
      value = true;
      return true;
    }
    if (input_.compare(pos_, 5, "false") == 0) {
      pos_ += 5;
      value = false;
      return true;
    }
    return false;
  }

  bool parseInt64(int64_t& value) {
    skipWhitespace();
    if (pos_ >= input_.size()) {
      return false;
    }
    size_t start = pos_;
    if (input_[pos_] == '-') {
      ++pos_;
    }
    bool sawDigit = false;
    while (pos_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[pos_]))) {
      sawDigit = true;
      ++pos_;
    }
    if (!sawDigit) {
      pos_ = start;
      return false;
    }
    const std::string token = input_.substr(start, pos_ - start);
    std::istringstream stream(token);
    stream >> value;
    return !stream.fail();
  }

  void skipValue() {
    skipWhitespace();
    if (pos_ >= input_.size()) {
      return;
    }
    const char c = input_[pos_];
    if (c == '"') {
      parseString();
    } else if (c == '{') {
      consume('{');
      while (!peek('}') && pos_ < input_.size()) {
        parseString();
        consume(':');
        skipValue();
        if (!consume(',')) {
          break;
        }
      }
      consume('}');
    } else if (c == '[') {
      consume('[');
      while (!peek(']') && pos_ < input_.size()) {
        skipValue();
        if (!consume(',')) {
          break;
        }
      }
      consume(']');
    } else {
      while (pos_ < input_.size()) {
        const char t = input_[pos_];
        if (t == ',' || t == '}' || t == ']') {
          break;
        }
        ++pos_;
      }
    }
  }

 private:
  const std::string& input_;
  size_t pos_ = 0;
};

std::string escapeJson(const std::string& input) {
  std::string out;
  out.reserve(input.size() + 8);
  for (const char c : input) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buffer[7];
          std::snprintf(buffer, sizeof(buffer), "\\u%04x",
                        static_cast<unsigned int>(static_cast<unsigned char>(c)));
          out += buffer;
        } else {
          out += c;
        }
        break;
    }
  }
  return out;
}

bool fsyncFileDescriptor(int fd) {
  if (fd < 0) {
    return false;
  }
  return ::fsync(fd) == 0;
}

bool fsyncPath(const std::string& path) {
  const int fd = ::open(path.c_str(), O_RDONLY);
  const bool ok = fsyncFileDescriptor(fd);
  if (fd >= 0) {
    ::close(fd);
  }
  return ok;
}

}  // namespace

std::string scanStatusToString(ScanStatus status) {
  switch (status) {
    case ScanStatus::Ok:
      return "OK";
    case ScanStatus::Timeout:
      return "TIMEOUT";
    case ScanStatus::Failed:
    default:
      return "FAILED";
  }
}

ScanStatus scanStatusFromString(const std::string& value) {
  if (value == "OK") {
    return ScanStatus::Ok;
  }
  if (value == "TIMEOUT") {
    return ScanStatus::Timeout;
  }
  return ScanStatus::Failed;
}

PluginCache readPluginCache(const std::string& path) {
  std::ifstream in(path);
  PluginCache cache;
  if (!in) {
    return cache;
  }

  std::string data((std::istreambuf_iterator<char>(in)),
                   std::istreambuf_iterator<char>());
  JsonCursor cursor(data);
  if (!cursor.consume('{')) {
    return cache;
  }

  while (!cursor.peek('}')) {
    const std::string key = cursor.parseString();
    if (key.empty()) {
      break;
    }
    cursor.consume(':');
    if (key == "schema_version") {
      int64_t value = 0;
      if (cursor.parseInt64(value)) {
        cache.schemaVersion = static_cast<int>(value);
      } else {
        cursor.skipValue();
      }
    } else if (key == "generated_at_ms") {
      cursor.parseInt64(cache.generatedAtMs);
    } else if (key == "plugins") {
      if (!cursor.consume('[')) {
        break;
      }
      while (!cursor.peek(']')) {
        PluginCacheEntry entry;
        if (!cursor.consume('{')) {
          break;
        }
        while (!cursor.peek('}')) {
          const std::string field = cursor.parseString();
          cursor.consume(':');
          if (field == "path") {
            entry.path = cursor.parseString();
          } else if (field == "plugin_id_string") {
            entry.pluginIdString = cursor.parseString();
          } else if (field == "plugin_uid16") {
            entry.pluginUid16 = cursor.parseString();
          } else if (field == "name") {
            entry.name = cursor.parseString();
          } else if (field == "vendor") {
            entry.vendor = cursor.parseString();
          } else if (field == "version") {
            entry.version = cursor.parseString();
          } else if (field == "category") {
            entry.category = cursor.parseString();
          } else if (field == "has_editor") {
            cursor.parseBool(entry.hasEditor);
          } else if (field == "is_instrument") {
            cursor.parseBool(entry.isInstrument);
          } else if (field == "num_inputs") {
            int64_t value = 0;
            if (cursor.parseInt64(value)) {
              entry.numInputChannels = static_cast<int>(value);
            } else {
              cursor.skipValue();
            }
          } else if (field == "num_outputs") {
            int64_t value = 0;
            if (cursor.parseInt64(value)) {
              entry.numOutputChannels = static_cast<int>(value);
            } else {
              cursor.skipValue();
            }
          } else if (field == "param_count") {
            int64_t value = 0;
            if (cursor.parseInt64(value)) {
              entry.paramCount = static_cast<int>(value);
            } else {
              cursor.skipValue();
            }
          } else if (field == "scan_status") {
            entry.scanStatus = scanStatusFromString(cursor.parseString());
          } else if (field == "error") {
            entry.error = cursor.parseString();
          } else if (field == "scan_time_ms") {
            cursor.parseInt64(entry.scanTimeMs);
          } else if (field == "next_retry_at_ms") {
            cursor.parseInt64(entry.nextRetryAtMs);
          } else {
            cursor.skipValue();
          }
          if (!cursor.consume(',')) {
            break;
          }
        }
        cursor.consume('}');
        cache.entries.push_back(std::move(entry));
        if (!cursor.consume(',')) {
          break;
        }
      }
      cursor.consume(']');
    } else {
      cursor.skipValue();
    }
    if (!cursor.consume(',')) {
      break;
    }
  }
  cursor.consume('}');
  return cache;
}

bool writePluginCacheAtomic(const std::string& path, const PluginCache& cache) {
  const std::string tmpPath = path + ".tmp";
  std::ofstream out(tmpPath);
  if (!out) {
    return false;
  }

  out << "{\n";
  out << "  \"schema_version\": " << cache.schemaVersion << ",\n";
  out << "  \"generated_at_ms\": " << cache.generatedAtMs << ",\n";
  out << "  \"plugins\": [\n";

  for (size_t i = 0; i < cache.entries.size(); ++i) {
    const auto& plugin = cache.entries[i];
    out << "    {\n";
    out << "      \"path\": \"" << escapeJson(plugin.path) << "\",\n";
    out << "      \"plugin_id_string\": \"" << escapeJson(plugin.pluginIdString) << "\",\n";
    out << "      \"plugin_uid16\": \"" << escapeJson(plugin.pluginUid16) << "\",\n";
    out << "      \"name\": \"" << escapeJson(plugin.name) << "\",\n";
    out << "      \"vendor\": \"" << escapeJson(plugin.vendor) << "\",\n";
    out << "      \"version\": \"" << escapeJson(plugin.version) << "\",\n";
    out << "      \"category\": \"" << escapeJson(plugin.category) << "\",\n";
    out << "      \"has_editor\": " << (plugin.hasEditor ? "true" : "false") << ",\n";
    out << "      \"is_instrument\": " << (plugin.isInstrument ? "true" : "false")
        << ",\n";
    out << "      \"num_inputs\": " << plugin.numInputChannels << ",\n";
    out << "      \"num_outputs\": " << plugin.numOutputChannels << ",\n";
    out << "      \"param_count\": " << plugin.paramCount << ",\n";
    out << "      \"scan_status\": \"" << scanStatusToString(plugin.scanStatus) << "\",\n";
    out << "      \"error\": \"" << escapeJson(plugin.error) << "\",\n";
    out << "      \"scan_time_ms\": " << plugin.scanTimeMs << ",\n";
    out << "      \"next_retry_at_ms\": " << plugin.nextRetryAtMs << "\n";
    out << "    }";
    if (i + 1 < cache.entries.size()) {
      out << ",";
    }
    out << "\n";
  }

  out << "  ]\n";
  out << "}\n";
  out.flush();
  if (!out) {
    return false;
  }
  out.close();

  const int fd = ::open(tmpPath.c_str(), O_RDONLY);
  const bool dataSynced = fsyncFileDescriptor(fd);
  if (fd >= 0) {
    ::close(fd);
  }
  if (!dataSynced) {
    return false;
  }
  if (::rename(tmpPath.c_str(), path.c_str()) != 0) {
    return false;
  }
  const auto dirPos = path.find_last_of('/');
  if (dirPos != std::string::npos) {
    fsyncPath(path.substr(0, dirPos));
  }
  return true;
}

}  // namespace daw
