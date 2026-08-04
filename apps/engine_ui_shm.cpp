#include "engine_ui_shm.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cmath>
#include <cstring>
#include <iostream>

#include "engine_instance.h"
#include "event_log.h"
#include "scale_library.h"
#include "shared_memory.h"

namespace daw::engine {

int setUpUiShm(UiShmState& uiShm,
               const daw::HostConfig& baseConfig,
               uint32_t uiDiffRingCapacity) {
  uiShm.name = uiShmName();
  daw::LogLine() << "UI SHM name (engine): " << uiShm.name << std::endl;
  ::shm_unlink(uiShm.name.c_str());
  uiShm.fd = ::shm_open(uiShm.name.c_str(), O_CREAT | O_RDWR, 0600);
  if (uiShm.fd < 0) {
    daw::LogLine() << "Failed to create UI SHM: " << uiShm.name << std::endl;
    return 1;
  }

  {
    daw::ShmHeader header{};
    header.blockSize = baseConfig.blockSize;
    header.sampleRate = baseConfig.sampleRate;
    header.numChannelsIn = 0;
    header.numChannelsOut = 0;
    header.numBlocks = 0;
    header.channelStrideBytes = 0;
    size_t offset = daw::alignUp(sizeof(daw::ShmHeader), 64);
    header.audioInOffset = offset;
    header.audioOutOffset = offset;
    header.ringStdOffset = offset;
    offset += daw::alignUp(daw::ringBytes(0), 64);
    header.ringCtrlOffset = offset;
    offset += daw::alignUp(daw::ringBytes(0), 64);
    header.ringUiOffset = offset;
    offset += daw::alignUp(daw::ringBytes(baseConfig.ringUiCapacity), 64);
    header.ringUiOutOffset = offset;
    offset += daw::alignUp(daw::ringBytes(uiDiffRingCapacity), 64);
    header.ringUiEditOffset = offset;
    offset += daw::alignUp(
        daw::ringBytesForEntrySize(daw::kUiEditBatchCapacity,
                                   sizeof(daw::UiEditBatchEntry)),
        64);
    header.mailboxOffset = offset;
    offset += daw::alignUp(sizeof(daw::BlockMailbox), 64);
    header.uiClipOffset = offset;
    header.uiClipBytes = sizeof(daw::UiClipWindowSnapshot);
    offset += daw::alignUp(header.uiClipBytes, 64);
    header.uiHarmonyOffset = offset;
    header.uiHarmonyBytes = sizeof(daw::UiHarmonySnapshot);
    offset += daw::alignUp(header.uiHarmonyBytes, 64);
    // v9: all-tracks published clip snapshot (one window per track) and a second
    // command ring dedicated to the in-app agent.
    header.uiClipAllOffset = offset;
    header.uiClipAllBytes =
        sizeof(daw::UiClipWindowSnapshot) * daw::kUiMaxTracks;
    offset += daw::alignUp(header.uiClipAllBytes, 64);
    header.ringUiAgentOffset = offset;
    offset += daw::alignUp(daw::ringBytes(baseConfig.ringUiCapacity), 64);
    header.uiClipExtentOffset = offset;  // v11: clip-extents region (rails)
    offset += daw::alignUp(sizeof(daw::UiClipExtentRegion), 64);
    header.uiPatcherOffset = offset;  // v14: published patcher graph
    offset += daw::alignUp(sizeof(daw::UiPatcherRegion), 64);
    header.uiArrangeOffset = offset;  // v27: section spine + meter map, resolved
    header.uiArrangeBytes = sizeof(daw::UiArrangeSummaryRegion);
    offset += daw::alignUp(header.uiArrangeBytes, 64);
    header.uiAutomationOffset = offset;  // v28: which params are automated (standing list)
    header.uiAutomationBytes = sizeof(daw::UiAutomationLaneRegion);
    offset += daw::alignUp(header.uiAutomationBytes, 64);
    header.uiAutomationSlotOffset = offset;  // v28: answered point queries (seqlock slots)
    header.uiAutomationSlotBytes = sizeof(daw::UiAutomationSlotRegion);
    offset += daw::alignUp(header.uiAutomationSlotBytes, 64);
    header.uiDeviceMeterOffset = offset;  // v24: per-insert meters
    offset += daw::alignUp(sizeof(daw::UiDeviceMeterRegion), 64);
    header.uiScalesOffset = offset;  // v16: scale registry read-back
    offset += daw::alignUp(sizeof(daw::UiScaleRegion), 64);
    header.uiDeviceParamsOffset = offset;  // v17: one device's params (on request)
    offset += daw::alignUp(sizeof(daw::UiDeviceParamsRegion), 64);
    header.uiAudioSourceOffset = offset;   // v18: audio source/clip metadata table
    offset += daw::alignUp(sizeof(daw::UiAudioSourceRegion), 64);
    header.uiWaveformOffset = offset;      // v18: windowed waveform answer slots
    offset += daw::alignUp(sizeof(daw::UiWaveformRegion), 64);
    header.uiSamplerKitOffset = offset;    // v32: one sampler device's kit, on request
    header.uiSamplerKitBytes = sizeof(daw::UiSamplerKitRegion);
    offset += daw::alignUp(header.uiSamplerKitBytes, 64);
    header.uiSamplerEnvelopeOffset = offset;  // v37: one modulator's envelope shape, on request
    header.uiSamplerEnvelopeBytes = sizeof(daw::UiSamplerEnvelopeRegion);
    offset += daw::alignUp(header.uiSamplerEnvelopeBytes, 64);
    uiShm.size = daw::alignUp(offset, 64);

    if (::ftruncate(uiShm.fd, static_cast<off_t>(uiShm.size)) != 0) {
      daw::LogLine() << "Failed to size UI SHM: " << uiShm.name << std::endl;
      return 1;
    }
    daw::LogLine() << "UI SHM name: " << uiShm.name
              << " size: " << uiShm.size << std::endl;
    uiShm.base = ::mmap(nullptr, uiShm.size, PROT_READ | PROT_WRITE,
                        MAP_SHARED, uiShm.fd, 0);
    if (uiShm.base == MAP_FAILED) {
      uiShm.base = nullptr;
      daw::LogLine() << "Failed to map UI SHM: " << uiShm.name << std::endl;
      return 1;
    }
    daw::LogLine() << "UI SHM mapped: " << uiShm.name << std::endl;
    std::memset(uiShm.base, 0, uiShm.size);
    std::memcpy(uiShm.base, &header, sizeof(header));
    uiShm.header = reinterpret_cast<daw::ShmHeader*>(uiShm.base);
    uiShm.header->uiVersion.store(0, std::memory_order_release);
    uiShm.header->uiClipVersion = 0;
    uiShm.header->uiHarmonyVersion = 0;

    // v16: publish the scale registry once — it is static, so the harmony + tuning
    // UI reads it after attach and never needs an update. Cents in milli-cents.
    if (uiShm.header->uiScalesOffset != 0) {
      auto* region = reinterpret_cast<daw::UiScaleRegion*>(
          reinterpret_cast<uint8_t*>(uiShm.base) + uiShm.header->uiScalesOffset);
      const auto& scales = daw::ScaleRegistry::instance().scales();
      uint32_t count = 0;
      for (const auto& scale : scales) {
        if (count >= daw::kUiMaxScales) {
          break;
        }
        daw::UiScale& out = region->scales[count++];
        out.id = scale.id;
        out.octaveMilliCents =
            static_cast<int32_t>(std::llround(daw::intervalToCents(scale.octave) * 1000.0));
        std::memset(out.name, 0, sizeof(out.name));
        std::memcpy(out.name, scale.name.data(),
                    std::min(scale.name.size(), sizeof(out.name) - 1));
        const uint32_t steps = static_cast<uint32_t>(
            std::min<size_t>(scale.steps.size(), daw::kUiMaxScaleSteps));
        out.stepCount = steps;
        for (uint32_t i = 0; i < steps; ++i) {
          out.stepMilliCents[i] = static_cast<int32_t>(
              std::llround(daw::intervalToCents(scale.steps[i]) * 1000.0));
        }
      }
      region->scaleCount = count;
      region->version = 1;
    }

    // v18: initialise the waveform region headers once. The source/clip tables are
    // filled on project load (rebuildAudioRender); the slots are written on request.
    if (uiShm.header->uiAudioSourceOffset != 0) {
      auto* region = reinterpret_cast<daw::UiAudioSourceRegion*>(
          reinterpret_cast<uint8_t*>(uiShm.base) + uiShm.header->uiAudioSourceOffset);
      region->formatVersion = daw::kWaveformFormatVersion;
      region->version = 0;
    }
    if (uiShm.header->uiWaveformOffset != 0) {
      auto* region = reinterpret_cast<daw::UiWaveformRegion*>(
          reinterpret_cast<uint8_t*>(uiShm.base) + uiShm.header->uiWaveformOffset);
      region->slotCount = daw::kUiWaveformSlots;
    }

    auto* ringUi = reinterpret_cast<daw::RingHeader*>(
        reinterpret_cast<uint8_t*>(uiShm.base) + header.ringUiOffset);
    ringUi->capacity = baseConfig.ringUiCapacity;
    ringUi->entrySize = sizeof(daw::EventEntry);
    ringUi->readIndex.store(0);
    ringUi->writeIndex.store(0);

    // v9: the agent's own SPSC command ring, drained by the same consumer as the
    // UI ring. base_version optimistic concurrency arbitrates edits across rings.
    auto* ringUiAgent = reinterpret_cast<daw::RingHeader*>(
        reinterpret_cast<uint8_t*>(uiShm.base) + header.ringUiAgentOffset);
    ringUiAgent->capacity = baseConfig.ringUiCapacity;
    ringUiAgent->entrySize = sizeof(daw::EventEntry);
    ringUiAgent->readIndex.store(0);
    ringUiAgent->writeIndex.store(0);

    auto* ringUiOut = reinterpret_cast<daw::RingHeader*>(
        reinterpret_cast<uint8_t*>(uiShm.base) + header.ringUiOutOffset);
    ringUiOut->capacity = uiDiffRingCapacity;
    ringUiOut->entrySize = sizeof(daw::EventEntry);
    ringUiOut->readIndex.store(0);
    ringUiOut->writeIndex.store(0);

    auto* ringUiEdit = reinterpret_cast<daw::RingHeader*>(
        reinterpret_cast<uint8_t*>(uiShm.base) + header.ringUiEditOffset);
    ringUiEdit->capacity = daw::kUiEditBatchCapacity;
    ringUiEdit->entrySize = sizeof(daw::UiEditBatchEntry);
    ringUiEdit->readIndex.store(0);
    ringUiEdit->writeIndex.store(0);

    daw::LogLine() << "UI rings ready (ui_offset=" << header.ringUiOffset
              << ", ui_capacity=" << ringUi->capacity
              << ", ui_entry_size=" << ringUi->entrySize
              << ", ui_out_offset=" << header.ringUiOutOffset
              << ", ui_out_capacity=" << ringUiOut->capacity
              << ", ui_edit_offset=" << header.ringUiEditOffset
              << ", ui_edit_capacity=" << ringUiEdit->capacity << ")"
              << std::endl;
  }

  return 0;
}

}  // namespace daw::engine
