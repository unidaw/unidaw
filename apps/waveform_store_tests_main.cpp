// Tests for the waveform source registry: the content-key names exactly its inputs
// and is stable, interning dedups a path within a load and survives a re-bounce,
// beginLoad drops the previous project, and a ready source is never downgraded to
// failed. Header-only, no deps.
#include "apps/waveform_store.h"
#include "apps/shared_memory.h"  // kWaveformFormatVersion (the store stays SHM-agnostic)

#include <cstdio>
#include <memory>

using namespace daw;

static int g_fail = 0;
#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
      ++g_fail;                                                           \
    }                                                                     \
  } while (0)

static std::shared_ptr<const WaveformPyramid> makePyramid() {
  std::vector<float> ch(128, 0.5f);
  const float* chans[1] = {ch.data()};
  return std::make_shared<const WaveformPyramid>(
      buildWaveformPyramid(chans, 1, 128, 64));
}

int main() {
  // The content key is a pure function of its named inputs, and every input moves it.
  const uint64_t base = computeWaveformContentKey("/a.wav", 100, 200, 300, 44100.0,
                                                  2, kDecoderVersion,
                                                  kWaveformFormatVersion);
  CHECK(base == computeWaveformContentKey("/a.wav", 100, 200, 300, 44100.0, 2,
                                          kDecoderVersion,
                                          kWaveformFormatVersion));  // stable
  CHECK(base != computeWaveformContentKey("/b.wav", 100, 200, 300, 44100.0, 2,
                                          kDecoderVersion, kWaveformFormatVersion));
  CHECK(base != computeWaveformContentKey("/a.wav", 101, 200, 300, 44100.0, 2,
                                          kDecoderVersion, kWaveformFormatVersion));
  CHECK(base != computeWaveformContentKey("/a.wav", 100, 201, 300, 44100.0, 2,
                                          kDecoderVersion, kWaveformFormatVersion));
  CHECK(base != computeWaveformContentKey("/a.wav", 100, 200, 301, 44100.0, 2,
                                          kDecoderVersion, kWaveformFormatVersion));
  CHECK(base != computeWaveformContentKey("/a.wav", 100, 200, 300, 48000.0, 2,
                                          kDecoderVersion, kWaveformFormatVersion));
  CHECK(base != computeWaveformContentKey("/a.wav", 100, 200, 300, 44100.0, 1,
                                          kDecoderVersion, kWaveformFormatVersion));
  CHECK(base != computeWaveformContentKey("/a.wav", 100, 200, 300, 44100.0, 2,
                                          kDecoderVersion + 1,
                                          kWaveformFormatVersion));
  CHECK(base != computeWaveformContentKey("/a.wav", 100, 200, 300, 44100.0, 2,
                                          kDecoderVersion,
                                          kWaveformFormatVersion + 1));

  WaveformStore store;

  // Interning one path twice within a load returns the same id (dedup); a distinct
  // path gets a distinct id.
  const uint32_t idA = store.internReady("/a.wav", base, 2, 300, 44100.0, 1.0f, 0x3c1,
                                         false, false, makePyramid());
  const uint32_t idA2 = store.internReady("/a.wav", base, 2, 300, 44100.0, 1.0f,
                                          0x3c1, false, false, makePyramid());
  const uint32_t idB = store.internReady("/b.wav", base ^ 1ull, 1, 128, 48000.0, 0.5f,
                                         0x1, false, false, makePyramid());
  CHECK(idA == idA2);
  CHECK(idA != idB);
  CHECK(store.sourceIdForPath("/a.wav") == idA);
  CHECK(store.sourceIdForPath("/b.wav") == idB);
  CHECK(store.sourceIdForPath("/nope.wav") == 0);

  // A re-bounce in place (same path, new content key) keeps the id, updates the key.
  const uint64_t rekey = base + 999;
  const uint32_t idA3 = store.internReady("/a.wav", rekey, 2, 300, 44100.0, 1.0f,
                                          0x3c1, false, false, makePyramid());
  CHECK(idA3 == idA);
  WaveformSourceEntry e{};
  CHECK(store.lookup(idA, e) && e.contentKey == rekey);

  // lookup returns metadata + a live pyramid; an unknown id fails.
  CHECK(e.status == kWaveformStatusReady && e.pyramid != nullptr);
  CHECK(e.waveChannels == 1 && e.sourceChannels == 2);
  WaveformSourceEntry miss{};
  CHECK(!store.lookup(99999, miss));

  // A failed decode gets a descriptor with the resolved path; a ready source is
  // never downgraded to failed by a later racing miss.
  const uint32_t idC = store.internFailed("/missing.wav");
  WaveformSourceEntry ec{};
  CHECK(store.lookup(idC, ec) && ec.status == kWaveformStatusFailed);
  CHECK(ec.path == "/missing.wav" && ec.pyramid == nullptr);
  store.internFailed("/a.wav");  // a.wav already ready — must stay ready
  WaveformSourceEntry ea{};
  CHECK(store.lookup(idA, ea) && ea.status == kWaveformStatusReady);

  // beginLoad drops the previous project's sources (and pyramids) and resets ids.
  store.beginLoad();
  CHECK(store.snapshot().empty());
  CHECK(store.sourceIdForPath("/a.wav") == 0);
  const uint32_t fresh = store.internReady("/x.wav", 42, 1, 64, 44100.0, 0.5f, 0x1,
                                           false, false, makePyramid());
  CHECK(fresh == 1);  // id allocator reset

  if (g_fail == 0) std::printf("waveform_store: all assertions passed\n");
  return g_fail == 0 ? 0 : 1;
}
