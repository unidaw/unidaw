#pragma once

#include <cstdint>

namespace daw {

// ONE DECLARATION OF WHAT A STRUCT'S FIELDS ARE, and every traversal written against it.
//
// THE PROBLEM THIS EXISTS TO END. Serialize, deserialize, compare, capture and apply are five
// separate hand-maintained lists of the same fields, and this repo has paid for the divergence
// repeatedly and expensively:
//   - captureDocument wrote FIVE fields of an aux-child track while the handlers accepted every
//     edit on one, so a stem's chain and quantize were dropped on save;
//   - AuxChildOverlay held the SAME five on the way back in, so undo could not restore them —
//     the same subset in the inverse function, which is where nobody looked;
//   - alternateClipId was emitted by save and read by the parser but never rebuilt by the load,
//     so an A/B draft survived until you reopened the project;
//   - the clip-adoption rule had FOUR hand-rolled copies, and a commit message that said two.
// Each was found separately, months apart, by a different accident. A visitor makes them one bug
// that cannot occur rather than four that recur.
//
// WHY A FUNCTION PER STRUCT AND NOT A MACRO. visitFields is ordinary C++ that a reader can step
// through, and adding a field to a struct without adding it here is caught by review and by the
// round-trip checks rather than by a macro expansion nobody can read. The codebase already
// prefers explicit + compiler-enforced (device_chain.h's switch with no default:, the deps
// structs matched positionally by name); this is the same taste.
//
// HOW IT WILL BE USED, in the order the work must happen:
//   1. Rebuild serializeProject/deserializeProject on this FIRST. They have 135 checks and every
//      shipped preset behind them, so a wrong visitor fails loudly and immediately. Building the
//      differ first would mean debugging a new mechanism against a new consumer with nothing to
//      check either one.
//   2. Then the differ, which is the same walk with two objects.
//   3. Then shared_ptr<const T> for the heavy leaves: a version drops from ~9.1 MB (stress-512,
//      80,896 events x 112 B) to ~100 bytes, with NO change to DocumentHistory's interface or to
//      undo's behaviour. That separation is precisely why correctness shipped before
//      representation.

// WHAT A FIELD *MEANS*, which is not the same question as what type it has.
//
// This enum is not decoration. It exists because hostSlotIndex proved that a single field can be
// two things at once, and that a differ cannot be written until they are told apart:
//
//   Device::hostSlotIndex is a DERIVED index into this machine's plugin-cache scan — recomputed
//   by resolveDeviceSlot (device_chain.cpp:200) from vstRef on every load. Two semantically
//   identical documents differ in it whenever the cache differs: a plugin installed, removed, or
//   scanned in another order. A differ that compares it reports a change nobody made.
//
//   ...EXCEPT that kHostSlotIndexDirect ("load by path, not from the scan") is AUTHORED INTENT,
//   which fixtures set deliberately as input. So the field is a derived cache AND an authored
//   mode sharing one uint32_t, and "just mark it Derived" is wrong — it has to be SPLIT. That
//   split is the first concrete task of stage 3, and this enum is what makes the requirement
//   visible instead of a surprise found halfway through the refactor.
//
// The cost of getting this wrong is not hypothetical either: a stale persisted hostSlotIndex once
// resolved to the engine's DEFAULT plugin on the master bus, which output silence and muted the
// whole mix (see the note at engine_load_project.cpp:467).
enum class FieldKind : uint8_t {
  // Part of the authored document. Saved, compared, undone. The default, because a field nobody
  // has thought about should be treated as the user's work rather than silently discarded.
  Authored,
  // Recomputable from Authored fields by a pure function. NOT compared by the differ and NOT a
  // reason to record an undo version. May still be serialized (as a cache), but a reader must be
  // free to ignore it — if it cannot, it is Authored and mislabelled.
  Derived,
  // Names the thing rather than describing it: trackId, clip id, device id, placement id. Compared
  // for CORRESPONDENCE (which object is this?) rather than for equality of content, so a differ
  // can say "device 7 changed" instead of "the third element differs".
  Identity,
  // A field UNDO MUST NEVER RESTORE. That is the whole rule, and it is what the name is for.
  //
  // Most such fields are session state that is not in the document at all — the transport loop,
  // playhead, selection — which is the bug fixed in 53b77d5, where undo ran the load path and
  // wiped the user's loop region on every Ctrl-Z. That was the only shape when this was written,
  // so the definition used to SAY "not part of the document at all".
  //
  // It is not the only shape. `ProjectDocument::nextDeviceId` is PERSISTED — a project must
  // remember which ids it has already spent — and must still never go backwards, because undo
  // restoring a lower watermark makes a deleted device's id available again, and the replacement
  // inherits its plugin state (AE-P1.2 G2-B item 18, R-DEVICE-ID-LIFETIME). Persisted and
  // un-undoable are independent properties; this kind names the second one.
  Session,
};

// A visitor names each field once. Implementations provide field() overloads for the leaf types
// they care about and recurse into nested structs by calling visitFields again.
//
// Deliberately NOT an abstract base with virtuals: this walks 80,000-element vectors, and the
// serializer already runs per save. Templated so each traversal inlines to roughly the hand-written
// loop it replaces.
//
//   struct MyVisitor {
//     template <typename T>
//     void field(const char* name, T& value, FieldKind kind = FieldKind::Authored);
//   };
//
// FIELDS ARE NAMED BY MEMBER POINTER, NOT BY REFERENCE TO ONE OBJECT'S MEMBER.
//
// The first version passed `T& value` and handed the visitor `field(name, value.member)`. That is
// fine for serialize, hash and collect — and it CANNOT DO A PAIRWISE COMPARE, which is what undo
// actually needs: field() had no way to reach the corresponding member of a second object. I found
// this by trying to write the comparer, which is the cheapest possible moment to find it, because
// nothing consumes the walk yet.
//
// A member pointer is a description of WHICH field, independent of any instance, so a visitor
// applies it to as many objects as it likes: one for serialize, two for compare, N for a merge.
// The alternatives were all worse — a second pairwise overload per struct doubles the declarations
// this exists to abolish; buffering both walks loses the short-circuit; and a digest visitor trades
// a hash collision for a SILENTLY LOST UNDO STEP, which is not a probability worth accepting in the
// mechanism that decides whether the user's edit is recorded.
//
//   struct MyVisitor {
//     template <typename C, typename M>
//     void field(const char* name, M C::*member, FieldKind kind = FieldKind::Authored);
//   };
//   visitFields<ProjectTrack>(myVisitor);
//
// The field list must be kept in the SAME ORDER as the struct's declaration — nothing mechanical
// enforces it, and the reader diffing the two lists is what catches a field somebody forgot.
template <typename T, typename V>
void visitFields(V& visitor);

}  // namespace daw
