# Device Chain + Patcher UX Notes

This document captures current UX decisions for the device chain, patcher,
and modulation mapping.

## Layout + Placement

- Device chain strip is pinned to the bottom, full width.
- Tracker (or arrangement editor) owns the main canvas and can expand right as
  columns grow; time column stays pinned on the left.
- Opening a patcher replaces the main editor canvas; device chain remains
  visible at the bottom for navigation.
- A single-line breadcrumb/status row shows context (e.g., `Track 1 › Tracker`
  or `Track 1 › Patcher: <name>`) and provides a clear "back" target.

## Modes + Navigation

- Primary modes: Tracker, Arrangement, Patcher, Mixer.
- Piano roll is a projection inside Arrangement (not a separate mode).
- Mixer is a full mode and retains the bottom device chain for the selected
  track.
- Mode switching uses explicit shortcuts (no Ctrl-Tab).
  - Suggested: F1 Tracker, F2 Arrangement, F3 Patcher, F8 Mixer.
- Pane switching uses Tab/Shift-Tab (or Alt-Tab/Alt-Shift-Tab if needed).
  - Example panes: main editor, device chain, inspector popovers.

## Device Chain (Ordered, No Wires)

- The chain is a simple ordered sequence of devices; no wires in the chain UI.
- Each device auto-connects to the appropriate rails based on capability.
- Order matters for audio/event flow.
- VSTs are shown as individual tiles; no visual grouping of contiguous VSTs.

## Device Tiles

- Each tile can show:
  - Macro UI, or
  - Patcher "presentation" UI if the device is patcher-backed.
- VST tiles include:
  - Inline parameter rows with horizontal scroll.
  - Search/filter for large parameter lists, with pinning for favorites.
  - "Editor" button to pop out the VST editor.
  - Optional vertical expand to reduce cramped layouts.
- Inspector is a button-triggered popover (not a persistent side panel).
  - Popover is useful for full parameter lists when tiles are cramped.

## Selection + Editing

- Click selects a device.
- Backspace deletes selected device.
- Cmd+D duplicates.
- Cmd+X cuts.
- Cmd+V pastes at selection.

## Patcher View

- No patcher graph view inside the device chain.
- Patcher graph is a separate view; patcher-backed device tiles can open it
  via "Focus in Patcher."
- Complex routing/logic lives inside a patch-as-device.
- Patcher devices expose a "presentation" view:
  - If provided, show that presentation UI in the tile.
  - If not provided, show standard macro knobs.
- Patcher device tiles show the selected patcher node id (Event/Audio sink) and
  allow cycling to another valid node on the track.
- Standard macro knobs: 8 knobs, float 0.0..1.0 (32-bit).

## Patcher UX (Graph View)

- Dedicated patcher view with pan/zoom and optional grid snap.
- Clean wires (no skeuomorphic cables).
- Ports are typed and visually distinct:
  - Event ports (MIDI/logic)
  - Audio ports (multichannel bus with channel_count; default stereo)
  - Control ports (float, block-rate or sample-rate)
  - Event list ports (note/degree/chord lists; tracker phrases later)
- Connections are type-safe:
  - Event -> Event
  - Audio -> Audio
  - Control -> Parameter targets
- Explicit convert nodes if a translation is needed (e.g., Event -> Control).
  - Port ids are stable across node versions so patches survive upgrades.
- Node palette:
  - Cmd+Shift+P opens a filtered node palette.
  - Drag nodes into the graph.
- Selection + editing:
  - Click selects; Shift adds to selection.
  - Backspace deletes selected nodes.
  - Cmd+D duplicates (keeps internal wiring when possible).
  - Cmd+X/Cmd+V cut/paste.
- Node UI:
  - Compact headers, optional inline controls for key params.
  - "Presentation" layout is author-defined for patch-as-device.
  - Inspector is a popover, not a persistent side panel.
- Modulation sources:
  - Patcher nodes can expose control outputs for mapping.
  - Mapping uses the same "Map" affordance as device tiles.
- Graph/chain linking:
  - Selecting a patcher-backed device highlights its node(s).
  - Selecting a node highlights its device tile when present.

