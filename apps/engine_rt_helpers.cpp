// Bodies for apps/engine_rt_helpers.h. The WHY for each rule is in the header, beside the
// declaration; this file is the mechanics only.
#include "apps/engine_rt_helpers.h"

namespace daw::engine {

std::optional<daw::HarmonyEvent> harmonyAtOrDefault(
    const std::vector<daw::HarmonyEvent>& events, uint64_t nanotick) {
  if (events.empty()) {
    return daw::HarmonyEvent{0, 0, 1, 0};
  }
  return daw::harmonyAt(events, nanotick);
}

daw::ResolvedPitch quantizePitch(const daw::ScaleRegistry& registry, uint8_t pitch,
                                 const daw::HarmonyEvent& harmony) {
  const auto* scale = registry.find(harmony.scaleId);
  if (!scale) {
    return daw::resolvedPitchFromCents(static_cast<double>(pitch) * 100.0);
  }
  return daw::quantizeToScale(pitch, harmony.root, *scale);
}

void enqueueMirrorReplay(TrackRuntime& runtime) {
  if (runtime.isAuxChild.load(std::memory_order_acquire)) {
    return;
  }
  runtime.mirrorGateSampleTime.store(0, std::memory_order_release);
  runtime.mirrorPending.store(true, std::memory_order_release);
  runtime.mirrorPrimed.store(false, std::memory_order_release);
}

}  // namespace daw::engine
