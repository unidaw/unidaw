#include "engine_bulk_edit.h"

// What the body actually reaches for — the file arrived carrying main.cpp's 94 includes,
// which describe where it used to live rather than what it uses.
#include <filesystem>

#include "engine_pure.h"
#include "event_log.h"


namespace daw::engine {

void handleAssembledBulk(AssembledBulkDeps& deps, const std::vector<uint8_t>& buf) {
  // Re-bind every dependency to the name the body already uses.
  auto& bumpClipVersionFor = deps.bumpClipVersionFor;
  auto& clipDirty = deps.clipDirty;
  auto& publishAudioClipTable = deps.publishAudioClipTable;
  auto& rebuildAudioRender = deps.rebuildAudioRender;
  auto& rebuildFlatAndPublish = deps.rebuildFlatAndPublish;
  auto& refreshSamplerForTrack = deps.refreshSamplerForTrack;
  auto& reportSamplerReject = deps.reportSamplerReject;
  auto& requireMatchingClipVersion = deps.requireMatchingClipVersion;
  auto& resolveSourcePath = deps.resolveSourcePath;
  auto& tracks = deps.tracks;
  auto& tracksMutex = deps.tracksMutex;

    if (buf.size() < sizeof(uint16_t)) {
      return;
    }
    uint16_t inner = 0;
    std::memcpy(&inner, buf.data(), sizeof(inner));
    const auto innerType = static_cast<daw::UiCommandType>(inner);

    // ---- SAMPLER SET ENVELOPE POINTS (84). The pencil, where 82 is the sliders.
    if (innerType == daw::UiCommandType::SamplerSetEnvelopePoints) {
      if (buf.size() < sizeof(daw::UiSamplerEnvPointsHeader)) {
        DAW_EVENT("bulk.rejected")
            .field("op", static_cast<uint32_t>(inner))
            .field("bytes", static_cast<uint64_t>(buf.size()))
            .field("reason", "short_header");
        return;
      }
      daw::UiSamplerEnvPointsHeader h{};
      std::memcpy(&h, buf.data(), sizeof(h));
      const size_t need =
          sizeof(h) + static_cast<size_t>(h.pointCount) * sizeof(daw::UiEnvPointWire);
      // REFUSED, not truncated. An envelope with half its points is a VALID envelope, so a
      // carrier that delivered what arrived would produce a wrong sound rather than an error —
      // which is the whole reason seq/total exist.
      if (h.pointCount < 2 || buf.size() < need) {
        DAW_EVENT("bulk.rejected")
            .field("op", static_cast<uint32_t>(inner))
            .field("points", static_cast<uint32_t>(h.pointCount))
            .field("bytes", static_cast<uint64_t>(buf.size()))
            .field("need", static_cast<uint64_t>(need))
            .field("reason", h.pointCount < 2 ? "too_few_points" : "short_payload");
        return;
      }
      TrackRuntime* runtime = daw::engine::trackAt(tracks, tracksMutex, h.trackId);
      if (!runtime) {
        reportSamplerReject(daw::UiCommandType::SamplerSetEnvelopePoints, daw::UiSamplerRejectReason::NoSuchTrack,
                            h.trackId, 0, 0);
        DAW_EVENT("sampler.envelope_rejected")
            .field("track", h.trackId)
            .field("reason", "no_such_track");
        return;
      }
      daw::EnvShape shape;
      shape.points.reserve(h.pointCount);
      for (uint16_t i = 0; i < h.pointCount; ++i) {
        daw::UiEnvPointWire w{};
        std::memcpy(&w, buf.data() + sizeof(h) + static_cast<size_t>(i) * sizeof(w), sizeof(w));
        daw::EnvPoint pt;
        pt.time = w.time;
        pt.valueMilli = std::clamp<int16_t>(w.valueMilli, -1000, 1000);
        pt.tension = w.tension;
        pt.flags = w.flags;
        shape.points.push_back(pt);
      }
      shape.sustainLoopStart = h.sustainLoopStart;
      shape.sustainLoopEnd = h.sustainLoopEnd;
      shape.releaseLoopStart = h.releaseLoopStart;
      shape.releaseLoopEnd = h.releaseLoopEnd;
      shape.releaseFade = h.releaseFade;

      bool applied = false;
      uint16_t targetId = 0;
      daw::EnvRepair repair;
      {
        std::lock_guard<std::mutex> lock(runtime->trackMutex);
        for (auto& d : runtime->track.chain.devices) {
          if (d.kind != daw::DeviceKind::Sampler ||
              (h.deviceId != 0 && d.id != h.deviceId)) {
            continue;
          }
          ensureDefaultModSet(d.sampler, h.modSetId);
          for (auto& ms : d.sampler.modSets) {
            if (h.modSetId != 0 && ms.id != h.modSetId) {
              continue;
            }
            daw::SamplerModulator* mod = nullptr;
            if ((h.flags & daw::kSamplerEnvByTarget) != 0) {
              mod = findOrMintEnvelope(
                  ms, static_cast<daw::ModTarget>(std::min<uint8_t>(h.target, 4)));
            } else {
              for (auto& m : ms.modulators) {
                if (m.id == h.modulatorId) {
                  mod = &m;
                  break;
                }
              }
            }
            if (mod == nullptr) {
              break;
            }
            mod->kind = daw::ModKind::Envelope;
            mod->env = shape;
            mod->timeBase = h.timeBase != 0 ? 1 : 0;
            mod->rateMilli = static_cast<uint16_t>(
                std::clamp<int32_t>(h.rateMilli == 0 ? 1000 : h.rateMilli, 250, 4000));
            // THE PENCIL, explicitly — and it never flips back to the sliders on its own.
            mod->editor = 1;
            // Enforces the one invariant that is not the caller's job to remember: a release
            // loop must have a non-zero releaseFade, or the envelope never finishes, the voice
            // never frees, and the leak is silent.
            //
            // REPORTED, never silent. repairEnvShape's own comment says the caller fires this,
            // and it is right: a clamped envelope is a sound the user cannot explain, and a
            // drawn shape is exactly where a bad index or a backwards time arrives. Discarding
            // the result would turn "your release loop was out of range and I removed it" into
            // "it does not sound like I drew it".
            repair = daw::repairEnvShape(mod->env);
            targetId = mod->id;
            applied = true;
            break;
          }
          if (applied) {
            break;
          }
        }
        if (applied) {
          refreshSamplerForTrack(*runtime);
        }
      }
      if (!applied) {
        reportSamplerReject(daw::UiCommandType::SamplerSetEnvelopePoints, daw::UiSamplerRejectReason::NoSuchModSet,
                            h.trackId, 0, static_cast<uint16_t>(h.modSetId));
        DAW_EVENT("sampler.envelope_rejected")
            .field("track", h.trackId)
            .field("mod_set", h.modSetId)
            .field("reason", "no_such_mod_set_or_modulator");
        return;
      }
      DAW_EVENT("sampler.envelope_points_set")
          .field("track", h.trackId)
          .field("modulator", static_cast<uint32_t>(targetId))
          .field("points", static_cast<uint32_t>(h.pointCount))
          .field("bytes", static_cast<uint64_t>(buf.size()));
      if (repair.any()) {
        DAW_EVENT("sampler.envelope_repaired")
            .field("track", h.trackId)
            .field("modulator", static_cast<uint32_t>(targetId))
            .field("reordered", repair.reorderedPoints)
            .field("dropped", repair.droppedPoints)
            .field("cleared_sustain_loop", repair.clearedSustainLoop ? 1u : 0u)
            .field("cleared_release_loop", repair.clearedReleaseLoop ? 1u : 0u)
            .field("swapped_sustain_loop", repair.swappedSustainLoop ? 1u : 0u)
            .field("swapped_release_loop", repair.swappedReleaseLoop ? 1u : 0u)
            .field("added_release_fade", repair.addedReleaseFade ? 1u : 0u);
      }
      return;
    }

    // ---- SAMPLER SET SLOT NAME (90). Task #110: the name was persisted by the project format,
    // published by nothing and written by nothing but the loader stamping a path onto it.
    if (innerType == daw::UiCommandType::SamplerSetSlotName) {
      if (buf.size() < sizeof(daw::UiSamplerSlotNameHeader)) {
        DAW_EVENT("bulk.rejected")
            .field("op", static_cast<uint32_t>(inner))
            .field("bytes", static_cast<uint64_t>(buf.size()))
            .field("reason", "short_header");
        return;
      }
      daw::UiSamplerSlotNameHeader h{};
      std::memcpy(&h, buf.data(), sizeof(h));
      const size_t need = sizeof(h) + static_cast<size_t>(h.nameBytes);
      if (buf.size() < need) {
        DAW_EVENT("bulk.rejected")
            .field("op", static_cast<uint32_t>(inner))
            .field("bytes", static_cast<uint64_t>(buf.size()))
            .field("need", static_cast<uint64_t>(need))
            .field("reason", "short_payload");
        return;
      }
      // REFUSED, NEVER TRUNCATED — the one rule this command has. A name that does not fit the
      // published field would round-trip through save and reload intact and read back short, so
      // the file and the screen would disagree forever and nothing would report it. Refusing on
      // BYTE length also means no multi-byte character is ever cut in half, because nothing is
      // ever cut.
      if (h.nameBytes >= daw::kUiSamplerSlotNameBytes) {
        reportSamplerReject(daw::UiCommandType::SamplerSetSlotName,
                            daw::UiSamplerRejectReason::BadValue, h.trackId, h.deviceId,
                            h.slotId);
        DAW_EVENT("sampler.slot_name_rejected")
            .field("track", h.trackId)
            .field("slot", static_cast<uint32_t>(h.slotId))
            .field("bytes", static_cast<uint32_t>(h.nameBytes))
            .field("max", daw::kUiSamplerSlotNameBytes - 1)
            .field("reason", "name_not_representable");
        return;
      }
      const char* nameStart = reinterpret_cast<const char*>(buf.data()) + sizeof(h);
      const std::string newName(nameStart, h.nameBytes);
      // An embedded NUL is the same disagreement by another route: the project format would keep
      // the whole string and the published field would stop at the NUL.
      if (newName.find('\0') != std::string::npos) {
        reportSamplerReject(daw::UiCommandType::SamplerSetSlotName,
                            daw::UiSamplerRejectReason::BadValue, h.trackId, h.deviceId,
                            h.slotId);
        DAW_EVENT("sampler.slot_name_rejected")
            .field("track", h.trackId)
            .field("slot", static_cast<uint32_t>(h.slotId))
            .field("reason", "embedded_nul");
        return;
      }
      TrackRuntime* runtime = daw::engine::trackAt(tracks, tracksMutex, h.trackId);
      if (!runtime) {
        reportSamplerReject(daw::UiCommandType::SamplerSetSlotName,
                            daw::UiSamplerRejectReason::NoSuchTrack, h.trackId, h.deviceId,
                            h.slotId);
        DAW_EVENT("sampler.slot_name_rejected")
            .field("track", h.trackId)
            .field("reason", "no_such_track");
        return;
      }
      bool applied = false;
      bool sawSampler = false;
      {
        std::lock_guard<std::mutex> lock(runtime->trackMutex);
        for (auto& d : runtime->track.chain.devices) {
          if (d.kind != daw::DeviceKind::Sampler ||
              (h.deviceId != 0 && d.id != h.deviceId)) {
            continue;
          }
          sawSampler = true;
          for (auto& sl : d.sampler.slots) {
            if (sl.id != h.slotId) {
              continue;
            }
            sl.name = newName;
            applied = true;
            break;
          }
          if (applied) {
            break;
          }
        }
        // THE PUBLISH READS THE RT SNAPSHOT, NOT THE MODEL. Without this the name would be
        // saved and the read-back would keep answering the old one — the exact trap opcode 88
        // fell into, one field along.
        if (applied) {
          refreshSamplerForTrack(*runtime);
        }
      }
      if (!applied) {
        reportSamplerReject(daw::UiCommandType::SamplerSetSlotName,
                            sawSampler ? daw::UiSamplerRejectReason::NoSuchSlot
                                       : daw::UiSamplerRejectReason::NoSuchDevice,
                            h.trackId, h.deviceId, h.slotId);
        DAW_EVENT("sampler.slot_name_rejected")
            .field("track", h.trackId)
            .field("device", static_cast<uint32_t>(h.deviceId))
            .field("slot", static_cast<uint32_t>(h.slotId))
            .field("reason", sawSampler ? "no_such_slot" : "no_such_sampler_device");
        return;
      }
      DAW_EVENT("sampler.slot_renamed")
          .field("track", h.trackId)
          .field("device", static_cast<uint32_t>(h.deviceId))
          .field("slot", static_cast<uint32_t>(h.slotId))
          .field("name", newName)
          .field("bytes", static_cast<uint32_t>(h.nameBytes));
      return;
    }

    // ---- SET CLIP TEXT (98). A clip's `name` and its audio `source_path` were the last two
    // GAPs in persisted_field_reach: both persisted, both published, both rendered, and neither
    // reachable from any surface. Both were GAPs for one reason — a string does not fit the
    // 40-byte ring payload — so both arrive here, over the carrier, rather than as two more
    // scalar opcodes that could not have carried them anyway.
    if (innerType == daw::UiCommandType::SetClipText) {
      if (buf.size() < sizeof(daw::UiClipTextHeader)) {
        DAW_EVENT("bulk.rejected")
            .field("op", static_cast<uint32_t>(inner))
            .field("bytes", static_cast<uint64_t>(buf.size()))
            .field("reason", "short_header");
        return;
      }
      daw::UiClipTextHeader h{};
      std::memcpy(&h, buf.data(), sizeof(h));
      const auto whichField = static_cast<daw::ClipTextField>(h.field);
      const char* fieldName = nullptr;
      switch (whichField) {
        case daw::ClipTextField::Name: fieldName = "name"; break;
        case daw::ClipTextField::SourcePath: fieldName = "source_path"; break;
      }
      if (fieldName == nullptr) {
        DAW_EVENT("clip_text.rejected")
            .field("track", h.trackId)
            .field("clip", h.clipId)
            .field("field", static_cast<uint64_t>(h.field))
            .field("reason", "no_field_named");
        return;
      }
      // REFUSED, not truncated — the carrier's whole point is that a partial delivery is
      // detectable, so accepting a short buffer here would undo it one layer up.
      if (buf.size() < sizeof(h) + static_cast<size_t>(h.textBytes)) {
        DAW_EVENT("clip_text.rejected")
            .field("track", h.trackId)
            .field("clip", h.clipId)
            .field("field", fieldName)
            .field("bytes", static_cast<uint64_t>(buf.size()))
            .field("need", static_cast<uint64_t>(sizeof(h) + h.textBytes))
            .field("reason", "short_payload");
        return;
      }
      const std::string text(reinterpret_cast<const char*>(buf.data()) + sizeof(h),
                             static_cast<size_t>(h.textBytes));
      // AN EMPTY NAME IS LEGAL (a clip with no name), AN EMPTY PATH IS NOT. Clearing a source
      // path would leave an audio clip that is still an audio clip and can never make a sound,
      // which is a broken state rather than an edit — deleting the clip is how you remove it.
      if (whichField == daw::ClipTextField::SourcePath && text.empty()) {
        DAW_EVENT("clip_text.rejected")
            .field("track", h.trackId)
            .field("clip", h.clipId)
            .field("field", fieldName)
            .field("reason", "empty_text");
        return;
      }
      // THE NAME IS REFUSED WHEN IT DOES NOT FIT THE PUBLISHED FIELD, which is what
      // SamplerSetSlotName does for pad names. Truncating would make the read-back disagree with
      // the write for every long name, silently, and "the engine never shortens" is a far easier
      // rule to rely on than a length the caller has to know.
      constexpr size_t kNameBytes = sizeof(daw::UiClipExtent::name);
      if (whichField == daw::ClipTextField::Name && text.size() >= kNameBytes) {
        DAW_EVENT("clip_text.rejected")
            .field("track", h.trackId)
            .field("clip", h.clipId)
            .field("field", fieldName)
            .field("bytes", static_cast<uint32_t>(text.size()))
            .field("max", static_cast<uint32_t>(kNameBytes - 1))
            .field("reason", "text_too_long");
        return;
      }
      if (!requireMatchingClipVersion(h.baseVersion, daw::UiCommandType::SetClipText,
                                      h.trackId)) {
        return;
      }
      // A RETARGET AT A FILE THAT IS NOT THERE IS REFUSED, and refused BEFORE anything is
      // written. rebuildAudioRender would otherwise accept it, log audio.decode_failed and
      // schedule nothing — leaving a clip that displays the new path and renders silence, which
      // is the "shows one thing, plays another" state this command exists to make impossible.
      //
      // The boundary, stated rather than overclaimed: this checks that the resolved path EXISTS.
      // A file that exists and fails to decode still surfaces as audio.decode_failed, because
      // proving decodability here means decoding twice.
      if (whichField == daw::ClipTextField::SourcePath) {
        const std::string resolved = resolveSourcePath(text);
        std::error_code ec;
        if (!std::filesystem::exists(resolved, ec)) {
          DAW_EVENT("clip_text.rejected")
              .field("track", h.trackId)
              .field("clip", h.clipId)
              .field("field", fieldName)
              .field("path", resolved)
              .field("reason", "source_unreadable");
          return;
        }
      }

      TrackRuntime* runtime = daw::engine::trackAt(tracks, tracksMutex, h.trackId);
      if (!runtime) {
        DAW_EVENT("clip_text.rejected")
            .field("track", h.trackId)
            .field("clip", h.clipId)
            .field("field", fieldName)
            .field("reason", "no_such_track");
        return;
      }
      bool applied = false;
      bool wrongKind = false;
      std::shared_ptr<const ClipSnapshot> snapshot;
      {
        std::lock_guard<std::mutex> lock(runtime->trackMutex);
        for (auto& oc : runtime->ownedClips) {
          if (oc.id != h.clipId) {
            continue;
          }
          // A SYMBOLIC CLIP HAS NO SOURCE PATH. Accepting one would write a field the save path
          // never emits for this kind and the renderer never reads — the same "succeeded and
          // nothing happened" that opcode 95 names rather than ignores. A NAME, by contrast, is
          // common to both kinds, so it is not gated here.
          if (whichField == daw::ClipTextField::SourcePath &&
              oc.kind != daw::ClipKind::Audio) {
            wrongKind = true;
            break;
          }
          if (whichField == daw::ClipTextField::Name) {
            oc.name = text;
          } else {
            oc.audio.sourcePath = text;
          }
          applied = true;
          break;
        }
        // THE TWO FIELDS DO NOT SHARE A PUBLISHER. A name reaches readers through
        // UiClipExtent::name, and rt.clipExtents is DERIVED inside rebuildFlatAndPublish; a
        // source path reaches them through UiAudioSource::path and the render. Calling one
        // publisher for both fields is the "saved correctly and drew nothing" failure this file
        // already documents for the placement-scope flag.
        if (applied) {
          if (whichField == daw::ClipTextField::Name) {
            snapshot = rebuildFlatAndPublish(*runtime);
          } else {
            std::atomic_store_explicit(&runtime->audioRender, rebuildAudioRender(*runtime),
                                       std::memory_order_release);
          }
        }
      }
      if (wrongKind) {
        DAW_EVENT("clip_text.rejected")
            .field("track", h.trackId)
            .field("clip", h.clipId)
            .field("field", fieldName)
            .field("reason", "not_an_audio_clip");
        return;
      }
      if (!applied) {
        DAW_EVENT("clip_text.rejected")
            .field("track", h.trackId)
            .field("clip", h.clipId)
            .field("field", fieldName)
            .field("reason", "no_such_clip");
        return;
      }
      if (snapshot) {
        std::atomic_store_explicit(&runtime->clipSnapshot, snapshot,
                                   std::memory_order_release);
      }
      if (whichField == daw::ClipTextField::Name) {
        // The extents rebuild on the clip version, so bump it or the new name stays invisible
        // until some unrelated edit happens to republish.
        bumpClipVersionFor(runtime);
        clipDirty.store(true, std::memory_order_release);
      } else {
        publishAudioClipTable();
      }
      DAW_EVENT("clip_text.set")
          .field("track", h.trackId)
          .field("clip", h.clipId)
          .field("field", fieldName)
          .field("text", text)
          .field("bytes", static_cast<uint32_t>(text.size()));
      return;
    }

    DAW_EVENT("bulk.rejected")
        .field("op", static_cast<uint32_t>(inner))
        .field("bytes", static_cast<uint64_t>(buf.size()))
        .field("reason", "unknown_inner_op");
}

}  // namespace daw::engine
