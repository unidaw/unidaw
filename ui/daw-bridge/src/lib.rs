pub mod clipboard;
pub mod control;
pub mod grid;
pub mod journal;
pub mod layout;
pub mod project;
pub mod reader;
pub mod rowop;

/// Bindgen-generated mirror of the C++ shared-memory layout (apps/shared_memory.h,
/// parsed with -DSHM_BINDGEN). This is the single source of truth for the wire
/// format; the readers in `control`/`reader` sit on top and expose the stable
/// public API. Not idiomatic Rust — it mirrors the C++ names verbatim.
#[allow(non_snake_case, non_camel_case_types, dead_code, non_upper_case_globals)]
pub mod sys {
    include!(concat!(env!("OUT_DIR"), "/shm_sys.rs"));
}
