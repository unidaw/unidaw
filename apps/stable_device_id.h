#pragma once

#include <cstdint>

// WHAT A STABLE DEVICE ID IS, in one place, because it crosses every durable boundary.
//
// AE-P1.2 G2-B item 18, R-DEVICE-ID-LIFETIME. A device id used to be TRACK-SCOPED: `addDevice`
// took `max(existing)+1` over one chain, so two tracks each held a device numbered 1, and the id
// alone did not say which device it meant. Everything that had to name a device across a durable
// boundary — automation, mirrors, editor/parameter requests, state capture, save/load, plugin
// state blobs, parameter manifests, meters, modulation, sampler and patcher ownership — therefore
// carried a track id beside it, or a compact host index, or both. Three of those carriers are 15
// or 16 bits wide and had no room for the pair.
//
// The id is PROJECT-GLOBAL now, so the id alone is the name. That only works if the id fits every
// carrier that already exists, which is why the ceiling is 0x7FFF rather than something rounder:
// it is the NARROWEST LOSSLESS BOUND across the carriers in this repo (a 15-bit patcher-owner
// field being the tightest). Widening the carriers instead would be a wire break in five places
// for a project that will never hold 32,767 devices.
//
// THE THREE RESERVED VALUES ARE NOT IDS: 0 means "absent" throughout the engine (nine sampler
// guards read it that way), `kDeviceIdAuto` (0xFFFFFFFF) means "pick one", and `kParamTargetAll`
// (0xFFFFFFFF) means "every target". All three are outside [1, 0x7FFF] by construction, and the
// static_asserts beside their own definitions keep it that way rather than leaving it to be true
// by coincidence.
//
// WHY A HEADER OF ITS OWN, rather than beside `kDeviceIdAuto` in device_chain.h: the narrow
// carriers that must validate before converting live in shared_memory.h and event_payloads.h, and
// neither can include device_chain.h (which drags in the plugin cache, the sampler document and
// the patcher graph). A rule that the narrowing sites cannot reach is a rule they will re-derive.

namespace daw {

// The first id an empty project allocates. Zero is the ABSENCE of a device, never a device.
constexpr uint32_t kStableDeviceIdMin = 1u;

// The last allocatable id. Exactly 0x7FFF — see the header comment for why this number.
constexpr uint32_t kStableDeviceIdMax = 0x7FFFu;

// max + 1 = 0x8000. A watermark holding this has no id left to give; it is not itself an id, and
// it is the value every "exhausted" test compares against rather than a second magic number.
constexpr uint32_t kStableDeviceIdExhausted = kStableDeviceIdMax + 1u;

static_assert(kStableDeviceIdExhausted == 0x8000u,
              "the exhausted watermark is max+1, and callers depend on the exact value");

// IS THIS A DEVICE IDENTITY? The single predicate. Every durable boundary asks this before
// trusting an id, and every narrowing carrier asks it before converting.
constexpr bool isStableDeviceId(uint32_t id) {
  return id >= kStableDeviceIdMin && id <= kStableDeviceIdMax;
}

// IS THIS A LEGAL WATERMARK VALUE? A watermark names the id the NEXT allocation would take, so
// it ranges over [min, exhausted] — one wider than an id, because "nothing left" is a legal
// state and must be representable without aliasing a real id.
constexpr bool isStableDeviceIdWatermark(uint32_t watermark) {
  return watermark >= kStableDeviceIdMin && watermark <= kStableDeviceIdExhausted;
}

// Has this watermark run out? Separate from the range check so a caller cannot accidentally
// accept `kStableDeviceIdExhausted` as an id by testing only that the watermark was legal.
constexpr bool stableDeviceIdWatermarkExhausted(uint32_t watermark) {
  return watermark >= kStableDeviceIdExhausted;
}

// CHECKED NARROWING TO 16 BITS, returning whether it was lossless.
//
// The point is the RETURN VALUE. `static_cast<uint16_t>(id)` compiles, never warns, and turns
// 0x10001 into 1 — a different, existing device. Every producer and consumer of a 15-bit, uint16,
// sampler-address, patcher-flag, or UI/control carrier goes through here so that a value which
// does not fit is REFUSED rather than silently becoming its own low half.
constexpr bool narrowStableDeviceId(uint32_t id, uint16_t& narrowed) {
  if (!isStableDeviceId(id)) {
    return false;
  }
  narrowed = static_cast<uint16_t>(id);
  return true;
}

// CHECKED WIDENING BACK. The inverse boundary: a carrier hands back 16 bits and the caller wants
// a device id. Zero on the wire is "no device", which is a legal thing for a carrier to say and
// is NOT an id — so it is refused here rather than being handed on as device 0.
constexpr bool widenStableDeviceId(uint16_t narrowed, uint32_t& id) {
  const uint32_t candidate = static_cast<uint32_t>(narrowed);
  if (!isStableDeviceId(candidate)) {
    return false;
  }
  id = candidate;
  return true;
}

}  // namespace daw
