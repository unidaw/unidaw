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
    let headers = [
        repo.join("apps/shared_memory.h"),
        repo.join("apps/event_payloads.h"),
    ];
    for h in &headers {
        println!("cargo:rerun-if-changed={}", h.display());
    }
    println!("cargo:rerun-if-changed=build.rs");

    let bindings = bindgen::Builder::default()
        .header(headers[0].to_str().unwrap())
        .header(headers[1].to_str().unwrap())
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
        .allowlist_var("daw::kUi.*")
        .allowlist_var("daw::kShm.*")
        .derive_default(true)
        .derive_copy(true)
        .layout_tests(true)
        .generate()
        .expect("bindgen failed on shared_memory.h");

    let out = PathBuf::from(std::env::var("OUT_DIR").unwrap());
    bindings
        .write_to_file(out.join("shm_sys.rs"))
        .expect("write shm_sys.rs");
}
