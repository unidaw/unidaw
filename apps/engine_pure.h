// THE ENGINE'S PURE HELPERS — the part of daw_engine_main.cpp that depends on nothing.
//
// Every function here was a lambda inside `main()`. All of them were written `[&]` or `[]` and
// capture NOTHING: they read only their arguments. That made them pure by construction and
// untestable by accident — a lambda in a 20,000-line `main()` has no name any other translation
// unit can say, so not one of these eleven rules had a unit test, however simple it was.
//
// That is the whole argument for this file. It is not tidying. `clipContentEnd` decides how far a
// clip extends, `invertUndoEntry` decides what undo does, `clampMidi` decides what happens at the
// edges of the MIDI range — and each was reachable only by booting an engine, opening shared
// memory and driving a command through a ring. Tests that expensive get written for features, not
// for edge cases, which is precisely backwards for rules like these.
//
// WHAT BELONGS HERE: a function with no captured state, no I/O, no locks and no threading
// affinity. If it needs engine state, it belongs in a module that owns that state, not here.
//
// Several of these are called from the PRODUCER THREAD, which runs at audio priority
// (`elevateToAudioPriority()`): clampMidi, and clipContentEnd via the flatten path. They allocate
// exactly what they allocated as lambdas and no more. Adding an allocation, a lock or a
// `std::function` indirection to anything here is a real-time correctness regression, not a
// style choice — see apps/engine_pure_tests_main.cpp, which pins the shapes that matter.
#pragma once

#include <cstdint>
#include <string>

#include "apps/clip_edit.h"
#include "apps/event_payloads.h"
#include "apps/musical_structures.h"
#include "apps/project_file.h"
#include "apps/time_signature_map.h"
#include "apps/sampler_state.h"

