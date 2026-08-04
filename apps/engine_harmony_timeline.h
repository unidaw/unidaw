#pragma once

// THE HARMONY TIMELINE — the first engine OBJECT, as opposed to another deps struct.
//
// WHY THIS IS A DIFFERENT SHAPE FROM THE *Deps STRUCTS AROUND IT, and the measurement that forced
// it. Extracting main()'s remaining lambdas behind deps structs was tried and abandoned: the cost
// plateaus at about six lines of body per new dependency, so moving the 14 largest would need a
// 116-member struct — worse than the 72-member HandleUiEntryDeps a maintainability panel named as
// a defect. Those lambdas are not handlers that happen to live in main(); they ARE main()'s state
// manipulation, and a struct of references cannot express that more cheaply than the state does.
//
// So the state moves WITH the code that owns it. harmonyEvents, harmonyMutex, harmonyVersion and
// harmonyDirty are not dependencies of these four operations — they are what these four operations
// ARE. Here they are members, the operations are member functions, and nothing has to be passed.
//
// AND THE DEPS COUNT GOES DOWN, which is the thing the deps-struct approach could not do. Those
// four variables appear as SEVENTEEN separate members across six *Deps structs today
// (arrangetime 4, consumer 4, load_project 4, produce_block 2, render_track 2, save_project 1).
// Each of those can hold one HarmonyTimeline& instead: 17 members become 6.
//
// THE MEMBER NAMES ARE THE OLD CAPTURE NAMES, deliberately and not for lack of better ones.
// `harmonyEvents` rather than `events` is what lets every body move VERBATIM and be proven so by a
// line-for-line diff — the same discipline every extraction in this refactor has used, and the only
// reason a change this shape is safe to make in one commit. Renaming is a separate edit that can be
// reviewed on its own.
//
// THREE COLLABORATORS ARE INJECTED, and only three: the scale registry it resolves against, and the
// two things it must tell when the timeline changes. Those are genuinely other people's business.
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <vector>

#include "clip_edit.h"
#include "event_payloads.h"
#include "harmony_timeline.h"
#include "scale_library.h"

namespace daw::engine {

class HarmonyTimeline {
 public:
  HarmonyTimeline(const daw::ScaleRegistry& scaleRegistry,
                  std::function<void(const daw::UiHarmonyDiffPayload&)> emitHarmonyDiff,
                  std::function<void(const daw::UndoEntry&)> pushHarmonyUndo)
      : scaleRegistry(scaleRegistry),
        emitHarmonyDiff(std::move(emitHarmonyDiff)),
        pushHarmonyUndo(std::move(pushHarmonyUndo)) {}

  // PUBLIC because the render and publish paths read them directly and always have. Making them
  // private would mean an accessor per member and a diff that is no longer verbatim — the point of
  // this commit is that the state stopped being scattered, not that it grew an interface.
  std::atomic<bool> harmonyDirty{true};
  std::atomic<uint32_t> harmonyVersion{0};
  std::mutex harmonyMutex;
  std::vector<daw::HarmonyEvent> harmonyEvents;

  std::optional<daw::HarmonyEvent> getHarmonyAt(uint64_t nanotick);
  const daw::Scale* getScaleForHarmony(const daw::HarmonyEvent& harmony);
  bool addOrUpdateHarmony(uint64_t nanotick, uint32_t root, uint32_t scaleId, bool recordUndo);
  bool removeHarmony(uint64_t nanotick, bool recordUndo);

 private:
  const daw::ScaleRegistry& scaleRegistry;
  std::function<void(const daw::UiHarmonyDiffPayload&)> emitHarmonyDiff;
  std::function<void(const daw::UndoEntry&)> pushHarmonyUndo;
};

}  // namespace daw::engine
