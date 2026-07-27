// Generate the Rust mirror of the shared-memory layout from the C++ header, so it
// is a single source of truth instead of a hand-written duplicate. The header is
// parsed with -DSHM_BINDGEN, which turns the std::atomic fields into plain
// integers (identical byte layout) that bindgen understands.
use std::path::PathBuf;

fn main() {
    let manifest = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    let repo = manifest.join("../..").canonicalize().expect("repo root");
    let header = repo.join("apps/shared_memory.h");
    println!("cargo:rerun-if-changed={}", header.display());
    println!("cargo:rerun-if-changed=build.rs");

    let bindings = bindgen::Builder::default()
        .header(header.to_str().unwrap())
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
