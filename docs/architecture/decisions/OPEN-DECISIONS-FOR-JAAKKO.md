# Open decisions — 3 questions

Each is a real fork I should not pick for you. Recommendation given; say "yes to all
recommendations" if you agree and I will proceed.

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
| 3 | **AE-P0.3: change `DEMO.md` to quote the DEFAULT branch's line** rather than pinning `DAW_WEBSTACK_ALLOW_CREDENTIALS=1` in the record | The runbook currently quotes output only the non-default invocation produces, so a reader following it sees a different line. Changing the doc makes the attested scenario the one people actually run. |
