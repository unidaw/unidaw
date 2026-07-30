#pragma once

// A MINIMAL ZIP CONTAINER — the `.uni` module (docs/SAMPLER_DESIGN.md R3).
//
// "The project is a module, and it is called .uni" — a zip holding project.json plus a samples/
// directory, exactly as MOD, XM, IT, Renoise and Live all do it. Broken sample links stop
// existing, and sending someone a song is sending them one file.
//
// STORED, NOT DEFLATED, and that is a decision rather than laziness:
//
//   * the payload is mostly WAV, which deflates by 10-20% — paying a compressor for that on
//     every save is a poor trade against save latency on a large kit;
//   * a stored entry is BYTE-IDENTICAL inside the archive, so a content key computed on the
//     file matches one computed on the extracted copy. With deflate those two would differ and
//     "has this sample changed?" would need decompressing to answer;
//   * MOD and XM are not compressed either. A module is a container.
//
// Deflate can be added later behind the same interface: readers already handle both methods, and
// the writer would choose per entry. It is not needed to ship the format.
//
// NO DEPENDENCY, deliberately. project_file.cpp is shared with daw_lint, which does not link
// JUCE — so JUCE's ZipFile is unavailable here, and pulling in a zip library for ~200 lines of
// well-specified format would be a dependency for the linter too.

#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace daw {

// CRC-32 (IEEE), which the format requires and readers verify. A zip whose CRCs are wrong opens
// in this reader and fails in every other tool — the worst kind of "works on my machine".
inline uint32_t zipCrc32(const uint8_t* data, size_t len, uint32_t crc = 0) {
  static uint32_t table[256];
  static bool built = false;
  if (!built) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) {
        c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      }
      table[i] = c;
    }
    built = true;
  }
  crc = ~crc;
  for (size_t i = 0; i < len; ++i) {
    crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  }
  return ~crc;
}

struct ZipEntry {
  std::string name;
  std::vector<uint8_t> data;
};

namespace zip_detail {

inline void put16(std::vector<uint8_t>& out, uint16_t v) {
  out.push_back(static_cast<uint8_t>(v & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}
inline void put32(std::vector<uint8_t>& out, uint32_t v) {
  for (int i = 0; i < 4; ++i) {
    out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
  }
}
inline uint16_t get16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (p[1] << 8));
}
inline uint32_t get32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

}  // namespace zip_detail

// Builds the archive bytes. Returns them rather than writing, so the caller can do the
// temp-file-then-rename dance that makes a save atomic — a half-written module is a lost song,
// and this format has no way to be partially valid.
inline std::vector<uint8_t> zipBuild(const std::vector<ZipEntry>& entries) {
  using namespace zip_detail;
  std::vector<uint8_t> out;
  struct Central {
    uint32_t crc, size, offset;
    std::string name;
  };
  std::vector<Central> central;
  central.reserve(entries.size());

  for (const auto& e : entries) {
    const uint32_t offset = static_cast<uint32_t>(out.size());
    const uint32_t crc = zipCrc32(e.data.data(), e.data.size());
    const uint32_t size = static_cast<uint32_t>(e.data.size());
    put32(out, 0x04034b50);              // local file header
    put16(out, 20);                      // version needed (2.0 = stored/deflate)
    put16(out, 0);                       // flags
    put16(out, 0);                       // method 0 = STORED
    put16(out, 0);                       // mod time
    // A FIXED DATE, not the wall clock. A module saved twice with identical content must be
    // byte-identical, or every save shows as a change in git and "did anything actually change?"
    // stops being answerable. 1980-01-01 is the format's own epoch.
    put16(out, 33);                      // mod date: 1980-01-01
    put32(out, crc);
    put32(out, size);                    // compressed
    put32(out, size);                    // uncompressed
    put16(out, static_cast<uint16_t>(e.name.size()));
    put16(out, 0);                       // extra length
    out.insert(out.end(), e.name.begin(), e.name.end());
    out.insert(out.end(), e.data.begin(), e.data.end());
    central.push_back({crc, size, offset, e.name});
  }

  const uint32_t cdStart = static_cast<uint32_t>(out.size());
  for (const auto& c : central) {
    put32(out, 0x02014b50);              // central directory header
    put16(out, 20);                      // version made by
    put16(out, 20);                      // version needed
    put16(out, 0);
    put16(out, 0);                       // STORED
    put16(out, 0);
    put16(out, 33);
    put32(out, c.crc);
    put32(out, c.size);
    put32(out, c.size);
    put16(out, static_cast<uint16_t>(c.name.size()));
    put16(out, 0);                       // extra
    put16(out, 0);                       // comment
    put16(out, 0);                       // disk
    put16(out, 0);                       // internal attrs
    put32(out, 0);                       // external attrs
    put32(out, c.offset);
    out.insert(out.end(), c.name.begin(), c.name.end());
  }
  const uint32_t cdSize = static_cast<uint32_t>(out.size()) - cdStart;

  put32(out, 0x06054b50);                // end of central directory
  put16(out, 0);
  put16(out, 0);
  put16(out, static_cast<uint16_t>(central.size()));
  put16(out, static_cast<uint16_t>(central.size()));
  put32(out, cdSize);
  put32(out, cdStart);
  put16(out, 0);                         // comment length
  return out;
}

