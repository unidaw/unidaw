# P2-WDOG-03 — what a dying host MEANS to the transport and the person using it

**Read-only design. No edits are proposed here, and none should be made from this document until
the questions below are answered by an owner.** Written from the current checkout; every site is
named with the line it is at, and the two sites are the whole subject.

## THE TWO SITES

**`engine_restart_worker.cpp:63` — give-up.** Guarded by `restartAttempts > kMaxRestartsPerWindow`.
Sets `hostGaveUp=true`, `hostReady=false`, `active=false`, `needsRestart=false`,
`restartInFlight=false`; writes a log line saying the track is disabled and the engine stays up;
emits `host.gave_up` with `track` and `attempts`; continues.

**`engine_restart_worker.cpp:70-76` — relaunch failure.** When `controller.launch()` returns false:
clears `hostReady`, `active`, `restartInFlight`; writes a log line; continues. **Emits nothing.**

## WHY THIS IS A POLICY TICKET AND NOT AN OBSERVABILITY ONE

Give-up has **no existing behaviour to extend**. It does not stop the transport, does not fail the
play, and does not reach the UI beyond one event. So "fatal play/start failure handling" is not a
gap in reporting — there is nothing being reported badly. It is an unanswered question about what
SHOULD happen, and writing code before it is answered would be inventing the semantics, which is
how P1.2's G3 acquired an oracle nobody could build.

## THE QUESTIONS, each with the reason it is not obvious

**1. Does a give-up stop the transport, or does the song keep playing with a hole in it?**
Today it keeps playing and one track goes silent. That is defensible — a musician mid-take does not
want the transport yanked because one plugin crashed — and it is also how a take gets ruined
silently. The answer probably differs between "recording" and "playing back", and this codebase
does not currently distinguish them at this site.

**2. Is a give-up recoverable in-session, and by what gesture?**
The log line says "Rebuild the chain (swap the plugin) to retry", so a recovery path exists and is
undiscoverable — it is in a log nobody reads during a session. If the answer to (1) is "keep
playing", this becomes the whole user story, because the state is otherwise permanent and unnamed.

**3. Should a FAILED RELAUNCH be visible before the give-up threshold is crossed?**
Today the answer is no: the attempts are invisible and only the final give-up is structured. So the
question "was this track struggling for a minute, or did it die at once?" is unanswerable after the
fact, which is exactly what a stall/drop oracle needs. **This is the cheapest of the three to
answer and the one with a clear default** — emit at :70-76 as the four drop sites will be — but it
is still a scope decision, because it changes what a consumer sees.

**4. What does the UI show for a given-up track?**
`hostGaveUp` exists on the runtime. Whether anything publishes it, and what the person is meant to
DO when they see it, is unaddressed.

## PREREQUISITE, AND THE ORDERING ARGUMENT

**P2-WDOG-02's transition surface must land first**, and not for tidiness: every question above is
of the form "what should happen WHEN we get here", and today the record of how we got here is a log
line. A policy written before the sequence is observable would be a policy nobody can verify was
followed — and the first bug report against it would be unanswerable for the same reason the
question above is.

## WHAT THIS TICKET DELIBERATELY DOES NOT DO

It proposes no behaviour. Every question above has a defensible answer in both directions and the
choice belongs to whoever owns how this program behaves when a plugin dies under someone's hands.
Recorded so the decision is made deliberately rather than by whoever next edits the restart worker.
