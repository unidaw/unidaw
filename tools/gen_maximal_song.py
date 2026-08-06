#!/usr/bin/env python3
"""Generate a maximalist .uniproj.json that exercises every feature the format
carries, so the frontend can test the whole UI against one song.

A 4-bar A-minor electronic groove: Am - F - C - G. Exercises: tempo map,
harmony timeline, per-track lines_per_beat (16ths / triplets / sextuplets =
polyrhythm), mixer (gain/pan/mute), device chains (VSTi + a euclidean device),
mod links (LFO -> param), notes with all three row ops (retrigger / probability
/ delay) across multiple columns, chords (triad + seventh, spread + humanize),
and a per-track patcher DAG (euclidean/random_degree/event_out).
"""
import json, sys, os

Q = 960_000          # nanoticks per quarter
BAR = 4 * Q
SIX = Q // 4         # a 16th
# ZEBRALETTE, NOT ZEBRA2, and the path is unchanged because they are the SAME BUNDLE —
# Zebra2.vst3 holds Zebra2, Zebralette, ZRev and Zebrify, and the name is what tells them apart
# (plugin_cache matches on path AND name for exactly this reason). Zebralette is the free member:
# Zebra2 loads unlicensed here and logs "Could not read lic from file", which for a u-he plugin
# means it is free to degrade the audio at a time of its choosing. Not something to find out
# during a demo. generator.uniproj.json already referenced Zebralette; this was the outlier.
ZEBRA = {"vendor": "u-he", "name": "Zebralette",
         "path": "/Library/Audio/Plug-Ins/VST3/Zebra2.vst3", "uid16": ""}

_nid = 1000
def nid():
    global _nid; _nid += 1; return _nid

def note(tick, pitch, dur, vel=100, col=0, retrigger=None, probability=None, delay=None):
    n = {"nanotick": tick, "duration": dur, "pitch": pitch, "velocity": vel,
         "column": col, "note_id": nid()}
    if retrigger is not None: n["retrigger"] = retrigger
    if probability is not None: n["probability"] = probability
    if delay is not None: n["delay"] = delay
    return n

def chord(tick, degree, dur, quality=1, inversion=0, base_octave=4, col=0,
          spread=0, humanize_timing=0, humanize_velocity=0):
    global _cid
    _cid += 1
    return {"nanotick": tick, "duration": dur, "chord_id": _cid, "degree": degree,
            "quality": quality, "inversion": inversion, "base_octave": base_octave,
            "column": col, "spread": spread, "humanize_timing": humanize_timing,
            "humanize_velocity": humanize_velocity}
_cid = 0

def route(kind="none", track_id=0, input_id=0):
    return {"kind": kind, "track_id": track_id, "input_id": input_id}

def vsti(device_id=0):
    return {"device_id": device_id, "kind": "vst_instrument", "capability_mask": 5,
            "patcher_node_id": 0, "host_slot_index": 0, "bypass": False,
            "vst_ref": dict(ZEBRA)}

def euclid_device(device_id=1):
    return {"device_id": device_id, "kind": "vst_instrument", "capability_mask": 5,
            "patcher_node_id": 0, "host_slot_index": 0, "bypass": False,
            "vst_ref": dict(ZEBRA),
            "euclidean": {"steps": 16, "hits": 7, "offset": 0, "duration_ticks": SIX,
                          "degree": 0, "octave_offset": 0, "velocity": 90, "base_octave": 3}}

def patcher(*, euclidean=False, hits=5):
    """A small event DAG. euclidean -> random_degree -> event_out, or just
    random_degree -> event_out."""
    nodes, edges, nx = [], [], 0
    if euclidean:
        nodes.append({"id": 0, "type": "euclidean",
                      "euclidean": {"steps": 16, "hits": hits, "offset": 0,
                                    "duration_ticks": 0, "degree": 1, "octave_offset": 0,
                                    "velocity": 100, "base_octave": 4}})
        nx = 1
    rnd_id = nx
    nodes.append({"id": rnd_id, "type": "random_degree",
                  "random_degree": {"degree": 8, "velocity": 100, "duration_ticks": 0}})
    out_id = rnd_id + 1
    nodes.append({"id": out_id, "type": "event_out"})
    if euclidean:
        edges.append({"src_node_id": 0, "src_port_id": 1, "dst_node_id": rnd_id,
                      "dst_port_id": 0, "kind": "event"})
    edges.append({"src_node_id": rnd_id, "src_port_id": 1, "dst_node_id": out_id,
                  "dst_port_id": 0, "kind": "event"})
    return {"nodes": nodes, "edges": edges}

def track(track_id, name, *, lpb=4, quant=False, gain=0.0, pan=0.0, mute=False,
          solo=False, chain=None, notes=None, chords=None, mod_links=None, pat=None):
    t = {
        "track_id": track_id, "name": name, "harmony_quantize": quant,
        "lines_per_beat": lpb,
        "mixer": {"gain_db": gain, "pan": pan, "mute": mute, "solo": solo},
        "routing": {"midi_in": route(), "midi_out": route(),
                    "audio_in": route(), "audio_out": route("master"),
                    "pre_fader_send": True},
        "device_chain": chain if chain is not None else [vsti()],
        "mod_links": mod_links or [],
        "notes": notes or [],
        "chords": chords or [],
    }
    if pat is not None:
        t["patcher"] = pat
    return t

# --- Harmony: Am - F - C - G, one per bar (root pitch-class, scale id) ---
harmony = [
    {"nanotick": 0 * BAR, "root": 9, "scale_id": 2},   # A minor
    {"nanotick": 1 * BAR, "root": 5, "scale_id": 1},   # F major
    {"nanotick": 2 * BAR, "root": 0, "scale_id": 1},   # C major
    {"nanotick": 3 * BAR, "root": 7, "scale_id": 1},   # G major
]