namespace daw::engine {

// True when any device in the document carries its own patcher graph. The save must never park
// the global pool on a device in that case: the device graphs ARE the authored data, and the pool
// is a derived join of them.
bool documentHasPerDeviceGraphs(const daw::ProjectDocument& doc);

// DERIVED FROM THE STRING THE LOG ALREADY CARRIES, rather than a second variable set beside it.
// Two handlers pick their reason at runtime into a `why` string; adding a parallel code would be
// two facts about one thing, and the first edit that touched only one of them would make the log
// and the wire disagree about why the same command was refused.
daw::UiSamplerRejectReason samplerReasonFor(const char* why);

// The inverse: a reason back to the word the engine used for it. For the journal, where a number
// tells the reader nothing — the same argument errorScopeName makes for chain/routing/mod.
const char* samplerReasonName(daw::UiSamplerRejectReason reason);

// Codes are per-family small integers; naming them here keeps the numbers out of the log, where
// nobody remembers what routing error 3 was. An unknown family or an out-of-range code formats as
// "code:N" rather than asserting — a refusal that cannot be named must still be reportable.
std::string errorScopeName(const char* family, uint16_t code);

// Flips an undo entry into its redo. UpdateHarmony is the only self-inverse case and it swaps the
// before/after pairs; every other type maps to its opposite.
daw::UndoEntry invertUndoEntry(const daw::UndoEntry& entry);

// pluginStateFileName / pluginParamsFileName WERE HERE, and they are gone deliberately.
//
// They took `(uint32_t trackId, uint32_t deviceId)` and forwarded to daw::artifactLeafName. Under
// AE-P1.2 G2-B item 18 there are exactly two legal reasons to name an artifact file, and neither
// wants that signature:
//
//   an inventory entry's leaf   -> daw::artifactLeafName(...), inside apps/artifact_inventory.*,
//                                  used to BUILD an entry and always joined to the generation dir
//   a schema 1-5 flat path      -> daw::legacyArtifactLeafName(key, kind), which cannot be called
//                                  without a LegacyArtifactKey
//
// A helper that accepts two loose integers accepts the device's CURRENT id, which is precisely the
// probe `legacy_precedence` forbids — and a grep guarding it was defeated three ways in review.
// Removing the spelling was cheaper and stronger than watching for its misuse.

// The tick just past the last event in a clip — its content extent. Notes and chords contribute
// their duration; everything else contributes only its offset.
uint64_t clipContentEnd(const daw::MusicalClip& clip);

// A clip-edit diff carries the edited note's tick in clip-relative space (the owned clip the edit
// ran on). Shift it back onto the arrangement timeline by the placement anchor before it goes to
// the UI, which speaks absolute ticks. The tick is split across two 32-bit wire fields, so this
// is also the one place that split is reassembled and re-split.
void shiftDiffTick(daw::UiDiffPayload& d, uint64_t placementAt);

// Find the envelope for a target in a mod set, or MINT one. Minting rather than refusing is the
// point: every mod set starts with no modulators at all, so "edit the cutoff envelope" would
// otherwise depend on a command that creates one, which does not exist.
//
// A fresh envelope's `apply` encodes the rule that a VOLUME envelope MULTIPLIES (an amp envelope
// that added would never reach silence, however deep it went) and everything else ADDS to a base
// value. Putting that on the wire would let a caller build a modulator that cannot do anything
// musical, and then wonder why.
//
// THE RETURNED POINTER IS INVALIDATED BY THE NEXT MINT INTO THE SAME MOD SET. It points into
// `ms.modulators`, and minting push_backs, which can reallocate. Use it and drop it; do not hold
// it across another call. Both engine call sites consume it within the same loop iteration and
// are correct today — this is written down because nothing enforces it, and the first test ever
// written against this function tripped over it immediately by comparing two pointers' fields
// across a mint.
daw::SamplerModulator* findOrMintEnvelope(daw::SamplerModSet& ms, daw::ModTarget target);

// THE SAME ARGUMENT ONE LEVEL UP: a mod set to mint the modulator IN.
//
// A sampler can legitimately hold no mod sets — a device added to a chain and not yet loaded, or a
// project that saved none — and every envelope and LFO command iterates `modSets` looking for one.
// Over an empty vector that loop body never runs, so the command applies to nothing and reports
// "no_such_mod_set" to the event log while the caller sees `{"sent": ...}` and a kit whose modMask
// stays 0.
//
// MINTING IS ONLY RIGHT WHEN THE CALLER DID NOT NAME ONE. `--mod-set 0` means "the default", a
// request that can always be satisfied. `--mod-set 7` when there is no 7 is a caller naming
// something that does not exist, and must still be refused — substituting a different mod set
// would silently edit the wrong thing, which is worse than doing nothing.
void ensureDefaultModSet(daw::SamplerState& sampler, uint32_t requestedId);

// Clamp an arbitrary int to the MIDI pitch range. Reached from the producer thread via the
// harmony/chord path, where an out-of-range pitch is a transposition that walked off the end of
// the keyboard rather than a caller error — so it saturates and stays silent about it.
uint8_t clampMidi(int pitch);


// THE DISPLAY NAME A SAMPLE FILE SEEDS A SLOT WITH: the basename with its extension dropped.
//
// `sampler-load` used to stamp the WHOLE PATH onto slot.name — the same string it matches against
// SamplerSource::path — which was invisible until v36 published the name and would have drawn
// /Users/jak/samples/kicks/BD_808.wav on a pad. The path still lives on the SOURCE, which is what
// actually needs to find the file again; the slot gets something a person can read.
//
// Clamped to the published width so a long filename seeds a name that survives the trip out. This
// is the ONE place a name is shortened rather than refused, and it is the right place: nobody
// typed it, so there is no write to reject and nothing to look like it worked.
std::string sampleDisplayName(const std::string& path);


// ---------------------------------------------------------------- WHERE A BAR STARTS AND ENDS
//
// THE RULER'S RULE, AND THE ONLY COPY OF IT. Four note-entry sites once computed a bar as
// `(tick / (4 * quarter) + 1) * (4 * quarter)` — 4/4 hardcoded, the meter map ignored. In any
// project that is not 4/4, or that changes meter anywhere, note entry then disagreed with the
// ruler about where a bar is: a new clip anchored to the wrong boundary, a note with no duration
// ran to the wrong place, and writing past the end grew the song to the wrong tick.
// time_signature_map.h's own opening comment warns about exactly this — "the bar a tick falls in
// is NOT (tick / barLength)" — because bars before a signature change are a different length.
//
// A null `meter` means the snapshot has not been published yet (startup), NOT "no meter": these
// then fall back to a 4/4 bar, which is what the map itself defaults to.
//
// WHY THESE TAKE A POINTER INSTEAD OF READING THE SNAPSHOT: the callers hold a track's
// trackMutex, and the map lives under arrangeMutex. Taking that pair nested is an AB/BA deadlock,
// so the caller loads the atomically-swapped snapshot and passes the result in. Keeping the load
// at the call site is also what makes these two testable at all.
//
// THE DEGENERATE GUARDS BELOW ARE LIVE, not defensive decoration. `TimeSignature::valid()` accepts
// any power-of-two denominator, so 4/4194304 is "valid" and its beatNanoticks() — computed as
// (4 * kNanoticksPerQuarter) / denominator — truncates to ZERO. A bar of zero length makes
// barBeatAt() give up and return bar 1, and then tickAtBar() answers 0 for every bar. Both of
// those numbers arrive here looking perfectly ordinary. A project file supplies the denominator,
// so this is reachable from a file rather than from a bug.

// Where the bar containing `tick` ENDS. Strictly greater than `tick`, always: a boundary at or
// before the tick would give a zero-length default note duration and a span that does not grow,
// which reads as "the note did nothing" rather than as a bad meter.
uint64_t barEndTick(const daw::TimeSignatureMap* meter, uint64_t tick);

// Where the bar containing `tick` STARTS — barEndTick's other half, and what a new clip anchors
// to. Never past `tick`: an anchor after the note it was created for would give that note a
// negative offset inside its own clip.
//
// ITS CLAMP IS CURRENTLY UNREACHABLE, and that is measured rather than assumed. TimeSignatureMap
// is self-consistent — the start of the bar barBeatAt() names is never past the tick asked about,
// including in every degenerate meter — so the clamp never fires and removing it changes no
// result. It is kept as defence against a future change to the map, and the premise it rests on
// is pinned by testTheMapNeverPutsABarStartPastItsOwnTick, which is what would report that the
// clamp had become live.
uint64_t barStartTick(const daw::TimeSignatureMap* meter, uint64_t tick);


// HOW FAR A PLACEMENT REACHES, in one place instead of five.
//
// A placement may carry its own length, or leave it zero to mean "as long as the clip". Three
// fallback levels, in order, because each was added by a different bug:
//
//   1. the placement's own lengthNanoticks, when set;
//   2. the referenced clip's lengthNanoticks;
//   3. the clip's CONTENT extent, when the clip's own length is also zero.
//
// LEVEL 3 EXISTED AT ONE SITE OUT OF FIVE, and its absence elsewhere is not theoretical. A clip
// can hold notes while its own length is still zero, and every site without this fallback then
// measured that placement as EMPTY — the web UI's shared-clip warning went silent on exactly the
// placement somebody had just created, which is when they are most likely to type into it. Note
// entry said the placement covered its content; the published extent said it covered nothing.
// One placement, two answers, and nothing comparing the two code paths could see it because they
// agree on every name.
//
// Returns 0 when no clip in `clips` has the placement's id. That is what all five hand-written
// copies did — a dangling clip reference reaches nowhere rather than defaulting to something
// plausible, because a plausible length would hide the dangling reference.
uint64_t placementLength(const daw::ProjectPlacement& placement,
                         const std::vector<daw::ProjectClip>& clips);

// Where a placement ENDS, saturating. A placement near the top of the tick range must not wrap to
// a small number: as a song end that would silence everything after it, and in the ripple planner
// that would wrongly accept or refuse a time edit. Three of the five copies added these two
// numbers unguarded; only the song-end one said why the guard was there.
uint64_t placementReach(uint64_t at, uint64_t length);



// DOES A REGION OF `regionSize` BYTES, AT `offset`, FIT INSIDE A MAPPING OF `mappedSize`?
//
// AE-P1.3. Extracted rather than written inline at the attach site, for the reason this whole file
// exists: the arithmetic that decides whether a shared-memory offset is believable was reachable
// only by connecting a real host, so the case it exists to refuse — a malformed one — could not be
// posed at all. A guard that cannot be shown to reject anything is indistinguishable from no guard.
//
// THE SUBTRACTION IS THE POINT, and it is why this is a function rather than `offset + regionSize
// <= mappedSize`. That form overflows: an offset near the top of the range wraps the sum back into
// bounds and the check waves through exactly the value it exists to catch. Subtracting instead
// cannot overflow, and the `mappedSize < regionSize` test in front of it is what stops the
// subtraction underflowing.
//
// Alignment is part of fitting, not a separate question: a misaligned pointer to a type with
// stricter alignment is undefined behaviour before it is a bounds problem.
constexpr bool shmRegionFits(uint64_t offset, uint64_t regionSize, uint64_t align,
                             uint64_t mappedSize) {
  if (align == 0 || offset % align != 0) {
    return false;
  }
  if (mappedSize < regionSize) {
    return false;
  }
  return offset <= mappedSize - regionSize;
}

}  // namespace daw::engine
