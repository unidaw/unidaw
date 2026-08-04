#include "engine_master_render.h"

#include <chrono>
#include <thread>

#include "event_log.h"

namespace daw::engine {

void runMasterRenderThread(MasterRenderDeps& deps) {
  auto& running = deps.running;
  auto& playing = deps.playing;
  auto& masterFxActive = deps.masterFxActive;
  auto& masterTrack = deps.masterTrack;
  auto& audioCallback = deps.audioCallback;
  auto& scheduleHostRestart = deps.scheduleHostRestart;


            const uint32_t chn = masterTrack->config.numChannelsOut;
            // Stride with the SAME block size the hand-off buffers were sized with. Using
            // engineConfig.blockSize here would smear channels the moment the device
            // buffer and the engine block size diverge.
            const uint32_t bs = audioCallback->masterFxBlockSize();
            std::vector<float> sumScratch;
            std::vector<float> outScratch(static_cast<size_t>(chn) * bs, 0.0f);
            uint32_t masterBlockId = 1;
            uint64_t masterSample = 0;
            uint32_t consecutiveTimeouts = 0;
            bool warnedNotConsumed = false;
            while (running.load(std::memory_order_acquire)) {
              if (!masterFxActive.load(std::memory_order_acquire) ||
                  !masterTrack->hostReady.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
              }
              if (!audioCallback || !audioCallback->takeMasterSum(sumScratch) ||
                  sumScratch.size() < static_cast<size_t>(chn) * bs) {
                std::this_thread::sleep_for(std::chrono::microseconds(200));
                continue;
              }
              bool sendFailed = false;
              bool timedOut = false;
              bool produced = false;
              {
                // Hold controllerMutex across the WHOLE host interaction. The restart
                // worker takes this same lock to call controller.launch(), which
                // disconnects and munmaps the very mapping this thread reads and writes —
                // touching header/mailbox unlocked is a use-after-munmap.
                std::lock_guard<std::mutex> lock(masterTrack->controllerMutex);
                const auto* header = masterTrack->controller.shmHeader();
                const auto* mailbox = masterTrack->controller.mailbox();
                const size_t shmSize = masterTrack->controller.shmSize();
                if (!header || !mailbox || header->numBlocks == 0 ||
                    header->channelStrideBytes == 0) {
                  sendFailed = true;
                } else {
                  const uint64_t stride = header->channelStrideBytes;
                  const uint32_t blockIndex = masterBlockId % header->numBlocks;
                  const uint64_t inBlockBytes =
                      static_cast<uint64_t>(header->numChannelsIn) * stride;
                  for (uint32_t ch = 0; ch < chn && ch < header->numChannelsIn; ++ch) {
                    const uint64_t off = header->audioInOffset +
                                         blockIndex * inBlockBytes +
                                         static_cast<uint64_t>(ch) * stride;
                    if (off + stride > shmSize) {
                      continue;
                    }
                    std::memcpy(reinterpret_cast<uint8_t*>(
                                    const_cast<daw::ShmHeader*>(header)) + off,
                                sumScratch.data() + static_cast<size_t>(ch) * bs,
                                bs * sizeof(float));
                  }
                  daw::HostTransport tr;
                  tr.isPlaying = playing.load(std::memory_order_acquire);
                  if (!masterTrack->controller.sendProcessBlock(
                          masterBlockId, masterSample, masterSample, 0, 0, tr)) {
                    sendFailed = true;
                  } else {
                    // Bounded wait for THIS block. A dead or wedged host must not hang
                    // the render thread.
                    const auto deadline = std::chrono::steady_clock::now() +
                                          std::chrono::milliseconds(50);
                    while (mailbox->completedBlockId.load(std::memory_order_acquire) <
                           masterBlockId) {
                      if (std::chrono::steady_clock::now() > deadline) {
                        timedOut = true;
                        break;
                      }
                      std::this_thread::sleep_for(std::chrono::microseconds(100));
                    }
                    // ONLY read the out plane when the host actually finished this block.
                    // On a timeout that slot still holds the block from numBlocks ago (or
                    // silence), and publishing it would present stale audio as fresh.
                    if (!timedOut) {
                      const uint64_t outBlockBytes =
                          static_cast<uint64_t>(header->numChannelsOut) * stride;
                      for (uint32_t ch = 0; ch < chn; ++ch) {
                        float* dst = outScratch.data() + static_cast<size_t>(ch) * bs;
                        if (ch < header->numChannelsOut) {
                          const uint64_t off = header->audioOutOffset +
                                               blockIndex * outBlockBytes +
                                               static_cast<uint64_t>(ch) * stride;
                          if (off + stride <= shmSize) {
                            std::memcpy(dst,
                                        reinterpret_cast<const uint8_t*>(header) + off,
                                        bs * sizeof(float));
                            continue;
                          }
                        }
                        std::fill(dst, dst + bs, 0.0f);
                      }
                      produced = true;
                    }
                  }
                }
              }
              if (sendFailed) {
                // The master is not in the tracks vector, so the consumer's periodic
                // re-arm never sees it: schedule its restart HERE or a dead master host
                // stays dead for the rest of the session.
                masterTrack->hostReady.store(false, std::memory_order_release);
                masterTrack->needsRestart.store(true, std::memory_order_release);
                scheduleHostRestart(*masterTrack);
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
              }
              if (timedOut) {
                if (++consecutiveTimeouts >= 10) {
                  daw::LogLine() << "Engine: master FX host is not completing blocks; "
                               "restarting it." << std::endl;
                  consecutiveTimeouts = 0;
                  masterTrack->hostReady.store(false, std::memory_order_release);
                  masterTrack->needsRestart.store(true, std::memory_order_release);
                  scheduleHostRestart(*masterTrack);
                }
                continue;
              }
              consecutiveTimeouts = 0;
              if (produced) {
                audioCallback->publishMasterOut(outScratch);
                ++masterBlockId;
                masterSample += bs;
                // If we are feeding the host but the callback never swaps our output in,
                // master FX is silently doing nothing (e.g. a surround master whose width
                // does not match the master host's, or a hand-off size mismatch). Say so
                // once rather than leaving an installed effect mysteriously inaudible.
                if (!warnedNotConsumed && masterBlockId > 200 &&
                    audioCallback->masterFxBlocks() == 0) {
                  warnedNotConsumed = true;
                  daw::LogLine() << "Engine: master FX is processing but the audio callback is "
                               "not using it — the master bus width does not match the "
                               "master host ("
                            << chn << " ch). The effect is installed but inaudible."
                            << std::endl;
                }
              }
            }
}

}  // namespace daw::engine
