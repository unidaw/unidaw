#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "apps/artifact_inventory.h"
#include "apps/ipc_protocol.h"

// COMMITTING A GENERATION, in the order the contract fixes.
//
// AE-P1.2 G2-B item 18, R-DEVICE-ID-LIFETIME `save_commit_order`:
//
//   "Build the complete inventory, write and verify a fresh temporary generation directory,
//    atomically rename it to the immutable generation path (or byte-verify an existing identical
//    generation), then atomically replace the ProjectDocument carrying that generation and exact
//    entries. Failure before the document rename leaves the prior document/generation
//    authoritative; an unreferenced generation is harmless."
//
// THE ORDER IS THE WHOLE THING, and the old code had it backwards: it wrote project.json first and
// then the blobs, best-effort, so an interruption between them left a document referring to state
// that was never written — and nothing could tell that from a project which genuinely had none.
//
// Content-addressing is what makes the recovery trivial. A generation's name IS the digest of its
// contents, so a half-written one can never be mistaken for a complete one, two saves of identical
// state converge on the same directory, and anything unreferenced is garbage rather than
// corruption. Nothing has to be cleaned up for correctness.

namespace daw::engine {

// One file to place in the generation being built.
struct ArtifactToCommit {
  daw::ArtifactEntry entry;       // trackId, deviceId, kind, leaf, size and digest of `bytes`
  std::vector<uint8_t> bytes;
};

// THE PARAMETER MANIFEST, AS TEXT — one renderer, because the bytes are now DIGESTED.
//
// It used to be written straight to an ofstream inside the save loop. That was fine while the file
// was the only artifact of it; the inventory commits each side by its SHA-256, so the bytes have
// to exist before they are written, and a second renderer producing "the same" JSON with different
// whitespace would produce a different digest for identical parameters.
//
// `track` and `device` are embedded in the document so a reader can tell whose parameters these
// are without the filename — and so the loader can refuse a manifest whose embedded key does not
// match the entry that pointed at it.
std::string renderParameterManifest(const std::string& pluginName,
                                    uint32_t trackId,
                                    uint32_t deviceId,
                                    const std::vector<daw::HostParamWire>& params);

// THE MANIFEST CARRIES ITS OWN {track, device}, AND SOMEBODY HAS TO KEEP IT TRUE.
//
// `renderParameterManifest` embeds the pair so a reader can tell whose parameters a file holds
// without its name. That is only useful while the embedded pair still matches the entry pointing
// at it, and there are two ways it stops matching — both named by the contract:
//
//   `present_file_rules`: "malformed manifest JSON, or manifest whose embedded track/device
//    differs from the expected source key (LegacyArtifactKey for schema 1-5, indexed global key
//    for schema 6) fails load"    <- the parenthetical is NOT optional: it is what says the
//                                    legacy side must compare too, and an earlier version of this
//                                    file elided it and then omitted that comparison.
//   `legacy_import`:      "rewrite manifest embedded ids in memory"
//   `artifact_presence_matrix` row 3 retained_for_save:
//                         "explicit_absent_blob_and_canonicalized_manifest_entries"
//
// So a schema-6 load REFUSES a mismatch, a legacy import REWRITES it, and a save that republishes
// retained bytes CANONICALIZES them to the pair they are being written under. Without that last
// one a device that moves tracks poisons itself: its retained manifest still names the old track,
// the save writes it under the new leaf, and the next load refuses the project it just wrote.
//
// STRUCTURAL, NOT TEXTUAL. These match the exact shape renderParameterManifest emits — the
// closing quote of the plugin name, then `,\n  "track": <digits>,\n  "device": <digits>,\n
// "params": [` — and refuse anything else as malformed. Matching a looser pattern would make a
// hand-edited or foreign file silently rewritable, and "it looked close enough" is the whole
// failure this record removes.
bool manifestEmbeddedKey(const std::vector<uint8_t>& bytes, uint32_t& trackId, uint32_t& deviceId);
bool rewriteManifestEmbeddedKey(std::vector<uint8_t>& bytes, uint32_t trackId, uint32_t deviceId);

// WRITE AND VERIFY THE GENERATION, returning false with `error` set on any failure.
//
// `entries` must already be sealed — sorted, with the generation computed from them — because the
// directory is NAMED by that generation and a caller that had not sealed would write to a path
// describing different contents.
//
// An existing generation directory is BYTE-VERIFIED rather than overwritten: the name is a digest,
// so a directory that is already there should already hold exactly these bytes, and if it does not
// then something is wrong that silently rewriting would hide.
bool commitArtifactGeneration(const std::string& stateDir,
                              const std::string& generation,
                              const std::vector<ArtifactToCommit>& files,
                              std::string* error);

}  // namespace daw::engine
