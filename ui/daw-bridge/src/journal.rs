//! WHAT THE ENGINE DID WITH A COMMAND, read back out of the project journal.
//!
//! Every client that sends a command has the same problem: the send succeeds the moment the bytes
//! reach the ring, which says nothing about whether the engine accepted them. daw-cli used to
//! print `{"sent": ...}` and exit 0 for edits the engine refused outright; the agent tools still
//! answer `{"sent": true}` the same way. The refusal was never missing — it is journalled in
//! history.jsonl with a named reason — it simply was not read.
//!
//! This module is that reader, and it lives here rather than in either client because there is
//! exactly one correct way to do it and two places that need it. A transcribed second copy would
//! agree on the function names and drift on the rules below, which is the expensive shape.
//!
//! THE TWO RULES THAT ARE NOT OBVIOUS, both learned by watching tests fail:
//!
//! 1. SCOPE DOES NOT IDENTIFY A COMMAND — it identifies a track. A sampler-load of a file that
//!    does not exist leaves a dangling source behind, and the engine re-resolves it on every
//!    subsequent project load, writing a fresh `rejected:load_failed` each time. Matching on scope
//!    alone made nine unrelated verbs report "the sample would not load" about a slot rename. So
//!    the op is matched too.
//!
//! 2. ONE VERB CAN BE JOURNALLED UNDER TWO OPS. A refused patcher connection is written under the
//!    family name `patcher_graph` by emitPatcherGraphError, and under the command's own name
//!    `connect_patcher_nodes` by the per-device path — which one depends on whether the caller
//!    passed a device. Waiting for a single name reports success for half of the verb's own
//!    behaviour. Callers pass a SET.
//!
//! The hole that remains, stated rather than left to be rediscovered: two commands of the SAME op
//! on one track inside one window are indistinguishable, because the engine writes an empty params
//! field for these families and the line carries no device, slot or file to tell them apart.

