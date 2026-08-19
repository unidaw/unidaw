#include "engine_audio_start.h"

#include <algorithm>
#include <iostream>

#include "engine_master_render.h"
#include "event_log.h"

namespace daw::engine {

void startAudioDevice(AudioStartDeps& deps) {
  auto& audioBackend = deps.audioBackend;
  auto& audioCallback = deps.audioCallback;
  auto& audioCallbackPublished = deps.audioCallbackPublished;
  auto& audioPlaybackBlockId = deps.audioPlaybackBlockId;
  auto& audioRuntime = deps.audioRuntime;
  auto& effBlockSize = deps.effBlockSize;
  auto& effOutChannels = deps.effOutChannels;
  auto& effSampleRate = deps.effSampleRate;
  auto& engineConfig = deps.engineConfig;
  auto& masterFxActive = deps.masterFxActive;
  auto& masterRenderDeps = deps.masterRenderDeps;
  auto& masterRenderThread = deps.masterRenderThread;
  auto& masterTrack = deps.masterTrack;
  auto& offlineChannels = deps.offlineChannels;
  auto& offlineRender = deps.offlineRender;
  auto& transport = deps.transport;

    // The runtime is created up where the device is OPENED, not here — see the comment there.
    // Kept as a fallback for the --no-audio path, which opens no device and so never made one.
    if (!audioRuntime) {
      audioRuntime = daw::createJuceRuntime();
    }
    // Opened earlier to adopt its sample rate; here we just wire the callback.
    //
    // OFFLINE takes this same branch with no device: it needs every bit of the setup below
    // (the callback, the master width, the master FX wiring and its render thread) and differs
    // only in what DRIVES it at the end — a pump instead of the device's callback. Hoisting
    // 200 lines of delicate master-FX wiring out of here to share it would have been the
    // riskier way to say the same thing.
    if (!audioBackend && !offlineRender) {
      daw::LogLine() << "No audio device; running without audio output" << std::endl;
    } else {
      std::cout << "Audio device: "
                << (audioBackend ? audioBackend->deviceName() : "(offline render)")
                << std::endl;
      std::cout << "  Sample rate: " << effSampleRate
                << " (engine now matches)" << std::endl;
      std::cout << "  Buffer size: " << effBlockSize
                << " (engine expects: " << engineConfig.blockSize << ")" << std::endl;
      audioCallback = std::make_unique<EngineAudioCallback>(
          effSampleRate,
          effBlockSize,
          engineConfig.numBlocks,
          &audioPlaybackBlockId);
      audioCallback->setPlaying(&transport.playing);
      // Movement 4 surround master: the mix width follows the device, but
      // DAW_MASTER_CHANNELS forces a wider (e.g. 5.1) master for placement + capture even
      // on a stereo device — the device just hears the downmixed front L/R. Determined
      // HERE, before the master FX wiring below, because the master host must be opened at
      // the master's true width: sized at a fixed 2 it could never match a surround mix,
      // and the gate would leave a master effect installed, hosted and inaudible.
      int masterChannels = std::max(2, effOutChannels);
      if (const char* mc = std::getenv("DAW_MASTER_CHANNELS")) {
        const int parsed = std::atoi(mc);
        if (parsed > masterChannels) {
          masterChannels = std::min(parsed, 8);
          audioCallback->setMasterChannels(masterChannels);
          std::cout << "Surround master: " << masterChannels
                    << " channels (device has " << effOutChannels
                    << ")" << std::endl;
        }
      }
      // Wire the master track's fader so its gain/mute controls the summed output
      // (patcher-is-a-device item 4a). The atomics live on masterTrack for its lifetime.
      if (masterTrack) {
        // Open the master host at the MIX's width, not a hardcoded stereo, so master FX
        // works on a surround master too. Its input IS the sum, so in == out.
        masterTrack->config.numChannelsOut = static_cast<uint32_t>(masterChannels);
        masterTrack->config.numChannelsIn = static_cast<uint32_t>(masterChannels);
        audioCallback->setMasterMixer(&masterTrack->mixGainLinear,
                                      &masterTrack->mixMute);
        // 4b: wire the master host's readiness + size the sum/processed hand-off buffers
        // to the master host's channel width.
        audioCallback->setMasterFxWiring(
            &masterFxActive, &masterTrack->hostReady,
            static_cast<uint32_t>(masterChannels),
            effBlockSize);
        // 4b render thread: one block behind the callback, it drives the master host —
        // take the summed block, write it to the host's input plane, process, read the
        // output plane, hand it back for the callback to emit next block. This is the ONLY
        // thing that blocks on the master host; the RT callback never does. Idle (a short
        // sleep) whenever there is no master effect or the host isn't ready.
        // PUBLISH. Everything above has finished touching the callback, so a thread that sees
        // this pointer sees a callback that is ready to be used. Release pairs with the acquire
        // in publishedCallback().
        audioCallbackPublished.store(audioCallback.get(), std::memory_order_release);

        if (!masterRenderThread.joinable()) {
          masterRenderThread =
              std::thread([&] { daw::engine::runMasterRenderThread(masterRenderDeps); });
        }
      }
      audioCallback->resetForStart();
        // DAW_SILENT_OUTPUT=1 silences the hardware and nothing else — the capture is taken before
        // it. ctest sets it for every test so a suite can run beside someone's music; a check run
        // by hand stays audible, which is what a human debugging one wants.
        if (const char* silent = std::getenv("DAW_SILENT_OUTPUT")) {
          if (*silent != '\0' && *silent != '0') {
            audioCallback->setSilentOutput(true);
            DAW_EVENT("audio.silent_output").field("source", std::string("DAW_SILENT_OUTPUT"));
          }
        }
      // DAW_CAPTURE_WAV=<path> records the master output so a take can be
      // analysed offline; DAW_CAPTURE_SECONDS bounds the preallocation.
      if (const char* capturePath = std::getenv("DAW_CAPTURE_WAV")) {
        if (*capturePath != '\0') {
          double seconds = 30.0;
          if (const char* secondsEnv = std::getenv("DAW_CAPTURE_SECONDS")) {
            const double parsed = std::atof(secondsEnv);
            if (parsed > 0.0) {
              seconds = parsed;
            }
          }
          const auto frames =
              static_cast<size_t>(effSampleRate * seconds);
          audioCallback->enableCapture(frames, masterChannels);
          DAW_EVENT("audio.capture_armed")
              .field("path", std::string(capturePath))
              .field("seconds", seconds);
        }
      }
      if (offlineRender) {
        // Offline: the callback is driven by a PUMP instead of the device, and the pump waits.
        // Wiring only — the loop itself runs after the threads are up (see the render section
        // further down), because it needs the producer and the hosts alive to feed it.
        audioCallback->setOfflineMode(true);
        offlineChannels = masterChannels;
        std::cout << "Offline render: " << effSampleRate << " Hz, " << effBlockSize
                  << " samples/block, " << masterChannels << " channels" << std::endl;
      } else if (audioBackend->start(
                     [&](float* const* outputs, int numChannels, int numFrames) {
                       audioCallback->process(outputs, numChannels, numFrames);
                     })) {
        // ASK THE DEVICE WHETHER IT IS RUNNING, rather than announcing it because a callback was
        // registered. `start()` returns true whenever it is handed a non-null callback, so this
        // line used to print on a machine where CoreAudio opens the device, reports its name,
        // rate and block size, and never runs a single callback — indistinguishable from a
        // working machine except in a summary at shutdown that nobody was reading. Both agents
        // spent time on "the app makes no sound" against that message.
        //
        // The device may take a moment to come up, so this samples rather than asking once: a
        // race lost here would print the alarming version on a machine that works, which is a
        // worse failure than the one being fixed.
        // WAIT FOR A CALLBACK, not for isPlaying(). The device's own isPlaying() answers TRUE on
        // a machine where CoreAudio never runs the IO proc — measured — so it cannot tell the two
        // cases apart, and the whole point of this check is to tell them apart. One real callback
        // is the only thing that proves the chain works end to end.
        //
        // Exits the moment the first callback lands, so a working device costs about one block
        // (~12 ms at 512/44100) and only a broken one waits the full second.
        bool running = false;
        for (int i = 0; i < 40 && !running; ++i) {
          std::this_thread::sleep_for(std::chrono::milliseconds(25));
          running = audioBackend->deviceCallbacks() > 0;
        }
        if (running) {
          std::cout << "Audio output started" << std::endl;
        } else {
          const std::string why = audioBackend->lastError();
          const int inputs = audioBackend->inputChannels();
          std::cout << "Audio output OPENED BUT NEVER STARTED on \""
                    << audioBackend->deviceName() << "\" — the device reported its rate and "
                    << "block size and is not playing, so nothing will be heard and every "
                    << "capture will be empty." << std::endl;
          if (inputs > 0) {
            std::cout << "  It was opened with " << inputs << " INPUT channel(s). On macOS an "
                      << "unanswered microphone permission stops the whole AudioUnit, output "
                      << "included; check System Settings > Privacy & Security > Microphone."
                      << std::endl;
          }
          if (!why.empty()) {
            std::cout << "  The device's own last error: " << why << std::endl;
          }
          DAW_EVENT("audio.device_not_running")
              .field("device", audioBackend->deviceName())
              .field("inputs", static_cast<uint64_t>(inputs))
              .field("error", why);
        }
      } else {
        daw::LogLine() << "Failed to start audio output" << std::endl;
      }
    }
}

}  // namespace daw::engine
