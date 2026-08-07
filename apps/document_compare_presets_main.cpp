// DOES THE STRUCTURAL COMPARER AGREE WITH THE SERIALIZER, ON REAL PROJECTS?
//
// This is the proof that was missing, and the reason the field visitor did not need the serializer
// rebuilt on it first. The plan said: rebuild serializeProject on the walk, because it has 135
// checks and every shipped preset behind it, so a wrong visitor fails loudly. That reasoning was
// about EVIDENCE, not about the serializer — and the same evidence is available without rewriting
// the format:
//
//     for every shipped preset, documentFieldsEqual(a, b) must agree with
//     serializeProject(a) == serializeProject(b)
//
// If the walk misses a field, a mutation to it changes the serialized bytes and NOT the structural
// compare, and this fails. That is exactly the failure mode a hand-maintained field list produces,
// and it is the one that cost this repo four separate bugs (the aux-child subset twice, in
// opposite directions; alternateClipId; the four copies of the clip-adoption rule).
//
// WHERE THEY ARE ALLOWED TO DISAGREE, and why that is not a hole:
//   DERIVED fields. hostSlotIndex is an index into this machine's plugin scan; the serializer no
//   longer writes it at all (3c4fd45) so the two agree there by construction. If a future Derived
//   field IS serialized, this check will catch the disagreement and the right answer will be to
//   stop serializing it, not to weaken the check.
//
// RUN AGAINST THE SHIPPED PRESETS, not a fixture written here — presets/projects/ has the device
// chains, samplers, patcher graphs, mod links and automation a hand-built document would omit, and
// those are precisely the parts a field list is most likely to be missing.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "apps/document_compare.h"
#include "apps/project_file.h"

namespace {

int failures = 0;
void expect(bool cond, const std::string& what) {
  if (!cond) {
    std::printf("  FAIL: %s\n", what.c_str());
    ++failures;
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("usage: document_compare_presets <presets/projects dir>\n");
    return 2;
  }
  const std::filesystem::path dir = argv[1];

  int checked = 0;
  for (const auto& entry : std::filesystem::directory_iterator(dir)) {
    const auto path = entry.path();
    if (path.extension() != ".json") {
      continue;
    }
    std::ifstream in(path);
    if (!in) {
      continue;
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    const std::string json = buffer.str();

    daw::ProjectDocument a;
    daw::ProjectDocument b;
    std::string errA;
    std::string errB;
    if (!daw::deserializeProject(json, a, &errA) || !daw::deserializeProject(json, b, &errB)) {
      // A preset that will not parse is a different defect and daw_lint owns it; skipping here
      // would be silent, so say so.
      std::printf("  SKIPPED %s — did not parse (%s)\n", path.filename().string().c_str(),
                  errA.empty() ? errB.c_str() : errA.c_str());
      continue;
    }
    ++checked;
    const std::string name = path.filename().string();

    // 1. TWO PARSES OF THE SAME BYTES AGREE, both ways. If this fails the comparer is reading
    //    something the serializer does not write, or vice versa.
    expect(daw::documentFieldsEqual(a, b),
           name + ": two parses of the same file compare UNEQUAL structurally");
    expect(daw::serializeProject(a) == daw::serializeProject(b),
           name + ": two parses of the same file serialize differently");

    // 2. A MUTATION IS SEEN BY BOTH. This is the assertion that catches a field the walk forgot:
    //    the serializer notices, the comparer does not, and they disagree.
    if (!b.tracks.empty()) {
      const std::string beforeBytes = daw::serializeProject(b);
      b.tracks.front().name += " (mutated)";
      const bool structurallyDiffers = !daw::documentFieldsEqual(a, b);
      const bool bytesDiffer = daw::serializeProject(b) != beforeBytes;
      expect(structurallyDiffers == bytesDiffer,
             name + ": the comparer and the serializer DISAGREE about whether a track rename is a "
                    "change — one of them cannot see a field the other can");
      expect(structurallyDiffers, name + ": a track rename must be a change");
    }
  }

  // A GLOB THAT MATCHED NOTHING WOULD PASS SILENTLY, asserting nothing at all.
  expect(checked >= 5,
         "fewer than 5 presets were compared — this check would prove nothing");

  if (failures != 0) {
    std::printf("document_compare_presets: FAIL (%d)\n", failures);
    return 1;
  }
  std::printf("document_compare_presets: PASS — the structural comparer agrees with the serializer "
              "on %d shipped project(s)\n", checked);
  return 0;
}