/// The engine's refusal reasons, worded.
///
/// The reason NAMES come from the engine — `errorScopeName` in engine_pure.cpp writes them into
/// the journal as `rejected:<name>` — so this maps a name to a sentence rather than a number to a
/// name, and an unmapped reason falls through to the engine's own word, which is already better
/// than a code. What a sentence adds is what the reader can DO: "order_violation" and "modulation
/// flows forward, so the source must sit before its target" are the same fact and only one of them
/// is actionable.
pub fn refusal_sentence(reason: &str) -> String {
    match reason {
        // TWO FAMILIES SHARE THIS WORD. errorScopeName spells a failed chain add and a failed
        // patcher-node add both as "add_failed", and a reason word alone cannot tell them apart —
        // the caller knows which it asked for, this function does not. Wording that covered only
        // the chain case told anyone whose patcher node was refused that a track takes one
        // head-of-chain instrument, which is true and entirely beside the point. So the sentence
        // names both cases rather than guessing at one. (The compiler found this, not a test: the
        // patcher arm below was unreachable.)
        "add_failed" => "it could not be added — adding a DEVICE fails when the track already has \
                         a head-of-chain instrument, since it takes only one; adding a patcher \
                         NODE fails when the graph rejected it",
        "remove_failed" => "there is no such device to remove",
        "move_failed" => "the device could not be moved to that position",
        "update_failed" => "the update changed nothing — the device id may not exist on that track",
        "track_missing" => "there is no such track",
        "link_missing" => "there is no such modulation link",
        "link_exists" => "that link already exists",
        "invalid_kind" => "that is not a kind this engine knows",
        "invalid_target" => "the route has no such target",
        "invalid_device" => "one of the devices in the link does not exist on that track",
        "order_violation" => "modulation flows FORWARD along the chain, so a source that sits \
                              after its target is refused — move the source earlier or pick a \
                              later target",
        "version" => "the edit quoted a base version the engine has already moved past",
        // The sampler family, now that its refusals reach the journal. Same rule as the rest:
        // the engine's word mapped to a sentence a reader can act on.
        // Read from the engine rather than guessed: each of these was found at its emit site
        // (engine_marker_commands, engine_placement_commands, engine_automation_commands,
        // engine_bulk_edit, engine_pure) before it was given a sentence.
        "no_such_track" => "there is no track with that id",
        "no_such_clip" => "there is no clip with that id",
        "no_such_placement" => "there is no placement with that id on that track",
        "no_alternate" => "this placement has nothing to swap to — swapping alternates between a \
                           clip and the one a previous scratch edit forked from it, and no fork \
                           has happened here yet",
        "id_exists" => "a marker with that id already exists — leave the id off to have one \
                        assigned",
        "empty_name" => "a marker needs a name: an unnamed flag is one nobody can read",
        "bad_value" => "that value is outside the range this field accepts",
        // The engine's own fallback (engine_pure.cpp `default:`), so it means the reverse of the
        // others: a reason code this build has a number for and no name. Saying so is more use
        // than repeating the word.
        "unnamed" => "the engine refused it but gave no reason name — this build's reason table \
                      has a gap, which is worth reporting rather than working around",
        "no_such_marker" => "there is no marker with that id — `get markers` lists the ones \
                              that exist",
        "no_such_slot" => "there is no slot with that id on this sampler — slot ids start at 1 \
                           and `get sampler-kit` lists the ones that exist",
        "no_such_device" => "there is no device with that id on this track",
        "not_a_sampler" => "that device is not a sampler — check the id, since a chain can hold \
                            effects and patchers beside it",
        "no_such_mod_set" => "there is no mod set with that id",
        "no_such_modulator" => "there is no modulator with that id in the mod set",
        "no_such_source" => "there is no loaded source with that id — load a sample first",
        "no_such_slice" => "there is no slice set with that id — chop the source first",
        "load_failed" => "the sample would not load: the file is missing, or not audio this \
                          build can decode",
        // The patcher graph family.
        "invalid_type" => "that is not a node type this engine knows",
        "invalid_node" => "there is no node with that id in this graph",
        "cycle" => "that connection would make a cycle, and the graph must stay acyclic",
        "invalid_connection" => "those two ports cannot be connected",
        "invalid_port" => "there is no such port on that node",
        "invalid_signature" => "that is not a time signature — the denominator must be a power of \
                                two, and 4/5 is refused rather than quietly clamped to 4/4, which \
                                would put the ruler somewhere nobody asked for",
        "zero_delta" => "a ripple of zero bars would change nothing",
        "no_track" => "there is no such track",
        "automation_in_removed_bars" => "the bars being removed carry automation, which would be \
                                         destroyed — clear it first, or remove a range that does \
                                         not cover it",
        other => return format!("the engine called it {other:?}"),
    }
    .to_string()
}

/// The journal's word for a command's scope. NOT `format!("track:{id}")` — the master track is
/// written as "master" and a global command as "global" (engine_history_journal.cpp), so a matcher
/// that only knew the track form would silently miss every refusal on the master track and every
/// global one. `kUiGlobalScope` is the id the engine itself passes for global.
pub const UI_GLOBAL_SCOPE: u32 = 0xFFFF_FFFF;

pub fn journal_scope(track: u32) -> String {
    match track {
        UI_GLOBAL_SCOPE => "global".to_string(),
        crate::layout::MASTER_TRACK_ID => "master".to_string(),
        id => format!("track:{id}"),
    }
}

pub fn history_path() -> std::path::PathBuf {
    std::path::Path::new(&crate::project::engine_project_dir()).join("history.jsonl")
}

/// The journal mark to take BEFORE sending, and the report to make after.
///
/// Two functions rather than one that owns the send, because every sampler verb has its own
/// typed send method — send_sampler_set_slot, send_sampler_device, send_named and so on — and a
/// helper that swallowed the send would need a closure per arm to say nothing extra.
pub fn journal_mark() -> u64 {
    std::fs::metadata(history_path()).map(|m| m.len()).unwrap_or(0)
}

