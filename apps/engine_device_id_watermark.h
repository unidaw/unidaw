#pragma once

#include <cstdint>
#include <mutex>

#include "apps/stable_device_id.h"

// THE ENGINE'S LIVE DEVICE-ID HIGH-WATER MARK.
//
// AE-P1.2 G2-B item 18, R-DEVICE-ID-LIFETIME: "The schema-6 ProjectDocument carries one
// next_device_id high-water mark allocated under the command-writer lock; it never decreases on
// load or undo/redo, is persisted across save/load, and never reuses a deleted id."
//
// TWO PLACES HOLD THIS NUMBER AND ONLY ONE IS AUTHORITATIVE. The document field is the
// SERIALIZATION; this object is the AUTHORITY. They meet at exactly two functions —
// `captureDocument` stamps the authority into the document, `applyDocument` adopts the document
// into the authority — which is why neither can drift: there is no third site that could form its
// own opinion.
//
// WHY `adopt` TAKES THE MAX RATHER THAN ASSIGNING. Undo restores an OLDER document, whose
// watermark is lower by exactly the ids the undone edits allocated. Assigning would hand those ids
// straight back out, and the device they used to name still owns a plugin-state blob, a parameter
// manifest, every automation lane pointed at it and every mirror entry keyed on it — so the
// replacement would inherit a dead device's sound. Taking the max is what makes "never reuses a
// deleted id" survive undo, and it is the whole reason this is not simply a document field.
//
// The same rule covers load: opening a project whose watermark is below the live one keeps the
// live one. That costs a few id numbers in a long session and buys the property that no id issued
// in this process ever names two different devices.
//
// ITS OWN MUTEX, not a comment saying "command thread only". Allocation is a read-modify-write,
// and `capture`/`adopt` are called from the command thread while a save may be reading. The lock
// is uncontended in the normal case and makes the invariant true by construction rather than by
// everyone remembering which thread they are on.

namespace daw::engine {

class DeviceIdWatermark {
 public:
  // ALLOCATE THE NEXT ID, or return 0 when the space is exhausted.
  //
  // Zero is not an id, so a caller that ignores the result cannot accidentally build a device out
  // of the failure value — `addDevice` refuses it, which is the second guard on the same fact.
  uint32_t allocate() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stableDeviceIdWatermarkExhausted(next_)) {
      return 0;
    }
    return next_++;
  }

  // RAISE THE MARK TO COVER `documentValue`, never lower it. Returns the value now held.
  //
  // A malformed value is clamped INTO range rather than adopted: a document claiming a watermark
  // above `kStableDeviceIdExhausted` would otherwise make the engine believe it had run out when
  // it had not, and one below `kStableDeviceIdMin` says nothing at all.
  uint32_t adopt(uint32_t documentValue) {
    const uint32_t bounded =
        documentValue > kStableDeviceIdExhausted ? kStableDeviceIdExhausted : documentValue;
    std::lock_guard<std::mutex> lock(mutex_);
    if (bounded > next_) {
      next_ = bounded;
    }
    return next_;
  }

  // WHAT A DOCUMENT SHOULD BE STAMPED WITH. Named `capture` rather than `get` because that is the
  // only thing it is for; a caller wanting to know whether allocation is still possible asks
  // `exhausted()`.
  uint32_t capture() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return next_;
  }

  bool exhausted() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stableDeviceIdWatermarkExhausted(next_);
  }

  // FOR TESTS AND FOR A FRESH ENGINE ONLY. Undo must never reach this — it is the one motion that
  // would otherwise lower the mark, which is exactly what `adopt` exists to prevent.
  void resetForNewProcess() {
    std::lock_guard<std::mutex> lock(mutex_);
    next_ = kStableDeviceIdMin;
  }

 private:
  mutable std::mutex mutex_;
  uint32_t next_ = kStableDeviceIdMin;
};

}  // namespace daw::engine
