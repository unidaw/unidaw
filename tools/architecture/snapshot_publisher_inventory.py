#!/usr/bin/env python3
"""P-SNAPSHOT-PUBLISHERS, inventoried mechanically instead of counted by hand.

AE-P1.2 G2-B item 18:

    "Exactly twenty-four production TrackStateSnapshot publications exist: three prepublication
     assignments and twenty-one atomic stores; this packet does not silently treat them as a
     coherent host-plan authority."

WHY A SCRIPT AND NOT A SENTENCE. That population is the reason R-HOST-PLAN-AUTHORITY exists:
twenty-four independent publications of overlapping state are not one authority, and the ruling
replaces them with a single session ExecutionSnapshot. A number in prose cannot notice a
twenty-fifth appearing while the work is in flight, and this step's whole job is to move authority
away from these sites — so the population has to be measured on every run.

THE NUMBERS COME FROM THE FROZEN RECORD, not from this file. They are parsed out of the packet's own
statement, so the check is against the contract rather than against a constant somebody typed here.
When the record says twenty-four, twenty-four is what the code must have.

HOW THE PREDICATE WAS GOT WRONG, recorded because the correction is the interesting part. The first
pass grepped `atomic_store_explicit(&<ident>->trackSnapshot` on one line and found NINETEEN, then
reported the contract's twenty-one as a discrepancy. Two of the twenty-one put the `&receiver` on
its own line, the call having opened on the previous one. A single-line pattern is a proxy for
"a store happens here", and it undercounted the population by two while looking exact.

The other trap in the same direction: sixty of the ninety-one mentions of the identifier on the
frozen base are a LOCAL `std::vector<TrackRuntime*> trackSnapshot` in engine_consumer.cpp and
engine_producer_thread.cpp — a different thing that shares a name. Counting mentions would have
reported roughly three times the population.
"""

import json
import os
import pathlib
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
MAP_PATH = os.path.join(ROOT, "docs", "architecture", "tasks",
                        "AE-P1.2-g2b-implementation-steps.json")

WORDS = {
    "zero": 0, "one": 1, "two": 2, "three": 3, "four": 4, "five": 5, "six": 6, "seven": 7,
    "eight": 8, "nine": 9, "ten": 10, "eleven": 11, "twelve": 12, "thirteen": 13,
    "fourteen": 14, "fifteen": 15, "sixteen": 16, "seventeen": 17, "eighteen": 18,
    "nineteen": 19, "twenty": 20, "thirty": 30, "forty": 40, "fifty": 50, "sixty": 60,
    "seventy": 70, "eighty": 80, "ninety": 90,
}

failures = []


def fail(message):
    failures.append(message)


TENS_TO_WORD = {20: "twenty", 30: "thirty", 40: "forty", 50: "fifty", 60: "sixty",
                70: "seventy", 80: "eighty", 90: "ninety"}
UNITS_TO_WORD = {v: k for k, v in WORDS.items() if v < 20}


