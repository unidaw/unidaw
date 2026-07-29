// M2.20: the deterministic linter, over the document and over history.jsonl.
//
// Everything it reports is something the engine currently TOLERATES: it loads the
// project, plays it, and the problem shows up later as a control that does nothing, a
// device that never loads, a note that never sounds, or an edit that is always refused.
// Those are the failures that cost the most, because nothing anywhere says a word.
//
// DETERMINISTIC means: same input, byte-identical output. Findings are sorted by
// (code, scope, detail) before printing, nothing carries a timestamp or a path outside
// the input, and no check depends on iteration order of an unordered container. So the
// output can be diffed between two versions of a project and the diff is the change.
//
// It reuses apps/project_file.h to parse, so the linter and the engine cannot disagree
// about what a project means — a linter with its own parser lints a different document.
//
//   daw_lint <project.uniproj.json> [--history <history.jsonl>] [--strict] [--quiet]
//                                   [--max-per-rule N] [--allow CODE[ SCOPE]]
//                                   [--ignore-file PATH]
//
// STAYING USABLE is a feature, not a courtesy. A linter that prints sixty thousand
// findings, or that cannot be told "yes, on purpose", gets muted within a week — and a
// muted linter is worse than none, because its silence reads as approval. Two
// mechanisms, both of which REPORT what they hid:
//
//   --max-per-rule N (default 10)  after N findings of one code, the rest become a
//                                  count. The pile is the point in a stress fixture;
//                                  seeing it 60,811 times is not.
//   .dawlint                       a file beside the project listing findings declared
//                                  intentional, one per line, "<code>" or
//                                  "<code> <scope>", # for comments. --allow adds one
//                                  from the command line; --ignore-file points at
//                                  another.
//
// Neither is silent. The summary always states how many findings were capped and how
// many were suppressed, so a suppression can be audited rather than forgotten.
//
// Exit 0 = no errors (warnings alone still exit 0, unless --strict), 1 = errors found,
// 2 = the input could not be read or parsed.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "apps/patcher_assemble.h"
#include "apps/patcher_graph.h"
#include "apps/project_file.h"

namespace {

enum class Severity { Error, Warning };

struct Finding {
  Severity severity = Severity::Warning;
  std::string code;    // stable, kebab-case; the thing a script matches on
  std::string scope;   // "track:3", "clip:2", "global", "history:seq 41"
  std::string detail;  // one line, states the consequence, not just the fact

