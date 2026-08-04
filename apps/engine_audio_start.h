#pragma once
// STARTING THE AUDIO DEVICE — the sequence between "the threads are up" and "sound comes out".
//
// It builds the callback, tells it what to play from and how deep the pipeline is, wires the
// master chain (which decides how wide the mix is, and therefore how wide an offline render is),
// starts the master render thread if the master has effects, and only then publishes the callback
// so the device may call it. The ORDER is the content: a callback published before the master
// chain exists would be called with a mix whose width nobody has decided yet.
//
// It runs on the online path only — main() calls it inside `if (!testMode)`, and the body no
// longer mentions testMode because the condition is the caller's.
#include <atomic>
#include <memory>
#include <thread>

#include "engine_audio_callback.h"
#include "engine_master_render.h"
#include "engine_transport_state.h"
#include "engine_types.h"
#include "platform_juce/juce_wrapper.h"

namespace daw::engine {

struct AudioStartDeps {
  std::unique_ptr<daw::IAudioBackend>& audioBackend;
  std::unique_ptr<EngineAudioCallback>& audioCallback;
  std::atomic<EngineAudioCallback*>& audioCallbackPublished;
  std::atomic<uint32_t>& audioPlaybackBlockId;
  std::unique_ptr<daw::IRuntime>& audioRuntime;
  const uint32_t effBlockSize;
  const int effOutChannels;
  const double effSampleRate;
  const daw::HostConfig& engineConfig;
  std::atomic<bool>& masterFxActive;
  // The master render thread is STARTED here and its deps are built ~1,300 lines earlier, next to
  // the state they refer to. Passing the struct rather than rebuilding it keeps both facts true.
  MasterRenderDeps& masterRenderDeps;
  std::thread& masterRenderThread;
  std::unique_ptr<TrackRuntime>& masterTrack;
  int& offlineChannels;  // the pump renders at the width the master chain turns out to be
  const bool offlineRender;
  TransportState& transport;
};

// Runs the whole start sequence. Leaves audioCallback non-null and published on success.
void startAudioDevice(AudioStartDeps& deps);

}  // namespace daw::engine
