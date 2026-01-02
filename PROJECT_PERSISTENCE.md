# Project Persistence (uniproj)

This document defines the initial persistence model for Unidaw projects and
patcher presets.

## Containers

### Project: `.uniproj`

- A zip container that holds a complete, recallable project.
- Goal: perfect recall, including VST state blobs and (later) audio assets.

Recommended contents:

- `project.json` (authoritative model + schema version)
- `vst_state/<deviceId>.bin` (raw plugin state blobs)
- `patchers/<deviceId>.json` (patcher graphs/configs per device)
- `audio/` (future: recorded clips, samples, consolidated audio)
- `meta.json` (optional: app/build version, time saved, platform)

### Presets (patcher devices)

- JSON only, stored in repo (lightweight, diffable).
- No binary payloads; only references to external assets.
- Future: `.unipatch` zip container for shareable patches with embedded audio
  and state.

## `project.json` (minimum schema)

- Global:
  - schema version
  - tempo map
  - time-signature map
  - harmony timeline
- Tracks:
  - name, order
  - routing (audio/midi)
  - mixer settings
- Device chains:
  - ordered devices: {id, kind, bypass, patcherNodeId, hostSlotIndex}
- Modulation links:
  - link list with sources/targets and depth/mode
- Automation:
  - automation clips and targets
- Clips:
  - per-track clip references and data (inline or by reference later)

## `project.json` schema stub (draft)

```json
{
  "schema_version": 1,
  "meta": {
    "name": "Untitled",
    "created_utc": "2026-01-01T12:00:00Z",
    "modified_utc": "2026-01-01T12:00:00Z"
  },
  "timebase": {
    "nanoticks_per_quarter": 960000
  },
  "tempo_map": [
    { "nanotick": 0, "bpm": 120.0 }
  ],
  "time_sig_map": [
    { "nanotick": 0, "num": 4, "den": 4 }
  ],
  "harmony_timeline": [
    { "nanotick": 0, "scale_id": 1, "root": 0 }
  ],
  "tracks": [
    {
      "track_id": 0,
      "name": "Track 1",
      "routing": {
        "audio_out": { "type": "master" },
        "midi_out": { "type": "none" },
        "pre_post": "post"
      },
      "mixer": {
        "gain_db": 0.0,
        "pan": 0.0,
        "mute": false,
        "solo": false
      },
      "device_chain": [
        {
          "device_id": 10,
          "kind": "patcher_event",
          "bypass": false,
          "patcher_node_id": 0,
          "host_slot_index": 0
        },
        {
          "device_id": 11,
          "kind": "vst_instrument",
          "bypass": false,
          "patcher_node_id": 0,
          "host_slot_index": 1,
          "vst_ref": {
            "vendor": "Vendor",
            "name": "Synth",
            "path": "/Library/Audio/Plug-Ins/VST3/Synth.vst3",
            "uid16": "00112233445566778899aabbccddeeff"
          }
        }
      ],
      "mod_links": [
        {
          "link_id": 100,
          "src": { "device_id": 10, "port": 0 },
          "dst": { "device_id": 11, "param_uid16": "ffeeddccbbaa99887766554433221100" },
          "depth": 0.5,
          "mode": "add",
          "enabled": true
        }
      ],
      "automation": [],
      "clips": []
    }
  ]
}
```

## Patcher device JSON

- Graph nodes and edges
- Node configs (per-node config blobs, in JSON)
- Exposed params/macros and ordering
- Asset references (samples by path or library id)

### Patcher device JSON example

```json
{
  "schema_version": 1,
  "device_id": 10,
  "graph": {
    "nodes": [
      {
        "node_id": 1,
        "type": "euclidean",
        "pos": [120, 80],
        "config": { "steps": 16, "hits": 5, "offset": 0, "duration_nanoticks": 0 }
      },
      {
        "node_id": 2,
        "type": "note_generator",
        "pos": [340, 80],
        "config": { "degree": 1, "octave_offset": 0, "velocity": 96 }
      },
      {
        "node_id": 3,
        "type": "event_out",
        "pos": [560, 80],
        "config": {}
      }
    ],
    "edges": [
      { "edge_id": 10, "src": { "node_id": 1, "port": 0 }, "dst": { "node_id": 2, "port": 0 } },
      { "edge_id": 11, "src": { "node_id": 2, "port": 0 }, "dst": { "node_id": 3, "port": 0 } }
    ]
  },
  "exposed_params": [
    { "node_id": 1, "param": "steps", "slot": 0, "label": "Steps" },
    { "node_id": 1, "param": "hits", "slot": 1, "label": "Hits" },
    { "node_id": 1, "param": "offset", "slot": 2, "label": "Offset" },
    { "node_id": 2, "param": "degree", "slot": 3, "label": "Degree" }
  ],
  "assets": [
    { "kind": "sample", "path": "samples/kick.wav" }
  ]
}
```

## Save Flow (engine)

1) Lock a consistent document snapshot (versioned).
2) Serialize `project.json`.
3) Dump per-device VST state to `vst_state/<deviceId>.bin`.
4) Serialize patcher graphs to `patchers/<deviceId>.json`.
5) Write the zip atomically (temp file -> rename).

## Load Flow (engine)

1) Open zip and read `project.json`.
2) Rehydrate tracks/device chains.
3) Load patchers from `patchers/` and attach to devices.
4) Load VST state blobs from `vst_state/` and restore.
5) Load audio assets from `audio/` (future).

## Notes

- Presets remain JSON-only to keep sharing/diff simple.
- Full-recall patch sharing can be added later as `.unipatch`.