  bool operator<(const Finding& other) const {
    if (code != other.code) return code < other.code;
    if (scope != other.scope) return scope < other.scope;
    return detail < other.detail;
  }
};

std::vector<Finding> g_findings;

// A finding declared intentional. An empty scope matches every scope for that code.
struct Allowance {
  std::string code;
  std::string scope;
};
std::vector<Allowance> g_allowed;

bool isAllowed(const Finding& finding) {
  for (const auto& a : g_allowed) {
    if (a.code == finding.code && (a.scope.empty() || a.scope == finding.scope)) {
      return true;
    }
  }
  return false;
}

// Reads a .dawlint file: one declaration per line, "<code>" or "<code> <scope>",
// blank lines and # comments ignored. Returns false only if the path was given and
// could not be opened — a MISSING default .dawlint is normal and not an error.
bool loadAllowFile(const std::string& path, bool required) {
  std::ifstream in(path);
  if (!in) {
    return !required;
  }
  std::string line;
  while (std::getline(in, line)) {
    const auto hash = line.find('#');
    if (hash != std::string::npos) {
      line = line.substr(0, hash);
    }
    std::istringstream fields(line);
    Allowance a;
    if (!(fields >> a.code)) {
      continue;
    }
    fields >> a.scope;  // optional
    g_allowed.push_back(std::move(a));
  }
  return true;
}

void report(Severity severity, std::string code, std::string scope, std::string detail) {
  g_findings.push_back({severity, std::move(code), std::move(scope), std::move(detail)});
}

std::string trackScope(const daw::ProjectTrack& track) {
  if (track.isMaster) {
    return "track:master";
  }
  return "track:" + std::to_string(track.trackId);
}

// ---------------------------------------------------------------------------
// Document checks.

void checkClipsAndPlacements(const daw::ProjectDocument& doc) {
  std::set<uint32_t> clipIds;
  std::map<uint32_t, uint64_t> clipLength;
  for (const auto& clip : doc.clips) {
    if (!clipIds.insert(clip.id).second) {
      report(Severity::Error, "clip-id-duplicate", "clip:" + std::to_string(clip.id),
             "two clips share this id; a placement referring to it resolves to "
             "whichever the loader saw last");
    }
    clipLength[clip.id] = clip.lengthNanoticks;
  }

  std::set<uint32_t> referenced;
  std::map<uint32_t, std::string> placementOwner;  // placement id -> first scope
  for (const auto& track : doc.tracks) {
    const std::string scope = trackScope(track);
    for (const auto& placement : track.placements) {
      referenced.insert(placement.clipId);
      if (clipIds.find(placement.clipId) == clipIds.end()) {
        report(Severity::Error, "clip-missing", scope,
               "placement references clip " + std::to_string(placement.clipId) +
                   ", which no clip defines; it will never sound");
      }
      if (placement.id != 0) {
        auto it = placementOwner.find(placement.id);
        if (it != placementOwner.end()) {
          report(Severity::Error, "placement-id-duplicate",
                 "placement:" + std::to_string(placement.id),
                 "shared by " + it->second + " and " + scope +
                     "; Move/Resize/Remove key on this id and will hit the wrong one");
        } else {
          placementOwner[placement.id] = scope;
        }
      }
    }
  }
  for (uint32_t id : clipIds) {
    if (referenced.find(id) == referenced.end()) {
      report(Severity::Warning, "clip-unplaced", "clip:" + std::to_string(id),
             "no placement references this clip; it is carried in the file and never "
             "heard (intentional for a session/loose clip)");
    }
  }

  // Notes that cannot sound, and notes that cut each other off. A column is the
  // monophonic unit, so two overlapping notes in one column is not a chord — the
  // second silences the first, which is almost never what was meant.
  for (const auto& clip : doc.clips) {
    if (clip.kind != daw::ClipKind::Symbolic) {
      continue;
    }
    const std::string scope = "clip:" + std::to_string(clip.id);
    std::set<uint32_t> noteIds;
    struct Span {
      uint64_t start = 0;
      uint64_t end = 0;
      uint8_t column = 0;
    };
    std::vector<Span> spans;
    for (const auto& event : clip.clip.events()) {
      if (event.type != daw::MusicalEventType::Note) {
        continue;
      }
      const auto& note = event.payload.note;
      if (note.noteId != 0 && !noteIds.insert(note.noteId).second) {
        report(Severity::Error, "note-id-duplicate", scope,
               "note id " + std::to_string(note.noteId) +
                   " appears twice; edits and undo address notes by id and will hit "
                   "the wrong one");
      }
      if (clip.lengthNanoticks > 0 && event.nanotickOffset >= clip.lengthNanoticks) {
        report(Severity::Warning, "note-past-clip-end", scope,
               "a note starts at " + std::to_string(event.nanotickOffset) +
                   ", at or past the clip's length " +
                   std::to_string(clip.lengthNanoticks) + "; it never sounds");
      }
      if (note.durationNanoticks == 0) {
        report(Severity::Warning, "note-zero-duration", scope,
               "a note at " + std::to_string(event.nanotickOffset) +
                   " has duration 0; the engine skips it, so it is invisible silence");
      }
      spans.push_back({event.nanotickOffset,
                       event.nanotickOffset + note.durationNanoticks, note.column});
    }
    std::sort(spans.begin(), spans.end(), [](const Span& a, const Span& b) {
      if (a.column != b.column) return a.column < b.column;
      if (a.start != b.start) return a.start < b.start;
      return a.end < b.end;
    });
    for (size_t i = 1; i < spans.size(); ++i) {
      if (spans[i].column != spans[i - 1].column) {
        continue;
      }
      if (spans[i].start == spans[i - 1].start) {
        // Same tick, same column. Distinct from an overlap: the tracker projects a
        // column onto rows, so two notes at one tick are two notes in ONE CELL and one
        // of them is simply not drawn. That is review item 11's projection collision.
        report(Severity::Warning, "note-same-tick-in-column", scope,
               "column " + std::to_string(spans[i].column) + ": two notes both start at " +
                   std::to_string(spans[i].start) +
                   "; the tracker draws one cell per (row, column), so one of them is "
                   "invisible");
      } else if (spans[i].start < spans[i - 1].end) {
        report(Severity::Warning, "note-overlap-in-column", scope,
               "column " + std::to_string(spans[i].column) + ": a note at " +
                   std::to_string(spans[i].start) + " starts before the one at " +
                   std::to_string(spans[i - 1].start) + " ends; a column is monophonic, "
                   "so the earlier note is cut off");
      }
    }
  }
}

void checkTracks(const daw::ProjectDocument& doc) {
  std::set<uint32_t> trackIds;
  uint32_t masterCount = 0;
  for (const auto& track : doc.tracks) {
    if (track.isMaster) {
      ++masterCount;
      continue;
    }
    if (!trackIds.insert(track.trackId).second) {
      report(Severity::Error, "track-id-duplicate", trackScope(track),
             "two tracks share this id; every per-track command addresses one of them "
             "and the other is unreachable");
    }
  }
  if (masterCount > 1) {
    report(Severity::Error, "master-duplicate", "global",
           std::to_string(masterCount) +
               " tracks are marked is_master; the engine lifts out exactly one and the "
               "rest become ordinary tracks with no arrangement");
  }

  for (const auto& track : doc.tracks) {
    const std::string scope = trackScope(track);
    if (track.parentId != 0 && trackIds.find(track.parentId) == trackIds.end()) {
      report(Severity::Error, "parent-missing", scope,
             "parent track " + std::to_string(track.parentId) +
                 " does not exist; this child has nothing to be fed from");
    }
    auto checkRoute = [&](const daw::TrackRoute& route, const char* which) {
      if (route.kind == daw::TrackRouteKind::Track &&
          trackIds.find(route.trackId) == trackIds.end()) {
        report(Severity::Error, "routing-track-missing", scope,
               std::string(which) + " routes to track " + std::to_string(route.trackId) +
                   ", which does not exist; that signal goes nowhere");
      }
    };
    checkRoute(track.routing.midiOut, "midi_out");
    checkRoute(track.routing.audioOut, "audio_out");
    checkRoute(track.routing.midiIn, "midi_in");
    checkRoute(track.routing.audioIn, "audio_in");
    checkRoute(track.routing.sidechain, "sidechain");

    // A control that looks set and does nothing. Exactly the class of defect the
    // frontend caught in the euclidean node: the read-back is honest, the sound never
    // moves, and the user spends their time doubting their ears.
    if (track.quantize.gridNanoticks == 0 && track.quantize.strengthMilli > 0) {
      report(Severity::Warning, "quantize-inert", scope,
             "quantize strength is " + std::to_string(track.quantize.strengthMilli) +
                 " but the grid is 0, so nothing is quantized; the strength control "
                 "reads as set and does nothing");
    }
    if (track.quantize.gridNanoticks > 0 && track.quantize.strengthMilli == 0) {
      report(Severity::Warning, "quantize-inert", scope,
             "a quantize grid is set but strength is 0, so nothing is quantized");
    }
  }
}

void checkChains(const daw::ProjectDocument& doc) {
  for (const auto& track : doc.tracks) {
    const std::string scope = trackScope(track);
    std::set<uint32_t> deviceIds;
    std::map<uint32_t, size_t> devicePos;
    for (size_t i = 0; i < track.chain.devices.size(); ++i) {
      const auto& device = track.chain.devices[i];
      if (!deviceIds.insert(device.id).second) {
        report(Severity::Error, "device-id-duplicate", scope,
               "device id " + std::to_string(device.id) +
                   " appears twice in this chain; chain edits and mod links address "
                   "devices by id and will hit the wrong one");
      } else {
        devicePos[device.id] = i;
      }

      const bool isPlugin = device.kind == daw::DeviceKind::VstInstrument ||
                            device.kind == daw::DeviceKind::VstEffect;
      if (isPlugin) {
        const bool hasIdentity =
            !device.vstRef.name.empty() || !device.vstRef.uid16.empty();
        if (device.vstRef.path.empty() && !hasIdentity) {
          report(Severity::Error, "device-plugin-unresolvable", scope,
                 "device " + std::to_string(device.id) +
                     " is a plugin with no vst_ref at all; it can only be resolved by "
                     "host_slot_index, which names a DIFFERENT plugin as soon as "
                     "anything is installed or removed — this is how a saved Zebra2 "
                     "came back as ZEBRIFY");
        } else if (device.vstRef.path.empty()) {
          // A name or uid without a path is how a PORTABLE fixture is written (the
          // Identity build product lives at a different path in every build dir), so
          // this is a warning rather than an error — but the resolution is still by
          // name against whatever the scan found, which is weaker than a path.
          report(Severity::Warning, "device-plugin-by-name-only", scope,
                 "device " + std::to_string(device.id) + " (" + device.vstRef.name +
                     ") has no vst_ref path, so it resolves by name against the current "
                     "scan; portable, but it will silently pick a different build or "
                     "version");
        } else if (!std::filesystem::exists(device.vstRef.path)) {
          report(Severity::Error, "device-plugin-missing", scope,
                 "device " + std::to_string(device.id) + " (" + device.vstRef.name +
                     ") points at " + device.vstRef.path +
                     ", which does not exist; it will load as silence");
        }
      } else if (!device.vstRef.path.empty()) {
        report(Severity::Warning, "device-vstref-on-nonplugin", scope,
               "device " + std::to_string(device.id) +
                   " is not a plugin but carries a vst_ref path; that is stale data "
                   "that a loader may act on");
      }

      // A device's patcher node id must name a node in ITS OWN graph, or be the
      // natural-output sentinel. A dangling id is why a patcher device used to run
      // silent: the per-device node filter could not seed from a node that was not there.
      if (!device.patcher.nodes.empty()) {
        std::set<uint32_t> nodeIds;
        for (const auto& node : device.patcher.nodes) {
          if (!nodeIds.insert(node.id).second) {
            report(Severity::Error, "patcher-node-id-duplicate", scope,
                   "device " + std::to_string(device.id) + ": node id " +
                       std::to_string(node.id) + " appears twice");
          }
        }
        if (device.patcherNodeId != 0xFFFFFFFFu &&
            nodeIds.find(device.patcherNodeId) == nodeIds.end()) {
          report(Severity::Error, "patcher-node-missing", scope,
                 "device " + std::to_string(device.id) + " reads its output from node " +
                     std::to_string(device.patcherNodeId) +
                     ", which its graph does not contain; the device runs silent");
        }
        for (const auto& edge : device.patcher.edges) {
          if (nodeIds.find(edge.src.nodeId) == nodeIds.end()) {
            report(Severity::Error, "patcher-edge-dangling", scope,
                   "device " + std::to_string(device.id) + ": an edge starts at node " +
                       std::to_string(edge.src.nodeId) + ", which does not exist");
          }
          if (nodeIds.find(edge.dst.nodeId) == nodeIds.end()) {
            report(Severity::Error, "patcher-edge-dangling", scope,
                   "device " + std::to_string(device.id) + ": an edge ends at node " +
                       std::to_string(edge.dst.nodeId) + ", which does not exist");
          }
        }
      } else if (device.patcherNodeId != 0xFFFFFFFFu && device.patcherNodeId != 0 &&
                 (device.kind == daw::DeviceKind::PatcherEvent ||
                  device.kind == daw::DeviceKind::PatcherAudio)) {
        report(Severity::Warning, "patcher-empty", scope,
               "device " + std::to_string(device.id) +
                   " is a patcher with no nodes but names an output node; it produces "
                   "nothing");
      }
    }

    std::set<uint32_t> linkIds;
    for (const auto& link : track.modLinks) {
      if (link.linkId != 0 && !linkIds.insert(link.linkId).second) {
        report(Severity::Error, "modlink-id-duplicate", scope,
               "mod link id " + std::to_string(link.linkId) + " appears twice");
      }
      auto srcIt = devicePos.find(link.source.deviceId);
      auto dstIt = devicePos.find(link.target.deviceId);
      if (srcIt == devicePos.end()) {
        report(Severity::Error, "modlink-device-missing", scope,
               "mod link " + std::to_string(link.linkId) + " sources from device " +
                   std::to_string(link.source.deviceId) +
                   ", which is not in this chain; the engine refuses the link");
      }
      if (dstIt == devicePos.end()) {
        report(Severity::Error, "modlink-device-missing", scope,
               "mod link " + std::to_string(link.linkId) + " targets device " +
                   std::to_string(link.target.deviceId) +
                   ", which is not in this chain; the engine refuses the link");
      }
      // Strictly backwards only. A device modulating ITSELF is legal and common with
      // per-device patchers; see the matching rule in the engine.
      if (srcIt != devicePos.end() && dstIt != devicePos.end() &&
          srcIt->second > dstIt->second) {
        report(Severity::Error, "modlink-order", scope,
               "mod link " + std::to_string(link.linkId) + " sources from device " +
                   std::to_string(link.source.deviceId) + " at position " +
                   std::to_string(srcIt->second) + " and targets device " +
                   std::to_string(link.target.deviceId) + " at position " +
                   std::to_string(dstIt->second) +
                   "; modulation only flows forward, so the engine refuses it");
      }
    }
  }
}

// Does this track's set of patcher devices actually ASSEMBLE and BUILD? Uses the very
// functions the engine uses, so the linter and the engine cannot disagree about whether
// a graph is valid — a reimplementation here would be a second opinion, which is worth
// nothing when the question is "will the engine run this".
//
// The failure this catches is severe and was silent: one device with an invalid edge
// (an LFO wired into an event input, say) fails the whole TRACK's assembly, so NONE of
// that track's patchers execute. The engine now says so at load; this says so without
// running it, and names the track.
void checkPatcherAssembly(const daw::ProjectDocument& doc) {
  for (const auto& track : doc.tracks) {
    bool anyGraph = false;
    for (const auto& device : track.chain.devices) {
      if (!device.patcher.nodes.empty()) {
        anyGraph = true;
        break;
      }
    }
    if (!anyGraph) {
      continue;
    }
    daw::AssembledPatcher sub = daw::assemblePatcherPool(track.chain.devices);
    if (!sub.anyPerDevice || sub.pool.nodes.empty()) {
      continue;
    }
    daw::PatcherGraph pool = sub.pool;
    if (!daw::buildPatcherGraph(pool)) {
      report(Severity::Error, "patcher-assembly-fails", trackScope(track),
             "this track's patcher devices do not assemble into a runnable graph, so "
             "NONE of them execute — one device's edges are invalid (a common cause is "
             "an LFO or other CV source wired into an EVENT input)");
    }
  }
}

void checkGlobals(const daw::ProjectDocument& doc) {
  if (doc.tempoMap.empty()) {
    report(Severity::Error, "tempo-map-empty", "global",
           "no tempo points; the engine falls back to a default and the file does not "
           "say what tempo the music is at");
  } else {
    if (doc.tempoMap.front().nanotick != 0) {
      report(Severity::Error, "tempo-map-no-origin", "global",
             "the first tempo point is at " +
                 std::to_string(doc.tempoMap.front().nanotick) +
                 ", not 0; everything before it has no defined tempo");
    }
    for (size_t i = 1; i < doc.tempoMap.size(); ++i) {
      if (doc.tempoMap[i].nanotick <= doc.tempoMap[i - 1].nanotick) {
        report(Severity::Error, "tempo-map-unsorted", "global",
               "tempo point " + std::to_string(i) + " is at " +
                   std::to_string(doc.tempoMap[i].nanotick) +
                   ", not after the previous one");
      }
    }
    for (size_t i = 0; i < doc.tempoMap.size(); ++i) {
      if (!(doc.tempoMap[i].bpm > 0.0)) {
        report(Severity::Error, "tempo-invalid", "global",
               "tempo point " + std::to_string(i) + " has bpm " +
                   std::to_string(doc.tempoMap[i].bpm));
      }
    }
  }
  if (doc.nanoticksPerQuarter == 0) {
    report(Severity::Error, "nanoticks-per-quarter-zero", "global",
           "every tick conversion divides by this");
  }
}

// ---------------------------------------------------------------------------
// history.jsonl checks. The journal is append-only JSON lines written by the engine;
// this reads it with string scanning rather than a JSON parser because the format is
// one flat object per line and a dependency-free linter is easier to run anywhere.

std::string jsonField(const std::string& line, const std::string& key) {
  const std::string needle = "\"" + key + "\":";
  const auto pos = line.find(needle);
  if (pos == std::string::npos) {
    return {};
  }
  auto i = pos + needle.size();
  while (i < line.size() && (line[i] == ' ')) ++i;
  if (i < line.size() && line[i] == '"') {
    const auto end = line.find('"', i + 1);
    if (end == std::string::npos) return {};
    return line.substr(i + 1, end - i - 1);
  }
  const auto end = line.find_first_of(",}", i);
  return line.substr(i, end == std::string::npos ? std::string::npos : end - i);
}

void checkHistory(const std::string& path) {
  std::ifstream in(path);
  if (!in) {
    report(Severity::Warning, "history-unreadable", "global",
           "could not read " + path);
    return;
  }
  std::string line;
  // A run of version rejections on ONE scope means a caller is presenting a base it
  // never refreshes — it is not losing a race, it is stuck, and every edit it makes is
  // being dropped on the floor. That is the exact failure per-track versions (M2.17)
  // and the multi-producer ring (M2.18) were meant to end, so a storm after those is
  // a caller that has not adopted them.
  std::map<std::string, uint32_t> consecutiveRejects;
  std::map<std::string, uint32_t> worstRun;
  std::map<std::string, uint32_t> opCounts;
  uint64_t lastSeq = 0;
  bool seqSeen = false;
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    const std::string scope = jsonField(line, "scope");
    const std::string outcome = jsonField(line, "outcome");
    const std::string op = jsonField(line, "op");
    const std::string seqStr = jsonField(line, "seq");
    if (!seqStr.empty()) {
      const uint64_t seq = std::strtoull(seqStr.c_str(), nullptr, 10);
      if (seqSeen && seq != lastSeq + 1) {
        report(Severity::Warning, "history-seq-gap", "history",
               "seq jumps from " + std::to_string(lastSeq) + " to " +
                   std::to_string(seq) + "; the journal is missing entries, so it is "
                   "not a complete record of what happened");
      }
      lastSeq = seq;
      seqSeen = true;
    }
    if (!op.empty()) {
      ++opCounts[op];
    }
    if (outcome.rfind("rejected", 0) == 0) {
      const uint32_t run = ++consecutiveRejects[scope];
      worstRun[scope] = std::max(worstRun[scope], run);
    } else if (!outcome.empty()) {
      consecutiveRejects[scope] = 0;
    }
  }
  for (const auto& [scope, run] : worstRun) {
    if (run >= 5) {
      report(Severity::Warning, "history-rejection-storm", "history",
             std::to_string(run) + " consecutive rejections on " + scope +
                 "; a caller there is presenting a base it never refreshes, so its "
                 "edits are being dropped");
    }
  }
  if (opCounts.count("op:unknown") > 0) {
    report(Severity::Warning, "history-unknown-op", "history",
           std::to_string(opCounts["op:unknown"]) +
               " journal entries name an opcode the engine has no name for; either the "
               "journal is from a newer build or an opcode was added without a name");
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::string projectPath;
  std::string historyPath;
  std::string ignoreFile;
  bool strict = false;
  bool quiet = false;
  uint32_t maxPerRule = 10;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--history" && i + 1 < argc) {
      historyPath = argv[++i];
    } else if (arg == "--ignore-file" && i + 1 < argc) {
      ignoreFile = argv[++i];
    } else if (arg == "--max-per-rule" && i + 1 < argc) {
      maxPerRule = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
    } else if (arg == "--allow" && i + 1 < argc) {
      // "--allow code" or "--allow 'code scope'".
      std::istringstream fields(argv[++i]);
      Allowance a;
      if (fields >> a.code) {
        fields >> a.scope;
        g_allowed.push_back(std::move(a));
      }
    } else if (arg == "--strict") {
      strict = true;
    } else if (arg == "--quiet") {
      quiet = true;
    } else if (arg.rfind("--", 0) == 0) {
      std::fprintf(stderr, "daw_lint: unknown option %s\n", arg.c_str());
      return 2;
    } else if (projectPath.empty()) {
      projectPath = arg;
    } else {
      std::fprintf(stderr, "daw_lint: unexpected argument %s\n", arg.c_str());
      return 2;
    }
  }
  if (projectPath.empty()) {
    std::fprintf(stderr,
                 "usage: daw_lint <project.uniproj.json> [--history <history.jsonl>]\n"
                 "                [--strict] [--quiet] [--max-per-rule N]\n"
                 "                [--allow \"CODE [SCOPE]\"] [--ignore-file PATH]\n");
    return 2;
  }

  std::ifstream in(projectPath);
  if (!in) {
    std::fprintf(stderr, "daw_lint: cannot read %s\n", projectPath.c_str());
    return 2;
  }
  std::stringstream buffer;
  buffer << in.rdbuf();

  daw::ProjectDocument doc;
  std::string error;
  if (!daw::deserializeProject(buffer.str(), doc, &error)) {
    std::fprintf(stderr, "daw_lint: %s does not parse: %s\n", projectPath.c_str(),
                 error.c_str());
    return 2;
  }

  // Declarations of intent: an explicit --ignore-file if given, otherwise a .dawlint
  // beside the project. Beside the project on purpose — the declaration belongs with
  // the thing it is about, so a fixture directory carries its own and moving the
  // project moves its exemptions with it.
  if (!ignoreFile.empty()) {
    if (!loadAllowFile(ignoreFile, /*required=*/true)) {
      std::fprintf(stderr, "daw_lint: cannot read %s\n", ignoreFile.c_str());
      return 2;
    }
  } else {
    const auto beside =
        std::filesystem::path(projectPath).parent_path() / ".dawlint";
    loadAllowFile(beside.string(), /*required=*/false);
  }

  checkGlobals(doc);
  checkClipsAndPlacements(doc);
  checkTracks(doc);
  checkChains(doc);
  checkPatcherAssembly(doc);
  if (!historyPath.empty()) {
    checkHistory(historyPath);
  }

  // Sorted, so the output is a function of the document and nothing else — two runs
  // agree, and a diff between two projects is the difference between them.
  std::sort(g_findings.begin(), g_findings.end());

  uint32_t errors = 0;
  uint32_t warnings = 0;
  uint32_t suppressed = 0;
  uint32_t capped = 0;
  std::map<std::string, uint32_t> shownPerRule;
  std::map<std::string, uint32_t> cappedPerRule;
  for (const auto& finding : g_findings) {
    if (isAllowed(finding)) {
      ++suppressed;
      continue;
    }
    // Counted BEFORE the cap: the cap changes what is printed, never the verdict.
    // Capping a rule into silence and then exiting 0 would be a linter lying about
    // what it found.
    (finding.severity == Severity::Error ? errors : warnings)++;
    uint32_t& shown = shownPerRule[finding.code];
    if (maxPerRule > 0 && shown >= maxPerRule) {
      ++cappedPerRule[finding.code];
      ++capped;
      continue;
    }
    ++shown;
    if (!quiet) {
      std::printf("%s %s %s: %s\n",
                  finding.severity == Severity::Error ? "error" : "warning",
                  finding.code.c_str(), finding.scope.c_str(), finding.detail.c_str());
    }
  }
  if (!quiet) {
    for (const auto& [code, more] : cappedPerRule) {
      std::printf("  ... and %u more %s (raise --max-per-rule to see them)\n", more,
                  code.c_str());
    }
    std::printf("daw_lint: %u error(s), %u warning(s)", errors, warnings);
    if (suppressed > 0) {
      // Always stated. A suppression nobody can see is how a linter stops meaning
      // anything without anyone deciding that it should.
      std::printf(", %u declared intentional", suppressed);
    }
    if (capped > 0) {
      std::printf(", %u not shown", capped);
    }
    std::printf("\n");
  }
  if (errors > 0) return 1;
  if (strict && warnings > 0) return 1;
  return 0;
}