/// The first refusal of THIS op in THIS scope after `offset`.
///
/// THE OP FILTER IS NOT OPTIONAL, and a test found out why. A `sampler-load` of a file that does
/// not exist leaves a dangling source that the engine RE-RESOLVES on every project load, so the
/// journal collects a fresh `sampler_load rejected:load_failed` long afterwards, interleaved with
/// whatever else is running. Without this filter, nine later verbs on the same track each adopted
/// that unrelated refusal and reported "the sample would not load" about a slot rename.
///
/// The residual hole, stated rather than hidden: two commands of the SAME op on the same track
/// within one window are still indistinguishable, because the journal line carries no device or
/// file to tell them apart. `sampler-load` is the only verb the retry behaviour makes likely, and
/// a wrong attribution there still names a real refusal of a real load.
pub fn journal_refusal_for(offset: u64, scope: &str, want_ops: &[String]) -> Option<String> {
    use std::io::{Read, Seek};
    let mut file = std::fs::File::open(history_path()).ok()?;
    file.seek(std::io::SeekFrom::Start(offset)).ok()?;
    let mut tail = String::new();
    file.read_to_string(&mut tail).ok()?;
    refusal_in(&tail, scope, want_ops)
}

/// The matching rule itself, over text — separated from the file so it can be tested without an
/// engine, a project directory, or an environment variable. Everything that makes this function
/// subtle is in the two rules at the top of this module, and both are pinned by tests below.
pub fn refusal_in(tail: &str, scope: &str, want_ops: &[String]) -> Option<String> {
    let want_scope = format!("\"scope\":\"{scope}\"");
    for line in tail.lines() {
        if !line.contains(&want_scope) || !want_ops.iter().any(|op| line.contains(op)) {
            continue;
        }
        if let Some(at) = line.find("\"outcome\":\"rejected:") {
            let rest = &line[at + "\"outcome\":\"rejected:".len()..];
            return Some(rest.split('"').next().unwrap_or("").to_string());
        }
    }
    None
}

/// Wait up to ~250ms for a refusal of one of `ops` on `track`, returning the engine's reason word.
///
/// SILENCE IS READ AS SUCCESS, and that is a deliberate trade rather than an oversight. The
/// families this serves — sampler, patcher — journal their refusals but publish no acknowledgement
/// on success, and they have no single choke point where one could be added. A waiter that
/// insisted on positive proof would therefore make every SUCCESSFUL command sit out the whole
/// window before printing. So a refusal that arrives later than the window is missed. That is
/// strictly better than what these callers did before, which was to report success unconditionally.
/// As `await_refusal`, but returns early when `applied` reports the edit landed.
///
/// SILENCE IS THE SUCCESS SIGNAL ONLY BECAUSE NOTHING BETTER WAS BEING READ. The sampler family
/// does publish an acknowledgement: refreshSamplerForTrack bumps samplerKitVersion on the success
/// path, and daw-bridge already exposes it. Watching it turns the window from the normal path into
/// a timeout — a successful command returns as soon as the engine has acted, instead of always
/// paying for the wait.
///
/// THE ACK IS AN OPTIMISATION, NOT A CORRECTNESS MECHANISM, and that is what makes it safe to
/// apply broadly: a refusal is still checked first on every pass, and a verb whose handler does
/// not bump the counter simply falls through to the same timeout it had before. Guessing wrong
/// costs latency, never truth.
///
/// The counter is GLOBAL — "bumped whenever any track's sampler state changes" — so another
/// track's edit can move it while ours is still being refused. Hence the final journal read before
/// reporting success: the cheap check that closes the window the shared counter opens.
pub fn await_refusal_or_ack(track: u32, journal_at: u64, ops: &[&str],
                            applied: impl Fn() -> bool) -> Option<String> {
    let scope = journal_scope(track);
    let want: Vec<String> = ops.iter().map(|op| format!("\"op\":\"{op}\"")).collect();
    for _ in 0..50 {
        if let Some(reason) = journal_refusal_for(journal_at, &scope, &want) {
            return Some(reason);
        }
        if applied() {
            return journal_refusal_for(journal_at, &scope, &want);
        }
        std::thread::sleep(std::time::Duration::from_millis(5));
    }
    None
}