// Reads every entry. Returns false on anything malformed rather than half a document — a module
// that opens with three of its four samples is worse than one that refuses, because the missing
// one is only discovered when that pad is played.
inline bool zipRead(const std::vector<uint8_t>& bytes, std::vector<ZipEntry>& out,
                    std::string* error = nullptr) {
  using namespace zip_detail;
  auto fail = [&](const char* why) {
    if (error) {
      *error = why;
    }
    return false;
  };
  if (bytes.size() < 22) {
    return fail("too small to be a zip");
  }
  // The EOCD is at the end, possibly behind a comment, so it is searched backwards.
  size_t eocd = 0;
  bool found = false;
  for (size_t i = bytes.size() - 22; ; --i) {
    if (get32(&bytes[i]) == 0x06054b50) {
      eocd = i;
      found = true;
      break;
    }
    if (i == 0) {
      break;
    }
  }
  if (!found) {
    return fail("no end-of-central-directory record");
  }
  const uint16_t count = get16(&bytes[eocd + 10]);
  const uint32_t cdSize = get32(&bytes[eocd + 12]);
  const uint32_t cdStart = get32(&bytes[eocd + 16]);
  if (static_cast<size_t>(cdStart) + cdSize > bytes.size()) {
    return fail("central directory runs past the end of the file");
  }

  size_t p = cdStart;
  out.clear();
  out.reserve(count);
  for (uint16_t i = 0; i < count; ++i) {
    if (p + 46 > bytes.size() || get32(&bytes[p]) != 0x02014b50) {
      return fail("malformed central directory entry");
    }
    const uint16_t method = get16(&bytes[p + 10]);
    const uint32_t crc = get32(&bytes[p + 16]);
    const uint32_t csize = get32(&bytes[p + 20]);
    const uint32_t usize = get32(&bytes[p + 24]);
    const uint16_t nameLen = get16(&bytes[p + 28]);
    const uint16_t extraLen = get16(&bytes[p + 30]);
    const uint16_t commentLen = get16(&bytes[p + 32]);
    const uint32_t localOff = get32(&bytes[p + 42]);
    if (p + 46 + nameLen > bytes.size()) {
      return fail("central directory name runs past the end");
    }
    ZipEntry e;
    e.name.assign(reinterpret_cast<const char*>(&bytes[p + 46]), nameLen);
    p += 46 + nameLen + extraLen + commentLen;

    if (method != 0) {
      // Deflate is legal zip and this reader does not do it. REFUSED rather than skipped: a
      // module missing one sample is only discovered when that pad is played.
      return fail("compressed entry (this container is stored-only)");
    }
    if (localOff + 30 > bytes.size() || get32(&bytes[localOff]) != 0x04034b50) {
      return fail("bad local file header");
    }
    const uint16_t lnameLen = get16(&bytes[localOff + 26]);
    const uint16_t lextraLen = get16(&bytes[localOff + 28]);
    const size_t dataAt = localOff + 30 + lnameLen + lextraLen;
    if (dataAt + csize > bytes.size()) {
      return fail("entry data runs past the end of the file");
    }
    e.data.assign(bytes.begin() + dataAt, bytes.begin() + dataAt + csize);
    if (e.data.size() != usize) {
      return fail("entry size disagrees with its header");
    }
    // THE CRC IS VERIFIED. A container that accepts corrupt entries turns "my song sounds wrong"
    // into a mystery; a container that refuses turns it into a message.
    if (zipCrc32(e.data.data(), e.data.size()) != crc) {
      return fail("entry failed its CRC — the module is corrupt");
    }
    out.push_back(std::move(e));
  }
  return true;
}

inline bool zipWriteFile(const std::string& path, const std::vector<ZipEntry>& entries) {
  const std::vector<uint8_t> bytes = zipBuild(entries);
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) {
    return false;
  }
  f.write(reinterpret_cast<const char*>(bytes.data()),
          static_cast<std::streamsize>(bytes.size()));
  return static_cast<bool>(f);
}

inline bool zipReadFile(const std::string& path, std::vector<ZipEntry>& out,
                        std::string* error = nullptr) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) {
    if (error) {
      *error = "cannot open " + path;
    }
    return false;
  }
  const std::streamsize n = f.tellg();
  f.seekg(0);
  std::vector<uint8_t> bytes(static_cast<size_t>(n));
  if (n > 0 && !f.read(reinterpret_cast<char*>(bytes.data()), n)) {
    if (error) {
      *error = "cannot read " + path;
    }
    return false;
  }
  return zipRead(bytes, out, error);
}

}  // namespace daw