## Parameter Mapping (Everywhere)

Any parameter that can be mapped has a small "Map" button near it.

Mapping targets:
1) Add Tracker Column (track-aware).
2) Add Automation Lane (later).
3) Map to a modulation source (LFO, envelope, other device output).

Add Tracker Column:
- Defaults to the track owning the selected device.
- If cross-track mapping is allowed, show a track picker.

## Modulation Links (Ordered)

- Modulation is link-based and respects chain order.
- A modulation source must appear before its target in the device chain.
- UI hard-blocks invalid links (source after target).
- Sample-accurate modulation is required.
  - Control ports declare rate; sample-rate implies per-sample buffers, block-rate is per-block.

### Source UI

- Patcher sources (LFO/envelope/etc) show a "broadcasting" icon with a count
  (e.g., "⚡ 3") indicating active links.
- Sources can also expose "Map to..." to pick a destination param.

### Target UI

- Modulated params show a "mod ring" indicating base value, depth, and live
  movement.
- Clicking a mod badge opens a "Link Inspector" popover:
  - List of sources
  - Depth/polarity
  - Enable/disable per-link
  - Remove link

## Track Routing (Audio + MIDI)

- Any track's audio and MIDI outputs can be routed to any other track.
- Track I/O dropdown includes:
  - Other tracks (audio and/or MIDI)
  - Hardware audio inputs
- Example: generate MIDI on one track and feed another track's instrument.
- Routing can disable master send and route to a specific track instead.
- Pre/Post is a single toggle button.

## Clarifications

- The device chain is not an implicit patcher.
- Patcher nodes are devices when placed in the chain; the chain itself is
  still a linear device list.
- Undo covers all user edits.
- Selected devices can be copied/pasted across tracks.

## IPC / Data Flow (UI <-> Engine)

Engine is authoritative. UI operates on snapshots + diffs with version checks.

### Required UI Read Models

- Track list + names + order.
- Device chain per track:
  - chainVersion
  - ordered devices: {id, kind, capabilityMask, patcherNodeId, hostSlotIndex, bypass}
- Patcher graph:
  - graphVersion
  - nodes: {nodeId, type, position, ports, config blob id}
  - edges: {edgeId, srcPort, dstPort, type}
  - node presentation metadata (for patch-as-device tiles)
- Parameter registry:
  - per-device param list: {paramId, name, type, default, range, units}
  - pinned/favorite flags (UI-owned)
- Modulation links (per track):
  - linkId, srcDeviceId/srcPort, dstDeviceId/dstParamId, depth, mode, enabled
- Track routing:
  - audio out target (master/track/hardware), pre/post flag
  - MIDI out target (track/none)

### Required UI Commands

- Chain edits (with baseVersion):
  - AddDevice, RemoveDevice, MoveDevice, SetBypass
  - SetDevicePatcherNodeId / SetDeviceHostSlotIndex
  - DuplicateDevice (can be UI-side copy + AddDevice)
- Patcher graph edits:
  - AddNode, RemoveNode, MoveNode, SetNodeConfig, SetNodePresentation
  - AddEdge, RemoveEdge
- Parameter edits:
  - SetParamValue (sample-accurate where possible)
  - PinParam / UnpinParam (UI-owned)
- Modulation mapping:
  - AddLink, RemoveLink, SetLinkDepth, SetLinkMode, ToggleLink
  - Hard-block invalid links (source after target).
- Track routing:
  - SetTrackAudioOut (target + pre/post)
  - SetTrackMidiOut (target)
- Clipboard:
  - Copy/Cut/Paste devices across tracks (UI tracks selection metadata).

### Diffs / Notifications (Engine -> UI)

- Chain diffs:
  - chainVersion increments, device add/remove/move/updated fields.
- Patcher graph diffs:
  - graphVersion increments, node/edge add/remove/update, presentation updates.
- Param changes:
  - value updates, normalized/real value, optional display string.
- Modulation links:
  - link add/remove/update.
- Routing changes:
  - track audio/MIDI output updates.

### Constraints

- Sample-accurate modulation and param updates use sample offsets (block-local).
- Commands include baseVersion; mismatches return ResyncNeeded with snapshot.
