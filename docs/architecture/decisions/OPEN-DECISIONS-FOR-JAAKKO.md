# Open decisions

**Decisions 1-3 ruled 2026-08-13; decisions 4-6 ruled 2026-08-14.** Kept as the record of what was
asked and decided; do not re-open without a new decision.

| # | Question | RULED |
|---|---|---|
| 1 | CMD00 sender identity | **per-process nonce** |
| 2 | `commandType` on the wire | **dropped** |
| 3 | pending flapping-reset expiry | **timestamped, expires with the 10s window** |
| 4 | AE-P0.3 credentialed capture | **option A — Jaakko supplied the env file; capture taken and attested** |
| 5 | version bump for a repurposed field | **option A — bump to 39, and write the rule down once** |
| 6 | agent reporting an unconfirmed command | **A then B — mark unconfirmed now, per-verb acks incrementally** |

## Decision 4, closed by observation

Ruled by Jaakko pointing at the checkout's `.env`. Run 2026-08-14:

    DAW_WEBSTACK_ALLOW_CREDENTIALS=1 DAW_ENV_FILE=<checkout>/.env KEEP_ENGINE=0 tools/webstack.sh

The credential-boundary line was OBSERVED and compares byte-for-byte with the line `docs/DEMO.md`
quotes, under the `> ` marker mapping decided separately:

    ask     explicit credentialed mode; sidecar cwd cannot discover checkout/home .env files

No bytes were typed in by hand, which is the whole point of the ticket. The key itself was never
read, printed or copied — only the file's PATH was passed to the launcher.

---

The original write-up follows.

---

# (superseded header)

**Jaakko ruled: go with the recommendations, all three.** Kept as the record of what was
asked and decided; do not re-open without a new decision.

| # | Question | RULED |
|---|---|---|
| 1 | CMD00 sender identity | **per-process nonce** |
| 2 | `commandType` on the wire | **dropped** |
| 3 | pending flapping-reset expiry | **timestamped, expires with the 10s window** |

The original write-up follows.

---

## 1. CMD00 — how is a command's sender identified?

**Options**
- **A. Per-process nonce** — each process mints a random id at startup.
- **B. Allocated producer id** — the engine hands out ids.

**Cost if wrong:** A has a 1.2 × 10⁻⁴ chance that a client adopts another's refusal, and it
self-corrects on the next read. B needs an allocation path and a recycling rule.

**Recommendation: A.** It is what the CMD00 memo recommends after measuring, and the failure
is both rare and self-healing.

---

## 2. CMD00 — does `commandType` travel on the wire?

**Options**
- **A. Drop it.** The receiver already knows the verb from the command it is answering.
- **B. Keep it.** One extra field per message.

**Cost if wrong:** A means a human reading a raw ring dump must consult the sender's log to
see which verb was refused. B keeps a field that can disagree with the command it describes —
one fact in two places.

**Recommendation: A.** The memo recommends it, and a field that can contradict its own
message is the failure mode this program keeps paying for.

---

## 3. HOST-R3c — when should a pending "reset the flapping budget" request expire?

Background: a chain rebuild asks the restart worker to reset a track's crash counter. If the
restart never happens, that request sits set forever and the **next, unrelated** crash storm
consumes it — getting a fresh 5-restart budget it did not earn.

**Options**
- **A. Leave it.** Bounded to a wrong flapping count. Simplest.
- **B. Clear it on teardown.** Removes the stale request, but a rebuild racing a teardown
  loses a reset it legitimately earned.
- **C. Timestamp it** — a request older than the 10s window is ignored.

**Cost if wrong:** all three are bounded to "a track is given up on slightly too early or too
late". Nothing crashes.

**Recommendation: C**, as it is the only one that expires by the same rule the guard already
uses. **A is a perfectly good answer** if you would rather not spend a ticket on this.

---

# Decided without you — override any of these freely

These were either already recommended by a reviewer or cheap and reversible.

| # | Decision | Why |
|---|---|---|
| 1 | **AE-P0.3: keep `> ` as a documented marker**, with an explicit versioned mapping to the observed bytes | Removing it would leave the extractor with no way to tell a quoted output line from prose, and the mapping is already specified. Silent trimming is what the review forbade, not the marker itself. |
| 2 | **AE-P0.3: attestations pin a historical tree**, with selective current-blob validation | The reviewer preferred this. Requiring equality with current HEAD would invalidate every attestation the moment an unrelated commit lands. |
| 3 | ~~change `DEMO.md` to quote the DEFAULT branch's line~~ **REVERSED 2026-08-13 — the record pins `DAW_WEBSTACK_ALLOW_CREDENTIALS=1` instead** | I was wrong. `DEMO.md:58-70` shows the credentialed invocation and then says "Check the line it prints" — the quoted line IS the documented command's output. I decided from a summary of the doc instead of the doc. |