def render_number_word(value):
    """24 -> "twenty-four". None when this cannot spell it."""
    if value in UNITS_TO_WORD:
        return UNITS_TO_WORD[value]
    tens = (value // 10) * 10
    units = value % 10
    if tens in TENS_TO_WORD and units == 0:
        return TENS_TO_WORD[tens]
    if tens in TENS_TO_WORD and units in UNITS_TO_WORD:
        return f"{TENS_TO_WORD[tens]}-{UNITS_TO_WORD[units]}"
    return None


def number_word(text):
    """"twenty-four" -> 24. Returns None when the phrase is not a number this understands.

    ROUND-TRIPPED, because summing tokens is commutative and this had to verify what the record SAYS
    rather than what its tokens add up to. The first version accepted "four-twenty",
    "eleven-thirteen" and "zero-twenty-four" as twenty-four — it was checking "these words total N",
    which is a different claim from "this text is the English for N". Re-spelling the total and
    requiring the original back closes the gap: only the canonical phrase survives.
    """
    total = 0
    for part in text.lower().replace("-", " ").split():
        if part not in WORDS:
            return None
        total += WORDS[part]
    if render_number_word(total) != text.lower():
        return None
    return total


def frozen_expectations():
    """The three counts, read out of the frozen P-SNAPSHOT-PUBLISHERS statement."""
    step_map = json.load(open(MAP_PATH))
    spec = step_map["item18"]
    worktree = os.path.normpath(os.path.join(ROOT, spec["worktree"]))
    manifest_path = os.path.join(worktree, "docs", "architecture", "tasks",
                                 "AE-P1.2-g2b-item18-manifest.json")
    if not os.path.isfile(manifest_path):
        fail(f"item18 manifest not found at {manifest_path}; the population cannot be checked "
             f"against the frozen record, and a run that cannot check it must not pass")
        return None
    head = subprocess.run(["git", "-C", worktree, "rev-parse", "HEAD"],
                          capture_output=True, text=True).stdout.strip()
    if not head.startswith(spec["commit"][:12]):
        fail(f"item18 worktree HEAD {head} != pinned {spec['commit']}")
        return None

    manifest = json.load(open(manifest_path))
    statement = None
    for record in manifest["records"]:
        if record["id"] == "P-SNAPSHOT-PUBLISHERS":
            statement = record["statement"]
    if statement is None:
        fail("the frozen manifest has no P-SNAPSHOT-PUBLISHERS record")
        return None

    # "Exactly twenty-four production TrackStateSnapshot publications exist: three prepublication
    #  assignments and twenty-one atomic stores"
    total = re.search(r"Exactly ([a-z-]+) production TrackStateSnapshot publications", statement)
    prepub = re.search(r"([a-z-]+) prepublication assignments", statement)
    stores = re.search(r"([a-z-]+) atomic stores", statement)
    if not (total and prepub and stores):
        fail("P-SNAPSHOT-PUBLISHERS no longer states its three counts in the shape this reads; "
             "the record changed and this parser did not")
        return None
    values = tuple(number_word(m.group(1)) for m in (total, prepub, stores))
    if any(v is None for v in values):
        fail(f"could not read the record's number words: {[m.group(1) for m in (total, prepub, stores)]}")
        return None
    return {"total": values[0], "prepublication": values[1], "atomic_stores": values[2],
            "statement": statement}


def source_files():
    """Every C++ file in the WORKING TREE, wherever it lives.

    Two boundaries were wrong here and each was a demonstrated bypass:

      `git ls-tree HEAD -- apps` enumerated only COMMITTED paths under apps/. A reviewer added a
      publication in a brand-new uncommitted .h file and in a file outside apps/, and neither was
      seen — the second one permanently, since any future refactor into another directory would be
      invisible to a check pinned to one.

    So: the working tree, and the whole repository. A scan that decides where publications are
    allowed to live cannot also be the thing that only looks there.
    """
    out = []
    for path in sorted(pathlib.Path(ROOT).rglob("*")):
        if not path.is_file() or path.suffix not in (".cpp", ".h", ".hpp", ".cc"):
            continue
        rel = str(path.relative_to(ROOT))
        # Build outputs and vendored third-party trees are not this repository's code.
        if rel.startswith(("build/", "ui/target/", "third_party/", "external/", ".git/")):
            continue
        out.append(rel)
    return out


def strip_comments_and_strings(text):
    """Blank out comments and string literals, keeping every byte offset and newline.

    THREE SEPARATE BYPASSES CAME THROUGH HERE, all from one regex, `re.sub(r"//[^\n]*", "", raw)`:

      a `/* */` BLOCK COMMENT holding a textually identical publication was COUNTED, because the
        regex only knows about `//`. Combined with deleting a real one, that laundered a population
        change past both the count and the emitted artifact.
      a line containing `"http://example"` had everything after the `//` DELETED, so a real
        publication on that line vanished from the count.
      the file's own prose, which names the identifiers it searches for, was inside the search space.

    A regex cannot do this: whether `//` starts a comment depends on whether it is inside a string,
    and whether a quote opens a string depends on whether it is inside a comment. That is a lexer's
    job, and this is the smallest one that answers it. Offsets are preserved so line numbers in the
    emitted artifact still point at real source lines.
    """
    out = list(text)
    i = 0
    n = len(text)
    while i < n:
        ch = text[i]
        if ch == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                out[i] = " "
                i += 1
        elif ch == "/" and i + 1 < n and text[i + 1] == "*":
            out[i] = out[i + 1] = " "
            i += 2
            while i < n and not (text[i] == "*" and i + 1 < n and text[i + 1] == "/"):
                if text[i] != "\n":
                    out[i] = " "
                i += 1
            if i < n:
                out[i] = " "
                if i + 1 < n:
                    out[i + 1] = " "
                i += 2
        elif ch in ("\"", "'"):
            quote = ch
            i += 1
            while i < n and text[i] != quote:
                if text[i] == "\\":
                    out[i] = " "
                    i += 1
                    if i < n and text[i] != "\n":
                        out[i] = " "
                        i += 1
                    continue
                if text[i] != "\n":
                    out[i] = " "
                i += 1
            if i < n:
                i += 1
        else:
            i += 1
    return "".join(out)


# THE TWO SPELLINGS, AND THERE ARE NO OTHERS.
#
# apps/published_track_snapshot.h makes the slot private and gives it exactly two writers, so this
# script no longer has to enumerate the ways C++ can write a shared_ptr — a question two reviews
# proved it could not answer, closing eight shapes and then eleven more. The compiler answers it
# now; this counts the answer.
PUBLISH = re.compile(r"\.publish\s*\(")
PREPUBLICATION = re.compile(r"\.assignBeforePublication\s*\(")
# The member's own name, to find anything touching it that is neither a publication nor a read.
MEMBER_ACCESS = re.compile(r"(->|\.)\s*trackSnapshot\b")
ALLOWED_AFTER_MEMBER = re.compile(r"^\s*\.\s*(publish|assignBeforePublication|load)\s*\(")


def inventory():
    """Every production publication of a TrackStateSnapshot, classified.

    COUNTS TWO NAMED CALLS. Earlier versions scanned for `atomic_store_explicit` with the member's
    address in its argument list, and were defeated by nineteen distinct shapes across two reviews —
    block-comment decoys, `(*rt).member`, `.swap()`, helpers taking the slot by pointer or by
    reference, `auto&&` binds, typedef binds, raw string literals and digit separators that broke the
    lexer, a substring test on the path for "is this a test file", and more. Every repair widened a
    pattern and the next shape was outside the new one.

    The surface is gone instead. What remains is arithmetic.
    """
    stores, prepublication, escapes, in_tests = [], [], [], []
    for path in source_files():
        full = os.path.join(ROOT, path)
        raw = open(full, encoding="utf-8", errors="replace").read()
        if "trackSnapshot" not in raw and "assignBeforePublication" not in raw:
            continue
        text = strip_comments_and_strings(raw)
        raw_lines = raw.split("\n")
        is_test = path.endswith("_tests_main.cpp")
        # The class's own definition names both methods; it is not a publication site.
        is_definition = path.endswith("published_track_snapshot.h")

        def line_of(offset):
            return text.count("\n", 0, offset) + 1

        def line_text(line):
            return raw_lines[line - 1].strip() if line - 1 < len(raw_lines) else ""

        for pattern, bucket in ((PUBLISH, stores), (PREPUBLICATION, prepublication)):
            for match in pattern.finditer(text):
                if is_definition:
                    continue
                line = line_of(match.start())
                entry = {"file": path, "line": line, "text": line_text(line)}
                (in_tests if is_test else bucket).append(entry)

        # ANYTHING ELSE TOUCHING THE MEMBER IS A FINDING. With the slot private there is nothing else
        # a caller can legally do with it, so a mention that is not one of the three methods means
        # either a new accessor was added — reopening the surface — or this script has gone stale.
        if is_definition or path.endswith("engine_types.h"):
            continue
        for match in MEMBER_ACCESS.finditer(text):
            rest = text[match.end():match.end() + 60]
            if ALLOWED_AFTER_MEMBER.match(rest):
                continue
            line = line_of(match.start())
            escapes.append({"file": path, "line": line, "text": line_text(line)})
    return stores, prepublication, escapes, in_tests


def main():
    expected = frozen_expectations()
    stores, prepublication, escapes, in_tests = inventory()
    total = len(stores) + len(prepublication)

    # AN ESCAPE IS NOT A COUNT PROBLEM, IT IS A COUNTABILITY PROBLEM. If the member's address leaves
    # a counted store, some other code can publish through it and no count of stores can be trusted
    # again. Refusing is the only honest answer.
    for escape in escapes:
        fail(f"{escape['file']}:{escape['line']}: trackSnapshot is touched by something other than "
             f"publish/assignBeforePublication/load: {escape['text'][:80]}")

    if expected is not None:
        if len(stores) != expected["atomic_stores"]:
            fail(f"{len(stores)} atomic stores of a TrackStateSnapshot; the frozen record says "
                 f"{expected['atomic_stores']}")
        if len(prepublication) != expected["prepublication"]:
            fail(f"{len(prepublication)} prepublication assignments; the frozen record says "
                 f"{expected['prepublication']}")
        if total != expected["total"]:
            fail(f"{total} publications in total; the frozen record says {expected['total']}")

    # THE INVENTORY IS EMITTED, NOT KEPT. A derived summary maintained beside its source drifts from
    # it; one written by the checker on every run cannot. The artifact is what makes a twenty-fifth
    # publication show up in a diff rather than only in a failing count.
    artifact = {
        "record": "P-SNAPSHOT-PUBLISHERS",
        "total": total,
        "atomic_stores": stores,
        "prepublication_assignments": prepublication,
        # Reported so nothing is invisible; not part of the contract's twenty-four.
        "publications_in_test_files": in_tests,
    }
    out_path = os.path.join(ROOT, "docs", "architecture", "evidence",
                            "AE-P1.2-g2b-snapshot-publishers.json")
    rendered = json.dumps(artifact, indent=2) + "\n"
    if "--write" in sys.argv:
        os.makedirs(os.path.dirname(out_path), exist_ok=True)
        open(out_path, "w", encoding="utf-8").write(rendered)
    else:
        current = open(out_path, encoding="utf-8").read() if os.path.isfile(out_path) else ""
        if current != rendered:
            fail(f"{os.path.relpath(out_path, ROOT)} is not the inventory this run measured; "
                 f"re-run with --write and read the diff")

    print("P-SNAPSHOT-PUBLISHERS inventory: " + ("FAIL" if failures else "PASS"))
    for message in failures:
        print(f"  - {message}")
    if not failures:
        print(f"  {total} production publications: {len(prepublication)} prepublication "
              f"assignment(s) + {len(stores)} atomic store(s)")
        print(f"  counts read from the frozen record, not from this file")
        print(f"  {len(in_tests)} publication(s) in test files, reported and not counted")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
