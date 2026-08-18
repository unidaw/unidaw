#pragma once

#include <cstdint>
#include <string>
#include <vector>

// SHA-256, because the artifact inventory is built on it and nothing in this tree had one.
//
// AE-P1.2 G2-B item 18, R-DEVICE-ID-LIFETIME: a schema-6 document references an immutable
// `artifact_generation` plus a sorted per-entry digest inventory, and "artifact_generation is
// lowercase SHA-256 of canonical sorted artifact_entries whose per-entry SHA-256 commits the
// bytes". A digest that is subtly wrong would not fail loudly — it would agree with itself on
// every write and read, and only disagree with the rest of the world.
//
// WHY NOT JUCE'S. `juce::SHA256` exists, and JUCE is confined to platform_juce/ by a coding
// constraint this repo states outright. The document layer (apps/project_file.cpp) is where the
// inventory is built and verified, and it must not reach into the audio platform to hash a file.
//
// WHY NOT A ONE-LINE SHELL-OUT. The digest is computed during save, inside the commit ordering
// that decides whether a document reference is replaced. Spawning a process there makes the
// atomicity argument depend on process scheduling.
//
// CORRECTNESS IS ASSERTED AGAINST PUBLISHED VECTORS, not against itself. apps/sha256_tests_main.cpp
// runs the NIST/FIPS-180-4 examples plus the standard million-'a' case: an implementation checked
// only by round-tripping its own output is one that cannot detect being wrong.

namespace daw {

// Lowercase hex, 64 characters.
std::string sha256Hex(const void* data, size_t size);

inline std::string sha256Hex(const std::string& text) {
  return sha256Hex(text.data(), text.size());
}

inline std::string sha256Hex(const std::vector<uint8_t>& bytes) {
  return sha256Hex(bytes.data(), bytes.size());
}

}  // namespace daw
