// STRUCTURAL EQUALITY OVER THE FIELD VISITOR — the first real consumer of the walk, and the reason
// the walk names fields by MEMBER POINTER rather than by reference to one object's member.
//
// WHAT IT IS FOR. DocumentHistory::commit answers "did this command change anything the user
// authored?" by serializing the whole document to JSON and comparing strings. That is correct — it
// is why a refused command no longer destroys the redo tail — but it builds megabytes of text per
// mutating command, and a knob drag emits one command per milli-unit (#119 item 10). This walks
// the fields and stops at the first difference.
//
// THE ASSERTIONS ARE BEHAVIOURAL. The one that matters most is that a change to a DERIVED field is
// NOT a change: hostSlotIndex is an index into this machine's plugin scan, so comparing it would
// report an edit nobody made whenever the cache differed. That is the entire justification for
// FieldKind existing, tested here rather than asserted in a comment.
//
// STILL TO COME, and this test is not the proof of it: documentFieldsEqual must be shown to AGREE
// with serializeProject() string equality across every shipped preset and every mutation
// undo_ratchet drives. That is the evidence rebuilding the serializer would have given, and it is
// what has to exist before commit() switches over.

#include <cstdio>
#include "apps/document_compare.h"
int main() {
  daw::Device a, b;
  int fails = 0;
  auto expect = [&](bool c, const char* w){ if(!c){ std::printf("FAIL: %s\n", w); ++fails; } };

  expect(daw::documentFieldsEqual(a, b), "identical devices compare equal");

  b.bypass = !b.bypass;
  expect(!daw::documentFieldsEqual(a, b), "an authored change is detected");

  b = a;
  b.hostSlotIndex = 12345;
  expect(daw::documentFieldsEqual(a, b),
         "a DERIVED field must NOT count as a change — this is the whole point of FieldKind");

  daw::ProjectTrack t1, t2;
  expect(daw::documentFieldsEqual(t1, t2), "identical tracks compare equal");
  t2.chain.devices.push_back(daw::Device{});
  expect(!daw::documentFieldsEqual(t1, t2), "a device added to the chain is detected (recursion)");

  t2 = t1;
  t2.routing.audioOut.trackId = 4;
  expect(!daw::documentFieldsEqual(t1, t2), "a re-pointed send is detected (nested recursion)");

  if (fails) { std::printf("cmp probe: FAIL (%d)\n", fails); return 1; }
  std::printf("cmp probe: PASS\n");
  return 0;
}
