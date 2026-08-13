// Generate the Rust mirror of the shared-memory layout from the C++ header, so it
// is a single source of truth instead of a hand-written duplicate. The header is
// parsed with -DSHM_BINDGEN, which turns the std::atomic fields into plain
// integers (identical byte layout) that bindgen understands.
use std::path::PathBuf;

fn main() {
    let manifest = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    let repo = manifest.join("../..").canonicalize().expect("repo root");
    // BOTH HALVES OF THE CONTRACT. shared_memory.h is the published regions; event_payloads.h is
    // every UI->engine command payload, and it was missing here — so the generated `sys` module
    // held 35 structs and not one of them was a Payload. The hand-written mirrors of those
    // payloads were pinned only to numbers a human typed after reading the header, which is the
    // one form of mirror that goes stale in silence: the person who changes the C++ struct is the
    // same person who has to remember the number. Every opcode payload added in the sampler work
    // was in that state.
    // ...and patcher_abi.h, which is the third. It is the call-frame ABI rather than a published
    // region: C++ fills a PatcherContext and hands it to the Rust node per block, on the audio
    // thread. Seven of patcher_rust's eight repr(C) types mirror this header and harmony_timeline.h
    // (which it includes), and until now not one of them had a generated twin to be pinned to —
    // PatcherContext least of all, whose members are mostly POINTERS, where a layout disagreement
    // is a wrong address rather than a wrong number.
    let headers = [
        repo.join("apps/shared_memory.h"),
        repo.join("apps/event_payloads.h"),
        repo.join("apps/patcher_abi.h"),
    ];
    // THE HEADERS THIS LISTS ARE NOT THE HEADERS BINDGEN READS, and that difference was a live
    // staleness hole. These three include two more — apps/event_id.h and apps/harmony_timeline.h —
    // and only the three were ever declared to cargo. So editing either of the other two did not
    // re-run this script: the bindings kept the old struct, and every check that compares a mirror
    // against them compared against a twin built from a header that no longer existed, and passed.
    // Not merely unverified — UNREBUILDABLE, because nothing knew a rebuild was due. HarmonyEvent
    // is declared in harmony_timeline.h and is one of the seven patcher mirrors now pinned against
    // this module, so the newest coverage sat on the weakest ground.
    //
    // The dependency list is now bindgen's own: `depfile` names every file it actually parsed, so
    // a header added to an include three levels down is declared to cargo without anyone
    // remembering to. See below the builder, where the depfile is read back.
    println!("cargo:rerun-if-changed=build.rs");

    let out = PathBuf::from(std::env::var("OUT_DIR").unwrap());
    let depfile = out.join("shm_sys.d");

    let bindings = bindgen::Builder::default()
        .depfile("shm_sys.rs", &depfile)
        .header(headers[0].to_str().unwrap())
        .header(headers[1].to_str().unwrap())
        .header(headers[2].to_str().unwrap())
        .clang_args([
            "-x",
            "c++",
            "-std=c++17",
            "-DSHM_BINDGEN",
            &format!("-I{}", repo.display()),
        ])
        // Only the POD wire structs + the size constants; skip the C++ machinery.
        .allowlist_type("daw::Shm.*")
        .allowlist_type("daw::Ring.*")
        .allowlist_type("daw::Ui.*")
        .allowlist_type("daw::EventEntry")
        .allowlist_type("daw::BlockMailbox")
        .allowlist_type("daw::Patcher.*")
        .allowlist_type("daw::MusicalLogicPayload")
        .allowlist_type("daw::HarmonyEvent")
        .allowlist_var("daw::kUi.*")
        .allowlist_var("daw::kShm.*")
        .derive_default(true)
        .derive_copy(true)
        .layout_tests(true)
        .generate()
        .expect("bindgen failed on shared_memory.h");

    bindings
        .write_to_file(out.join("shm_sys.rs"))
        .expect("write shm_sys.rs");

    // Makefile syntax: `shm_sys.rs: /path/a.h /path/b.h ...`, one line, spaces and backslashes
    // escaped. Everything after the first colon is the dependency set.
    let dep_text = std::fs::read_to_string(&depfile)
        .expect("bindgen wrote no depfile; without it this script cannot declare what it read");
    let mut deps: Vec<String> = Vec::new();
    let mut cur = String::new();
    let mut chars = dep_text.splitn(2, ':').nth(1).unwrap_or("").chars();
    while let Some(c) = chars.next() {
        match c {
            '\\' => {
                if let Some(escaped) = chars.next() {
                    cur.push(escaped);
                }
            }
            ' ' | '\n' | '\t' | '\r' => {
                if !cur.is_empty() {
                    deps.push(std::mem::take(&mut cur));
                }
            }
            other => cur.push(other),
        }
    }
    if !cur.is_empty() {
        deps.push(cur);
    }

    // Only what lives in this repo. System headers appear in the depfile too, and declaring a path
    // that may not exist on the next machine makes cargo re-run this script unconditionally — a
    // build that always rebuilds is its own kind of broken, and libc++ is not the contract.
    let mut declared = 0usize;
    for d in &deps {
        let path = PathBuf::from(d);
        if path.starts_with(&repo) && path.exists() {
            println!("cargo:rerun-if-changed={}", path.display());
            declared += 1;
        }
    }

    // FAIL CLOSED. If depfile support ever goes away, or the parse above stops matching, the loop
    // declares nothing and cargo silently stops rebuilding on header edits — the exact hole this
    // replaced, restored in silence and with no symptom. Every root header must at minimum appear.
    for h in &headers {
        let canonical = h.canonicalize().unwrap_or_else(|_| h.clone());
        assert!(
            deps.iter().any(|d| PathBuf::from(d) == canonical || PathBuf::from(d) == *h),
            "bindgen's depfile does not list {}, so the dependency set is not what it parsed \
             and header edits would stop triggering a rebuild",
            h.display()
        );
    }
    assert!(
        declared >= headers.len(),
        "only {} in-repo dependencies declared from a depfile of {}; expected at least the {} root \
         headers",
        declared,
        deps.len(),
        headers.len()
    );
}
