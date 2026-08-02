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

// Codes are per-family small integers; naming them here keeps the numbers out of the log, where
// nobody remembers what routing error 3 was. An unknown family or an out-of-range code formats as
// "code:N" rather than asserting — a refusal that cannot be named must still be reportable.
std::string errorScopeName(const char* family, uint16_t code);

// Flips an undo entry into its redo. UpdateHarmony is the only self-inverse case and it swaps the
// before/after pairs; every other type maps to its opposite.
daw::UndoEntry invertUndoEntry(const daw::UndoEntry& entry);

// Blob and manifest filenames for one (track, device). The DIRECTORY rule deliberately does not
// live here — it is daw::pluginStateDirFor in project_file.cpp, because saveProjectModule needs it
// too: a lambda in the engine could be seen by the save that writes the blobs and the load that
// restores them, and NOT by the packer that has to find them, which is exactly why a `.uni`
// carried every sample and no plugin state at all.
std::string pluginStateFileName(uint32_t trackId, uint32_t deviceId);

// The PARAMETER MANIFEST beside the opaque blob. The blob is the plugin's private state and says
// nothing to anyone but the plugin; this says what the knobs WERE, in a form readable without the
// plugin installed and without the engine running. Its own file rather than a field in
// project.json, because it is DERIVED from the plugin rather than authored.
std::string pluginParamsFileName(uint32_t trackId, uint32_t deviceId);

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

}  // namespace daw::engine
