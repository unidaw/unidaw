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


# The device kinds that MAY carry a patcher. See below.
PATCHER_KINDS = ("patcher_event", "patcher_instrument", "patcher_audio")


def output_node_id(graph):
    """The graph's own event_out, or the sentinel if it somehow has none.

    NAMED EXPLICITLY rather than left as 0xFFFFFFFF. The sentinel means "engine,
    work it out", and the engine does — but only on the multi-device assembly
    path. A LONE patcher device carrying the sentinel seeds no nodes into the
    evaluator's filter, so its generator never runs and the track is silent: a
    file that looks authored, loads clean, and makes no sound.

    That was fixed engine-side, and naming the node is still better. It costs
    nothing, it removes the dependency on a resolve step entirely, and it makes
    the published id walkable for per-device attribution whatever the engine
    does with it.
    """
    for n in graph.get("nodes", []):
        if n.get("type") == "event_out":
            return n["id"]
    return PATCHER_NODE_AUTO


def new_patcher_device(device_id=0, graph=None):
    return {
        "kind": "patcher_event",
        # It sits in the EVENT path: it may transform what passes through and it
        # may add events of its own. Audio is none of its business.
        "capability_mask": CONSUMES_MIDI | PRODUCES_MIDI,
        "patcher_node_id": output_node_id(graph or {}),
        "host_slot_index": 0,
        "device_id": device_id,
        "bypass": False,
    }


def migrate(path, write=True):
    """Enforce the rule on one file. Returns what it changed."""
    text = path.read_text()
    doc = json.loads(text)
    changed = []
    for track in doc.get("tracks", []):
        chain = track.setdefault("device_chain", [])
        used_ids = {d.get("device_id", 0) for d in chain}

        def free_id():
            i = 0
            while i in used_ids:
                i += 1
            used_ids.add(i)
            return i

        # What already lives on a device that is ALLOWED to carry a patcher.
        legit = [json.dumps((d.get("patcher") or {}).get("nodes"), sort_keys=True)
                 for d in chain
                 if d.get("kind") in PATCHER_KINDS and (d.get("patcher") or {}).get("nodes")]

        """
        A PATCHER ON A NON-PATCHER DEVICE IS A SILENT GENERATOR.

        Not a style rule — `assemblePatcherPool` only pools Patcher-kind devices
        and SKIPS vst_instrument, so in any project with two or more generator
        devices a graph attached to an instrument is never assembled and never
        runs. It looks authored, it looks correct, and it makes no sound.

        My first pass at this migration attached graphs to the track's instrument
        because that is what `generator.uniproj.json` did. That preset works only
        because it is a SINGLE-device project, which takes a different path
        entirely. Applied to `maximal` — two generator tracks — it would have
        silenced both, and silencing the generators is the failure that disguises
        itself as fixing the phantom notes.
        """
        for dev in chain:
            graph = (dev.get("patcher") or {}).get("nodes")
            if not graph or dev.get("kind") in PATCHER_KINDS:
                continue
            key = json.dumps(graph, sort_keys=True)
            if key in legit:
                # A device that may carry it already does. This is a duplicate —
                # left alone it is the same generator running twice.
                dev.pop("patcher", None)
                changed.append((track.get("name"), "dropped duplicate from "
                                + str(dev.get("kind")), [n["type"] for n in graph]))
            else:
                moved = dev.pop("patcher")
                head = new_patcher_device(free_id(), moved)
                head["patcher"] = moved
                chain.insert(0, head)
                legit.append(key)
                changed.append((track.get("name"), "moved off "
                                + str(dev.get("kind")) + " to a head device",
                                [n["type"] for n in graph]))

        # The legacy track-level field, which the engine still reads only to
        # migrate. See the note below on why it is REMOVED rather than nulled.
        graph = track.get("patcher")
        if graph and graph.get("nodes"):
            head = new_patcher_device(free_id(), graph)
            head["patcher"] = graph
            chain.insert(0, head)
            changed.append((track.get("name"), "moved off the TRACK to a head device",
                            [n["type"] for n in graph["nodes"]]))
        """
        REMOVED, not nulled. boost::property_tree reads a JSON null as a present
        but EMPTY node, so `get_child_optional("patcher")` succeeds, the graph
        reader finds no "nodes", and project_file.cpp returns false — which fails
        the ENTIRE project load. The engine falls back to an empty document, so
        the symptom is "my song is gone". The device path swallows the same error
        into a local, so a null on a DEVICE is harmless and a null on a TRACK is
        fatal.
        """
        track.pop("patcher", None)

    if changed and write:
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
    return changed


def main():
    root = Path(__file__).resolve().parent.parent / "presets" / "projects"
    write = "--dry-run" not in sys.argv
    total = 0
    for path in sorted(root.glob("*.json")):
        moved = migrate(path, write)
        for name, where, nodes in moved:
            print(f"{path.name:28} {name:8} -> {where:22} {' + '.join(nodes)}")
            total += 1
    print(f"\n{total} patcher(s) {'put' if write else 'would be put'} in their place")
    # The rule, audited. Both halves: nothing at track level, and nothing on a
    # device kind that the engine will not pool.
    bad = []
    for path in sorted(root.glob("*.json")):
        doc = json.loads(path.read_text())
        for track in doc.get("tracks", []):
            if (track.get("patcher") or {}).get("nodes"):
                bad.append(f"{path.name}:{track.get('name')} (track level)")
            for dev in track.get("device_chain", []):
                if (dev.get("patcher") or {}).get("nodes") \
                        and dev.get("kind") not in PATCHER_KINDS:
                    bad.append(f"{path.name}:{track.get('name')} (on {dev.get('kind')})")
    if bad:
        raise SystemExit("RULE VIOLATED:\n  " + "\n  ".join(bad))
    print("every patcher in every project lives on a patcher device")


if __name__ == "__main__":
    main()