# --- Bass: root motion, some notes delayed; euclidean patcher; LFO->param ---
bass_roots = [33, 29, 36, 31]  # A1, F1, C2, G1
bass_notes = []
for b, root in enumerate(bass_roots):
    bass_notes.append(note(b * BAR, root, Q, vel=112))
    bass_notes.append(note(b * BAR + 2 * Q, root, Q, vel=96, delay=Q // 6))  # laid-back
    bass_notes.append(note(b * BAR + 3 * Q, root + 7, Q // 2, vel=90))
bass_mod = [{"link_id": 1,
             "src": {"device_id": 0, "source_id": 3, "kind": "lfo"},
             "dst": {"device_id": 0, "target_id": 7, "kind": "vst_param",
                     "param_uid16": "00112233445566778899aabbccddeeff"},
             "depth": 0.4, "bias": -0.1, "rate": "block"}]

# --- Pad: a chord per bar, triads + one seventh, with spread + humanize ---
pad_chords = [
    chord(0 * BAR, 0, BAR, quality=1, spread=SIX, humanize_timing=20, humanize_velocity=15),
    chord(1 * BAR, 0, BAR, quality=2, inversion=1, spread=SIX, humanize_velocity=12),  # 7th
    chord(2 * BAR, 0, BAR, quality=1, spread=SIX, humanize_timing=20),
    chord(3 * BAR, 4, BAR, quality=1, inversion=2, spread=SIX, humanize_velocity=10),
]

# --- Lead: melody, harmony-quantized, probability variation + a retrigger fill ---
lead_scale = [69, 71, 72, 74, 76, 77, 79]  # A4 up
lead_notes = []
for b in range(4):
    for s in range(4):
        p = lead_scale[(b * 2 + s) % len(lead_scale)]
        prob = 100 if s == 0 else 60  # downbeats sure, offbeats 60%
        lead_notes.append(note(b * BAR + s * Q, p, Q, vel=100, probability=(None if prob == 100 else prob)))
    # a ratchet fill on the last beat of bars 2 and 4
    if b % 2 == 1:
        lead_notes.append(note(b * BAR + 3 * Q, lead_scale[b % len(lead_scale)], Q, vel=105, retrigger=4))

# --- Arp: TRIPLET grid (lpb=3), fast, some retriggers ---
arp_scale = [57, 60, 64, 69]  # A3 C4 E4 A4
arp_notes = []
step = BAR // 12  # 12 triplet steps per bar
for b in range(4):
    for i in range(12):
        p = arp_scale[i % len(arp_scale)]
        arp_notes.append(note(b * BAR + i * step, p, step, vel=80,
                              retrigger=(2 if i % 6 == 5 else None)))

# --- Drums: multiple columns (kick/snare/hat), hat rolls + ghost snares ---
drum_notes = []
for b in range(4):
    for beat in range(4):
        drum_notes.append(note(b * BAR + beat * Q, 36, SIX, vel=115, col=0))  # kick each beat
    drum_notes.append(note(b * BAR + 1 * Q, 38, SIX, vel=110, col=1))         # snare on 2
    drum_notes.append(note(b * BAR + 3 * Q, 38, SIX, vel=110, col=1))         # snare on 4
    drum_notes.append(note(b * BAR + 2 * Q + Q // 2, 38, SIX, vel=55, col=1, probability=40))  # ghost
    for i in range(8):  # hats, 8ths, with a roll mid-bar
        drum_notes.append(note(b * BAR + i * (Q // 2), 42, Q // 4, vel=70, col=2,
                              retrigger=(4 if i == 6 else None)))

# --- Perc: SEXTUPLET grid (lpb=6), sparse, probability, MUTED; euclidean device ---
perc_notes = []
pstep = BAR // 24
for b in range(4):
    for i in range(0, 24, 5):
        perc_notes.append(note(b * BAR + i * pstep, 60 + (i % 5), pstep, vel=60,
                              probability=50))

tracks = [
    track(0, "Bass", quant=True, gain=-2.0, pan=-0.30, notes=bass_notes,
          mod_links=bass_mod, pat=patcher(euclidean=True, hits=5)),
    track(1, "Pad", gain=-4.0, pan=0.0, chords=pad_chords),
    track(2, "Lead", quant=True, gain=-3.0, pan=0.40, notes=lead_notes),
    track(3, "Arp", quant=True, lpb=3, gain=-5.0, pan=-0.50, notes=arp_notes,
          pat=patcher(euclidean=False)),
    track(4, "Drums", gain=0.0, pan=0.0, notes=drum_notes),
    track(5, "Perc", lpb=6, gain=-6.0, pan=0.60, mute=True, notes=perc_notes,
          chain=[euclid_device(0)]),
]

doc = {
    "schema_version": 1,
    "meta": {"name": "maximal", "created_utc": "", "modified_utc": ""},
    "timebase": {"nanoticks_per_quarter": Q},
    "tempo_map": [{"nanotick": 0, "bpm": 120.0},
                  {"nanotick": 2 * BAR, "bpm": 128.0}],   # tempo change at bar 3
    "harmony_timeline": harmony,
    "tracks": tracks,
}

out = sys.argv[1]
json.dump(doc, open(out, "w"), indent=2)
notes_total = sum(len(t.get("notes", [])) for t in tracks)
chords_total = sum(len(t.get("chords", [])) for t in tracks)
pats = sum(1 for t in tracks if "patcher" in t)
print(f"wrote {out}")
print(f"  {len(tracks)} tracks, {notes_total} notes, {chords_total} chords, "
      f"{len(harmony)} harmony changes, {len(doc['tempo_map'])} tempo points, "
      f"{pats} per-track patchers")
