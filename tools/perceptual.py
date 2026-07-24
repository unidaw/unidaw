#!/usr/bin/env python3
# perceptual.py — objective metrics mapped to the listener's vocabulary, so the
# engine can be checked without ears. Adapted from harp-fx/harness/perceptual2.py.
#
# Pairs with the engine's capture tap:
#   DAW_CAPTURE_WAV=/tmp/take.wav ./daw_engine
#   tools/perceptual.py /tmp/take.wav
#
# Accepts several files, and --expect-silence / --expect-audio to make it usable
# as a test assertion rather than only as a readout.
#
# One line per file:
#   rough   : sensory dissonance (Sethares). HIGH = harsh/buzzy.
#   flux    : spectral change over time. ~0 = static; higher = moving.
#   peaks   : prominent partials. LOW = thin/simple; HIGH = rich.
#   cent    : spectral centroid (Hz). LOW = dull/dark; HIGH = bright/tinny.
#   low%    : energy fraction 90–350 Hz. HIGH (with low cent) = muddy/boomy.
#   wob     : intra-note dominant-pitch jitter (cents). HIGH = wobbly.
#   move    : amplitude-envelope modulation energy 1–12 Hz (tremolo/evolution).
#   ends    : last non-silent second (cutoff detector).  clip% / peak : level.
import sys, wave, numpy as np

args = [a for a in sys.argv[1:] if not a.startswith('--')]
flags = {a for a in sys.argv[1:] if a.startswith('--')}
if not args:
    print("usage: perceptual.py [--expect-audio|--expect-silence] file.wav ...",
          file=sys.stderr)
    raise SystemExit(2)

def analyse(path):
  w = wave.open(path, 'rb')
  sr = w.getframerate(); n = w.getnframes(); ch = w.getnchannels()
  d = np.frombuffer(w.readframes(n), dtype=np.int16).astype(np.float64) / 32768.0
  if ch > 1: d = d.reshape(-1, ch).mean(axis=1)
  name = path.split('/')[-1].replace('.wav', '')
  if len(d) == 0:
    print(f"{name:18s} EMPTY")
    return 0.0

  N, H = 4096, 1024
  win = np.hanning(N)
  S = np.array([np.abs(np.fft.rfft(d[i:i+N] * win)) for i in range(0, max(len(d)-N, 1), H)])
  freqs = np.fft.rfftfreq(N, 1.0 / sr)
  avg = S.mean(axis=0) + 1e-12

  # flux
  diff = np.diff(S, axis=0)
  flux = float((np.sqrt((np.maximum(diff, 0)**2).sum(axis=1)) / (S[:-1].sum(axis=1)+1e-9)).mean())

  # richness + brightness
  p = avg / avg.max()
  peaks = int(sum(1 for k in range(2, len(p)-2) if p[k] > 0.04 and p[k] > p[k-1] and p[k] >= p[k+1] and p[k] > p[k-2] and p[k] > p[k+2]))
  tot = avg.sum() + 1e-20
  cent = float((freqs * avg).sum() / tot)
  low = float(avg[(freqs >= 90) & (freqs < 350)].sum() / tot * 100)

  # roughness (Sethares over top partials)
  idx = np.argsort(avg)[-60:]; pf, pa = freqs[idx], avg[idx] / avg[idx].max()
  rough = 0.0
  for i in range(len(pf)):
      for j in range(i+1, len(pf)):
          f1, f2 = sorted((pf[i], pf[j]))
          if f1 < 20: continue
          df = f2 - f1; s = 0.24 / (0.0207*f1 + 18.96)
          rough += pa[i]*pa[j]*(np.exp(-3.5*s*df) - np.exp(-5.75*s*df))
  rough = float(rough)

  # wobble: dominant melodic-band pitch per frame, jitter within ~90 ms windows
  band = (freqs >= 80) & (freqs <= 2500)
  fb = freqs[band]
  pitch = []
  for row in S:
      r = row[band]
      if r.max() < 1e-6: pitch.append(np.nan); continue
      k = int(np.argmax(r))
      if 0 < k < len(r)-1:                       # parabolic sub-bin refine
          a0, b0, c0 = r[k-1], r[k], r[k+1]
          off = 0.5*(a0-c0)/(a0-2*b0+c0+1e-12)
          f = fb[k] + off*(fb[1]-fb[0])
      else: f = fb[k]
      pitch.append(f)
  pitch = np.array(pitch)
  fps = sr / H; wlen = max(int(0.09*fps), 2)
  wobs = []
  for i in range(0, len(pitch)-wlen, wlen//2 or 1):
      seg = pitch[i:i+wlen]; seg = seg[~np.isnan(seg)]
      if len(seg) > 2 and np.median(seg) > 0:
          wobs.append(np.std(1200*np.log2(seg/np.median(seg))))
  wob = float(np.median(wobs)) if wobs else 0.0

  # movement: amplitude-envelope modulation energy 1–12 Hz
  hop = 64; ne = len(d)//hop
  env = np.sqrt((d[:ne*hop].reshape(ne, hop)**2).mean(axis=1)); env -= env.mean()
  E = np.abs(np.fft.rfft(env*np.hanning(len(env)))); ef = np.fft.rfftfreq(len(env), hop/sr)
  move = float(E[(ef >= 1) & (ef <= 12)].sum() / (E.sum()+1e-9) * 100)

  # level / cutoff
  clip = float(np.mean(np.abs(d) >= 0.999)*100); pk = float(np.abs(d).max())
  nz = np.where(np.abs(d) > 0.01)[0]; ends = float(nz[-1]/sr) if len(nz) else 0.0

  print(f"{name:18s} rough {rough:6.2f} flux {flux:.4f} peaks {peaks:4d} cent {cent:5.0f} "
        f"low {low:4.1f}% wob {wob:4.1f}c move {move:4.1f}% | ends {ends:5.1f}s clip {clip:.2f}% pk {pk:.2f}")

  return pk

worst = 0.0
for path in args:
  worst = max(worst, analyse(path))

# Assertion modes, so this can gate a test rather than only inform a human.
if '--expect-audio' in flags and worst < 0.001:
  print("FAIL: expected audio, got silence", file=sys.stderr)
  raise SystemExit(1)
if '--expect-silence' in flags and worst >= 0.001:
  print(f"FAIL: expected silence, got peak {worst:.3f}", file=sys.stderr)
  raise SystemExit(1)
