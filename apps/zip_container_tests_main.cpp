// THE `.uni` CONTAINER. A module you can send someone.
//
// Two properties carry the format, and both are about what happens when something is WRONG:
// a corrupt entry must be REFUSED rather than returned, and a save with identical content must
// be byte-identical so "did anything change?" stays answerable.
//
// The rest is the zip specification, which is only worth testing because a file this reader
// accepts and every other tool rejects is the worst possible outcome — you would not find out
// until you sent the song to someone.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "apps/zip_container.h"

namespace {

int g_fail = 0;

void check(bool ok, const char* what) {
  if (!ok) {
    std::printf("FAIL %s\n", what);
    ++g_fail;
  }
}

std::vector<uint8_t> bytesOf(const std::string& s) {
  return std::vector<uint8_t>(s.begin(), s.end());
}

std::string stringOf(const std::vector<uint8_t>& v) {
  return std::string(v.begin(), v.end());
}

}  // namespace

int main() {
  // ---- ROUND TRIP. Several entries, including a nested path and binary content.
  {
    std::vector<daw::ZipEntry> in{
        {"project.json", bytesOf("{\"schema_version\":4}")},
        {"samples/kick.wav", std::vector<uint8_t>{0, 1, 2, 255, 254, 0, 0, 7}},
        {"samples/snare.wav", bytesOf("RIFF....WAVEfmt ")},
    };
    const auto bytes = daw::zipBuild(in);
    std::vector<daw::ZipEntry> out;
    std::string err;
    check(daw::zipRead(bytes, out, &err), "a built archive reads back");
    check(out.size() == 3, "with all three entries");
    if (out.size() == 3) {
      check(out[0].name == "project.json", "names survive");
      check(out[1].name == "samples/kick.wav", "including nested paths — a module has a samples/ "
                                               "directory, and a flat namespace would collide the "
                                               "moment two kits both had a kick");
      check(stringOf(out[0].data) == "{\"schema_version\":4}", "text content survives");
      check(out[1].data.size() == 8 && out[1].data[3] == 255 && out[1].data[5] == 0,
            "BINARY content survives byte for byte, including embedded nulls and high bytes — "
            "audio is binary and a container that mangles it is not a container");
    }
  }

  // ---- AN EMPTY ARCHIVE is valid, and reads as empty rather than as an error. A project with
  // no samples yet is an ordinary state, not a failure.
  {
    const auto bytes = daw::zipBuild({});
    std::vector<daw::ZipEntry> out;
    check(daw::zipRead(bytes, out), "an archive with no entries is still a valid archive");
    check(out.empty(), "and reads as empty");
  }

  // ---- AN EMPTY ENTRY, which a zero-byte sample would produce.
  {
    std::vector<daw::ZipEntry> in{{"empty.wav", {}}};
    const auto bytes = daw::zipBuild(in);
    std::vector<daw::ZipEntry> out;
    check(daw::zipRead(bytes, out), "a zero-length entry round-trips");
    check(out.size() == 1 && out[0].data.empty(), "and stays zero-length");
  }

  // ---- IDENTICAL CONTENT PRODUCES AN IDENTICAL FILE.
  //
  // The timestamp is FIXED at the format's epoch rather than taken from the wall clock. Without
  // that, saving twice with no edits shows as a change in git and "did anything actually change?"
  // stops being answerable — for a format whose whole purpose is to be sent to people, that is
  // a real loss.
  {
    std::vector<daw::ZipEntry> in{{"project.json", bytesOf("same")},
                                  {"samples/a.wav", bytesOf("audio")}};
    const auto a = daw::zipBuild(in);
    const auto b = daw::zipBuild(in);
    check(a == b, "two builds of identical content are BYTE-IDENTICAL — a wall-clock timestamp "
                  "would make every save a diff");
  }

  // ---- A CORRUPT ENTRY IS REFUSED, not returned. This is the property that matters most: a
  // module that opens with three of its four samples is worse than one that refuses, because the
  // missing one is only discovered when that pad is played.
  {
    std::vector<daw::ZipEntry> in{{"samples/a.wav", bytesOf("original content here")}};
    auto bytes = daw::zipBuild(in);
    // Flip a byte in the stored payload. The CRC in the header no longer matches.
    bool flipped = false;
    for (size_t i = 30; i + 4 < bytes.size(); ++i) {
      if (bytes[i] == 'o' && bytes[i + 1] == 'r' && bytes[i + 2] == 'i') {
        bytes[i + 1] = 'X';
        flipped = true;
        break;
      }
    }
    check(flipped, "the fixture found the payload to corrupt");
    std::vector<daw::ZipEntry> out;
    std::string err;
    check(!daw::zipRead(bytes, out, &err),
          "a corrupt entry is REFUSED. Returning it turns 'my song sounds wrong' into a mystery; "
          "refusing turns it into a message");
    check(err.find("CRC") != std::string::npos,
          "and the reason NAMES the CRC rather than saying 'bad zip' — a message that identifies "
          "the failure is the difference between a fix and an investigation");
  }

  // ---- TRUNCATION IS REFUSED. Half a download is the common way a module arrives broken.
  {
    std::vector<daw::ZipEntry> in{{"samples/a.wav", bytesOf("some audio bytes")}};
    auto bytes = daw::zipBuild(in);
    bytes.resize(bytes.size() / 2);
    std::vector<daw::ZipEntry> out;
    std::string err;
    check(!daw::zipRead(bytes, out, &err), "a truncated archive is refused");
  }
  {
    std::vector<daw::ZipEntry> out;
    std::string err;
    check(!daw::zipRead({}, out, &err), "an empty file is not an archive");
    check(!daw::zipRead(bytesOf("not a zip at all, just text"), out, &err),
          "arbitrary bytes are not an archive");
  }

  // ---- A COMPRESSED ENTRY IS REFUSED RATHER THAN SKIPPED. Deflate is legal zip and this
  // container does not do it; silently dropping such an entry would produce a module missing a
  // sample, which is the failure mode this whole file is organised against.
  {
    std::vector<daw::ZipEntry> in{{"a.wav", bytesOf("payload")}};
    auto bytes = daw::zipBuild(in);
    // Set the method to 8 (deflate) in both the local header and the central directory.
    for (size_t i = 0; i + 4 < bytes.size(); ++i) {
      const uint32_t sig = static_cast<uint32_t>(bytes[i]) |
                           (static_cast<uint32_t>(bytes[i + 1]) << 8) |
                           (static_cast<uint32_t>(bytes[i + 2]) << 16) |
                           (static_cast<uint32_t>(bytes[i + 3]) << 24);
      if (sig == 0x02014b50) {
        bytes[i + 10] = 8;
      }
    }
    std::vector<daw::ZipEntry> out;
    std::string err;
    check(!daw::zipRead(bytes, out, &err), "a deflated entry is REFUSED, not skipped");
    check(err.find("stored-only") != std::string::npos,
          "and the message says WHY, so the fix is obvious rather than guessed at");
  }

  // ---- CRC-32 IS THE REAL ONE. A container whose CRCs are wrong opens here and fails in every
  // other tool — the worst kind of "works on my machine", and one that only shows up after you
  // have sent someone the file.
  {
    const std::string s = "123456789";
    const uint32_t crc =
        daw::zipCrc32(reinterpret_cast<const uint8_t*>(s.data()), s.size());
    check(crc == 0xCBF43926u,
          "CRC-32 of the standard check string is 0xCBF43926 — if this is wrong the archives are "
          "unreadable by unzip, and nothing here would notice");
  }

  // ---- A REAL-SIZED PAYLOAD. A kit is megabytes, and an off-by-one in the offsets shows up as
  // size grows rather than on a three-byte fixture.
  {
    std::vector<uint8_t> big(300000);
    for (size_t i = 0; i < big.size(); ++i) {
      big[i] = static_cast<uint8_t>((i * 7 + (i >> 8)) & 0xFF);
    }
    std::vector<daw::ZipEntry> in{{"project.json", bytesOf("{}")},
                                  {"samples/big.wav", big},
                                  {"samples/after.wav", bytesOf("follows the big one")}};
    const auto bytes = daw::zipBuild(in);
    std::vector<daw::ZipEntry> out;
    std::string err;
    check(daw::zipRead(bytes, out, &err), "a 300 KB payload round-trips");
    check(out.size() == 3 && out[1].data == big, "byte for byte");
    check(out.size() == 3 && stringOf(out[2].data) == "follows the big one",
          "and the entry AFTER it is still found — offsets past 64 KB are where a 16-bit "
          "mistake would surface");
  }

  if (g_fail == 0) {
    std::printf("zip_container_tests: PASS\n");
  }
  return g_fail == 0 ? 0 : 1;
}
