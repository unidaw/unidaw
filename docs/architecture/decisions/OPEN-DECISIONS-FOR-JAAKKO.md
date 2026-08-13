# Open decisions — ALL ANSWERED 2026-08-13

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
