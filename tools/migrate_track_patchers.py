#!/usr/bin/env python3
"""Move every track-level patcher into a device, so no project has one.

A patcher is a DEVICE. Not a property of a track, and never a global graph.

WHY THIS MATTERS ENOUGH TO BE A SCRIPT. A track-level patcher has no position in
the signal path — is it before the instrument or after it? — so "where did that
note come from" has no answer even in principle. That question has been asked
three times about this project, cost hours each time, and produced three
confident wrong diagnoses (a missing note-off, MIDI leaking between tracks, a
version race) before anyone noticed a euclidean node. In a device chain the
generator is visible, ordered, bypassable and deletable, and the diagnostic
becomes "look at the chain".

`apps/device_chain.h` already says the same thing about the field it added:
"Per-device (a track can have several), superseding the single per-track
patcher." This finishes that sentence for the presets.

TWO CASES, and the difference is deliberate:

  The track HAS an instrument  -> attach the graph to THAT device. The graph
    emits events and the instrument plays them; putting the generator on the
    thing it drives is what `generator.uniproj.json` already does, and it keeps
    the relationship legible.

  The track has NO devices     -> synthesise a `patcher_event` device to carry
    it. The events then flow down an otherwise empty chain, which is exactly
    what happened before: silent until you drop an instrument in, at which point
    it plays. Same behaviour, now with something on screen to explain it.

Idempotent: a project with no track-level patcher is left byte-identical.
"""

import json
import sys
from pathlib import Path

# apps/device_chain.h, enum DeviceCapability.
CONSUMES_MIDI = 1 << 0
PRODUCES_MIDI = 1 << 1

# apps/device_chain.h. 0xFFFFFFFF is what generator.uniproj.json — the one
# preset that already carries a device-level patcher — uses for a device whose
# patcher node the engine assigns.
PATCHER_NODE_AUTO = 0xFFFFFFFF


def carrier_device(existing, graph):
    """The device that will own `graph`, and whether it is new."""
    if existing:
        # An instrument is already here: the generator drives it. Attaching
        # rather than inserting also avoids inventing a device id that has to
        # not collide with one the engine may already have handed out.
        return existing[0], False
    return {
        "device_id": 0,
        "kind": "patcher_event",
        # It sits in the EVENT path: it may transform what passes through and it
        # may add events of its own. Audio is none of its business.
        "capability_mask": CONSUMES_MIDI | PRODUCES_MIDI,
        "patcher_node_id": PATCHER_NODE_AUTO,
        "host_slot_index": 0,
        "bypass": False,
    }, True


def migrate(path, write=True):
    text = path.read_text()
    doc = json.loads(text)
    moved = []
    for track in doc.get("tracks", []):
        graph = track.get("patcher")
        if not graph or not graph.get("nodes"):
            # REMOVED, not nulled. See below — this is the same trap.
            track.pop("patcher", None)
            continue
        chain = track.setdefault("device_chain", [])
        device, is_new = carrier_device(chain, graph)
        if device.get("patcher", {}).get("nodes"):
            # Already carries one. Refuse rather than merge two graphs into a
            # shape nobody authored — two generators silently becoming one is
            # the class of bug this whole change exists to end.
            raise SystemExit(
                f"{path.name}: track {track.get('name')!r} has BOTH a track-level "
                f"patcher and a device that already carries one. Merge by hand.")
        device["patcher"] = graph
        if is_new:
            # At the HEAD: a generator feeds the chain, so it belongs before
            # everything the chain does with what it makes.
            chain.insert(0, device)
        # REMOVE THE KEY. Setting it to null does not work, and the failure is
        # spectacular: boost::property_tree reads a JSON null as a present but
        # EMPTY node, so `get_child_optional("patcher")` succeeds, the graph
        # reader finds no "nodes", and project_file.cpp:951 returns false — which
        # fails the ENTIRE project load. The engine then falls back to an empty
        # document, so the symptom is "my song is gone", reported by the e2e
        # suite as "0 notes on 1 tracks".
        #
        # Note the device path two hundred lines up swallows the same error
        # (project_file.cpp:869 discards it into a local `perr`), so a null on a
        # DEVICE is harmless and a null on a TRACK is fatal. Worth knowing before
        # trusting either.
        track.pop("patcher", None)
        moved.append((track.get("name"), "new device" if is_new else
                      f"existing {device.get('kind')}",
                      [n["type"] for n in graph["nodes"]]))
    if moved and write:
        # KEEP THE FILE'S OWN FORMATTING. The stress fixtures are written on one
        # line and the hand-authored ones are indented; reformatting either way
        # turned a nine-line change into 877,000 and buried it completely. A
        # migration whose diff cannot be read is a migration nobody can check.
        indented = text.lstrip().startswith("{\n")
        if indented:
            out = json.dumps(doc, indent=2) + "\n"
        else:
            out = json.dumps(doc, separators=(", ", ": "))
            if text.endswith("\n"):
                out += "\n"
        path.write_text(out)
    return moved


def main():
    root = Path(__file__).resolve().parent.parent / "presets" / "projects"
    write = "--dry-run" not in sys.argv
    total = 0
    for path in sorted(root.glob("*.json")):
        moved = migrate(path, write)
        for name, where, nodes in moved:
            print(f"{path.name:28} {name:8} -> {where:22} {' + '.join(nodes)}")
            total += 1
    print(f"\n{total} track-level patcher(s) "
          f"{'moved' if write else 'would move'} into devices")
    # The check the instruction actually asked for.
    left = []
    for path in sorted(root.glob("*.json")):
        doc = json.loads(path.read_text())
        for track in doc.get("tracks", []):
            if (track.get("patcher") or {}).get("nodes"):
                left.append(f"{path.name}:{track.get('name')}")
    if left and write:
        raise SystemExit("STILL TRACK-LEVEL: " + ", ".join(left))
    if write:
        print("no project has a track-level patcher")


if __name__ == "__main__":
    main()