pub fn await_refusal(track: u32, journal_at: u64, ops: &[&str]) -> Option<String> {
    let scope = journal_scope(track);
    let want: Vec<String> = ops.iter().map(|op| format!("\"op\":\"{op}\"")).collect();
    for _ in 0..50 {
        if let Some(reason) = journal_refusal_for(journal_at, &scope, &want) {
            return Some(reason);
        }
        std::thread::sleep(std::time::Duration::from_millis(5));
    }
    None
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Real lines, trimmed of the fields the matcher does not read. Written the way the engine
    /// writes them — no spaces — because the matcher is substring-based and a prettified fixture
    /// would pass while the real thing failed.
    const TAIL: &str = concat!(
        r#"{"seq":1,"scope":"track:0","op":"sampler_set_filter","outcome":"received"}"#, "\n",
        r#"{"seq":2,"scope":"track:0","op":"sampler_load","outcome":"rejected:load_failed"}"#, "\n",
        r#"{"seq":3,"scope":"track:0","op":"sampler_set_slot","outcome":"rejected:no_such_slot"}"#, "\n",
        r#"{"seq":4,"scope":"track:1","op":"sampler_set_slot","outcome":"rejected:not_a_sampler"}"#, "\n",
    );

    fn ops(list: &[&str]) -> Vec<String> {
        list.iter().map(|o| format!("\"op\":\"{o}\"")).collect()
    }

    #[test]
    fn a_refusal_is_found_by_its_own_op_and_scope() {
        assert_eq!(refusal_in(TAIL, "track:0", &ops(&["sampler_set_slot"])),
                   Some("no_such_slot".to_string()));
    }

    /// RULE 1. The load_failed on line 2 is in the same scope and comes FIRST. A matcher that only
    /// looked at scope would return it for a slot edit — which is exactly what happened, and made
    /// nine unrelated verbs report "the sample would not load" about a slot rename.
    #[test]
    fn another_commands_refusal_in_the_same_scope_is_not_adopted() {
        assert_eq!(refusal_in(TAIL, "track:0", &ops(&["sampler_set_device"])), None);
        assert_eq!(refusal_in(TAIL, "track:0", &ops(&["sampler_set_slot"])),
                   Some("no_such_slot".to_string()),
                   "and the op's own refusal is still found past the one it must skip");
    }

    /// RULE 2. One verb can be journalled under either name, so a caller passes both and either
    /// must hit.
    #[test]
    fn any_op_in_the_set_matches() {
        assert_eq!(refusal_in(TAIL, "track:0", &ops(&["patcher_graph", "sampler_load"])),
                   Some("load_failed".to_string()));
    }

    /// Scope still has to hold: track 1's refusal is not track 0's, and the two lines here are
    /// otherwise identical in op.
    #[test]
    fn a_refusal_on_another_track_is_not_this_tracks() {
        assert_eq!(refusal_in(TAIL, "track:2", &ops(&["sampler_set_slot"])), None);
        assert_eq!(refusal_in(TAIL, "track:1", &ops(&["sampler_set_slot"])),
                   Some("not_a_sampler".to_string()));
    }

    /// A `received` line is not a refusal — the matcher must not treat the presence of the op as
    /// the answer.
    #[test]
    fn an_accepted_command_reads_as_no_refusal() {
        assert_eq!(refusal_in(TAIL, "track:0", &ops(&["sampler_set_filter"])), None);
    }

    /// The master track and global commands are NOT written as "track:N", so a matcher built only
    /// from the track form would miss every refusal on them.
    #[test]
    fn master_and_global_have_their_own_scope_words() {
        assert_eq!(journal_scope(UI_GLOBAL_SCOPE), "global");
        assert_eq!(journal_scope(crate::layout::MASTER_TRACK_ID), "master");
        assert_eq!(journal_scope(3), "track:3");
    }

    /// A reason the table does not know still has to produce a usable sentence rather than an
    /// empty string — the engine may name a reason this build has never seen.
    #[test]
    fn an_unknown_reason_is_still_reported() {
        let s = refusal_sentence("some_new_reason");
        assert!(s.contains("some_new_reason"), "got {s:?}");
    }
}