---

# NEW — decision 4 (2026-08-13, found by measurement)

## The one line the runbook quotes cannot be observed without your API key

`DEMO.md:70` quotes the **credentialed** launch's output. I tried to capture it and the
script refuses before printing anything: *"credentialed mode requested but no explicit key
resolves"*. Attestation is a record of something a person OBSERVED, so this line cannot be
attested by me, and I will not type the bytes in by hand — that is the exact defect this
ticket exists to catch.

The **credential-free** line is freely observable. I captured it. The two differ only in
one value; everything else is byte-identical.

**Options**
- **A. You run one capture.** `DAW_WEBSTACK_ALLOW_CREDENTIALS=1 DAW_ENV_FILE=<your file>
  tools/webstack.sh`, and paste me the line plus its surrounding output. One command, once.
  Keeps the runbook exactly as it is.
- **B. The runbook quotes BOTH lines** — credential-free and credentialed — and only the
  observable one is attested; the other is marked as requiring a key.
- **C. Attestation is scoped to lines observable without secrets.** The credentialed line
  gets a named, recorded exemption rather than an attestation.

**Recommendation: A if you are willing** — it is one command and it keeps the strongest
claim. Otherwise **C**, which is honest about the boundary rather than quietly narrowing it.

**Cost if wrong:** none is risky. B and C both weaken what the check proves about one line;
A costs you sixty seconds.


---

# NEW — decision 5 (2026-08-14)

## Does giving an existing field a new meaning require a version bump?

The tree holds two contradictory doctrines and they meet in CMD00 step 2.

- **CMD00's own rule**, stated in three of its documents: *"Giving a reserved field a meaning IS a
  wire change even though no size moves — that is precisely how a wire change lands without a
  bump."*
- **The tree's practice**, nine documented counter-precedents, e.g. `shared_memory.h:202`
  *"Repurposed reserved slot — same offset, no kShmVersion bump"*, and five more in
  `event_payloads.h`.

Step 2 gives `EventEntry::sampleTime` a meaning on UI command entries. Technically **no bump is
required** — nothing can observe it: `correlationLo` has no producer yet, and every shipped sender
writes `sampleTime = 0`.

**Options**
- **A. Bump to 39 anyway**, folded into step 2, since that commit also gives `correlationLo` its
  first producer — one coherent cutover.
- **B. Do not bump**, following the nine precedents, since nothing can observe the change.

**Recommendation: A** — and, separately from the choice, **write the rule down once**. Whichever you
pick, the contradiction will otherwise be re-litigated at every reserved-field change; that recurring
cost is larger than either option's.

**Cost if wrong:** A costs one unnecessary version step and a coordinated six-file edit. B risks a
future reader treating v38 and v39 wire images as identical when their field meanings differ.

*(Independent note, not a decision: the ring-index scheme I proposed is refuted and your ruling 1 —
the per-process nonce — stands unchanged. Recorded for information only.)*


---

# NEW — decision 6 (2026-08-14, found by measurement)

## The AI agent reports `ok: true` when it has no evidence the engine did anything

`refused_or` in `ui/daw-agent/src/tools.rs` waits 250 ms for a refusal. **No refusal ⇒ `ok: true`.**
That is absence of evidence reported as success.

Measured: **24 of 24** agent tool call sites work this way. Zero use `await_refusal_or_ack`, the
function that takes an "did it actually apply?" predicate and returns only after positive
confirmation. Only `daw-cli` uses that, at one site.

Why it matters more here than on the command line is already written in the function's own comment:
a person reads "sent" and moves on, **a model reads it as confirmation and reasons from it** — told
a sample loaded, it chops slices off a source that does not exist and reports a finished kit. And
the 250 ms window is known to be too short in at least one real path: a sampler refusal is emitted
from the render rebuild, later than the command ack.

**Options**
- **A. Add an explicit `confirmed: false`** to results that only saw no refusal. Additive, breaks
  no caller, ~1 hour. The model can still misread `ok`.
- **B. Give each site an `applied()` predicate** and use the ack path. Correct, and 24 sites each
  needing their own read-back — days, and some verbs may have no observable signal.
- **C. Report unconfirmed as `ok: false`.** Truthful about evidence, but reports failure for
  commands that in fact succeeded, which is worse for a model than the current defect.

**Recommendation: A now, B incrementally** for the verbs where a read-back already exists (the
sampler ones do). Not C.

**Cost if wrong:** A leaves the misreading possible but records the truth in the payload. B is
slow but ends the class. C would actively cause bad model behaviour.
