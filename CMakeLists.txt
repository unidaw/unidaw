cmake_minimum_required(VERSION 3.18)

# clangd and tools/extraction_cost.sh both read compile_commands.json; generating it here means
# neither depends on how a particular build directory happened to be configured.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

project(daw VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(DAW_BUILD_PATCHER_RUST "Build Rust patcher staticlib" ON)

find_package(Boost REQUIRED)

set(JUCE_DIR "$ENV{HOME}/src/juce/JUCE" CACHE PATH "Path to JUCE")
add_subdirectory(${JUCE_DIR} juce)

add_library(platform_juce STATIC
  platform_juce/juce_wrapper.cpp
  platform_juce/uid_utils.cpp
)

target_include_directories(platform_juce PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_definitions(platform_juce PUBLIC
  JUCE_PLUGINHOST_VST3=1
  JUCE_USE_CURL=0
)

target_link_libraries(platform_juce PUBLIC
  juce::juce_audio_utils
  juce::juce_audio_processors
  juce::juce_cryptography
)

add_executable(juce_host
  apps/juce_host_main.cpp
  apps/plugin_cache.cpp
  apps/state_container.cpp
  apps/render_ahead.cpp
)

target_link_libraries(juce_host PRIVATE platform_juce)

add_executable(juce_scan
  apps/juce_scan_main.cpp
  apps/juce_scan_worker.cpp
  apps/plugin_cache.cpp
)

target_link_libraries(juce_scan PRIVATE platform_juce)

add_executable(juce_host_process
  apps/juce_host_process_main.cpp
  apps/audio_shm.cpp
  apps/event_ring.cpp
  apps/ipc_io.cpp
  apps/shared_memory.cpp
  apps/scale_library.cpp
)

target_include_directories(juce_host_process PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(juce_host_process PRIVATE platform_juce)

# THE DEPENDENCY LISTS RATCHET, rather than being minimal by hand.
#
# Each extracted command module aliases its deps at the top of every handler. When those were
# written, a blanket `(void)a; (void)b;` line followed to silence unused-variable warnings — which
# also silenced the only mechanism that could tell us a dependency is no longer needed. A
# re-grading panel pointed out that the lists were therefore minimal only because someone checked
# by hand, and hands stop checking.
#
# The discards are gone and these sources compile with -Werror=unused-variable. An alias nobody uses is
# now a warning naming the member that can leave the struct.
# AN ERROR, NOT A WARNING, and that is the whole point of it. As a warning this printed into a
# build log nobody reads: extracting renderTrack turned the warning on for code that had never
# had it and found SIX dead declarations, two of them entire lambdas — chainConsumesMidi (20
# lines) and cutAllActiveNotes — that were defined and never once called, verified against the
# pre-extraction file so they were dead before the move rather than broken by it. main.cpp had
# three more. A warning that does not fail is a comment, and this repo has spent the night
# finding out what comments are worth.
set(engine_commands_warnings -Werror=unused-variable)
file(GLOB _engine_command_sources ${CMAKE_CURRENT_SOURCE_DIR}/apps/engine_*_commands.cpp)
set_source_files_properties(apps/engine_render_track.cpp apps/engine_save_project.cpp
                            apps/engine_handle_ui_entry.cpp
                            apps/engine_load_project.cpp
                            apps/engine_bulk_edit.cpp
                            apps/engine_track_rebuild.cpp
                            apps/engine_chain_host.cpp
                            apps/engine_track_setup.cpp
                            apps/engine_clip_edit.cpp
                            apps/engine_publish_clips.cpp
                            apps/engine_produce_block.cpp
                            apps/engine_producer_thread.cpp
                            apps/engine_startup.cpp
                            apps/engine_offline_render.cpp
                            apps/engine_audio_start.cpp
                            apps/engine_shutdown.cpp
                            apps/engine_ui_shm.cpp
                            apps/engine_consumer.cpp
                            PROPERTIES COMPILE_OPTIONS -Werror=unused-variable)
set_source_files_properties(${_engine_command_sources}
  PROPERTIES COMPILE_OPTIONS "${engine_commands_warnings}")

# THE ENGINE, AS TWO LIBRARIES AND A main.cpp.
#
# The sixteen command modules used to compile only into this executable, so nothing else could
# link them: each test binary re-listed a dozen support sources by hand, and adding a test for any
# of the remaining untested handlers meant copying that list again. That friction is why they were
# untested. Measured cost of an edit-build-run cycle on an extracted command: 2.74s against 14.36s
# to compile and link daw_engine.
#
# No code moved and no file changed. This is a build-graph edit whose whole purpose is to make the
# next test three lines instead of twenty.
add_library(engine_core STATIC
  apps/audio_shm.cpp
  apps/clip_edit.cpp
  apps/chord_resolver.cpp
  apps/device_chain.cpp
  apps/event_ring.cpp
  apps/host_controller.cpp
  apps/ipc_io.cpp
  apps/patcher_graph.cpp
  apps/patcher_preset_library.cpp
  apps/patcher_preset.cpp
  apps/patcher_abi.cpp
  apps/plugin_cache.cpp
  apps/event_log.cpp
  apps/project_file.cpp
  apps/harmony_timeline.cpp
  apps/scale_library.cpp
  apps/ui_snapshot.cpp
  apps/shared_memory.cpp
)
target_include_directories(engine_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${Boost_INCLUDE_DIRS}
)
target_link_libraries(engine_core PUBLIC platform_juce)

add_library(engine_commands STATIC
  apps/engine_arrangetime_commands.cpp
  apps/engine_placement_commands.cpp
  apps/engine_transport_commands.cpp
  apps/engine_restart_worker.cpp
  apps/engine_harmony_timeline.cpp
  apps/engine_master_render.cpp
  apps/engine_song_extent.cpp
  apps/engine_ui_publish.cpp
  apps/engine_patcher_assemble.cpp
  apps/engine_ui_thread.cpp
  apps/engine_xrun_reporter.cpp
  apps/engine_pure.cpp
  apps/engine_rt_helpers.cpp
  apps/engine_render_track.cpp
  apps/engine_handle_ui_entry.cpp
  apps/engine_load_project.cpp
  apps/engine_bulk_edit.cpp
  apps/engine_track_rebuild.cpp
  apps/engine_chain_host.cpp
  apps/engine_track_setup.cpp
  apps/engine_clip_edit.cpp
  apps/engine_publish_clips.cpp
  apps/engine_produce_block.cpp
  apps/engine_producer_thread.cpp
  apps/engine_startup.cpp
  apps/engine_offline_render.cpp
  apps/engine_audio_start.cpp
  apps/engine_shutdown.cpp
  apps/engine_ui_shm.cpp
  apps/engine_consumer.cpp
  apps/engine_save_project.cpp
  apps/engine_sampler_commands.cpp
  apps/engine_automation_commands.cpp
  apps/engine_clip_commands.cpp
  apps/engine_modlink_commands.cpp
  apps/engine_module_commands.cpp
  apps/engine_patcher_commands.cpp
  apps/engine_chain_commands.cpp
  apps/engine_marker_commands.cpp
  apps/engine_project_commands.cpp
  apps/engine_rowops_commands.cpp
  apps/engine_track_commands.cpp
  apps/engine_request_commands.cpp
  apps/engine_trackprops_commands.cpp
  apps/engine_device_commands.cpp
  apps/engine_note_commands.cpp
  apps/engine_undo_commands.cpp
)
target_link_libraries(engine_commands PUBLIC engine_core)

add_executable(daw_engine
  apps/daw_engine_main.cpp
)
target_compile_options(daw_engine PRIVATE -Werror=unused-variable)

# The host processes get the same gate as the engine. Dead code hid in main.cpp for as long as it
# did because that file was never compiled with this warning; leaving the two hosts uncovered
# would just move the hiding place.
target_compile_options(juce_host PRIVATE -Werror=unused-variable)
target_compile_options(juce_host_process PRIVATE -Werror=unused-variable)


target_include_directories(daw_engine PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${Boost_INCLUDE_DIRS}
)

target_link_libraries(daw_engine PRIVATE engine_commands)

# THE SMALLEST PROGRAM THAT CAN ANSWER "does audio work on this machine". No producer, no
# threads, no shared memory, no plugin hosts, no transport — just the default output device and a
# count of how many times it asks for audio, plus a real tone so the answer can also be heard.
# Built because the engine could not answer this about itself: it opens the device, is told
# isPlaying() is true, and receives zero callbacks, which every layer above reads as silence with
# a hundred possible causes. See apps/audio_probe_main.cpp.
add_executable(daw_audio_probe apps/audio_probe_main.cpp)
target_include_directories(daw_audio_probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(daw_audio_probe PRIVATE platform_juce)

if (DAW_BUILD_PATCHER_RUST)
  set(PATCHER_RUST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/patcher_rust)
  set(PATCHER_RUST_TARGET_DIR ${CMAKE_CURRENT_BINARY_DIR}/patcher_rust)
  set(PATCHER_RUST_LIB ${PATCHER_RUST_TARGET_DIR}/release/libpatcher_rust.a)

  add_custom_command(
    OUTPUT ${PATCHER_RUST_LIB}
    COMMAND cargo build --release --manifest-path ${PATCHER_RUST_DIR}/Cargo.toml
            --target-dir ${PATCHER_RUST_TARGET_DIR}
    WORKING_DIRECTORY ${PATCHER_RUST_DIR}
    DEPENDS ${PATCHER_RUST_DIR}/Cargo.toml
            ${PATCHER_RUST_DIR}/src/lib.rs
    COMMENT "Building patcher_rust staticlib"
  )

  add_custom_target(patcher_rust_build DEPENDS ${PATCHER_RUST_LIB})

  add_library(patcher_rust STATIC IMPORTED)
  set_target_properties(patcher_rust PROPERTIES IMPORTED_LOCATION ${PATCHER_RUST_LIB})
  add_dependencies(patcher_rust patcher_rust_build)

  target_link_libraries(daw_engine PRIVATE patcher_rust)
endif()

juce_add_plugin(identity_plugin
  COMPANY_NAME "daw"
  PLUGIN_MANUFACTURER_CODE "DAW1"
  PLUGIN_CODE "IDEN"
  FORMATS VST3
  PRODUCT_NAME "Identity"
  IS_SYNTH FALSE
  NEEDS_MIDI_INPUT TRUE
  NEEDS_MIDI_OUTPUT FALSE
  IS_MIDI_EFFECT FALSE
  COPY_PLUGIN_AFTER_BUILD FALSE
)

target_sources(identity_plugin PRIVATE
  plugins/identity/IdentityProcessor.cpp
  plugins/identity/IdentityProcessor.h
)

target_include_directories(identity_plugin PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(identity_plugin PRIVATE
  juce::juce_audio_utils
)

target_compile_definitions(identity_plugin_VST3 PRIVATE
  JUCE_VST3_CAN_REPLACE_VST2=0
  JUCE_VST2_CAN_REPLACE_VST3=0
)

add_executable(phase2_tests
  apps/phase2_tests_main.cpp
  apps/audio_shm.cpp
  apps/clip_edit.cpp
  apps/chord_resolver.cpp
  apps/event_ring.cpp
  apps/host_controller.cpp
  apps/ipc_io.cpp
  apps/harmony_timeline.cpp
  apps/scale_library.cpp
  apps/ui_snapshot.cpp
  apps/shared_memory.cpp
)

target_include_directories(phase2_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(phase2_tests PRIVATE platform_juce)

add_executable(patcher_tests
  apps/patcher_tests_main.cpp
  apps/chord_resolver.cpp
  apps/harmony_timeline.cpp
  apps/patcher_abi.cpp
  apps/patcher_graph.cpp
  apps/scale_library.cpp
)

target_include_directories(patcher_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${Boost_INCLUDE_DIRS}
)

target_link_libraries(patcher_tests PRIVATE platform_juce)

if (DAW_BUILD_PATCHER_RUST)
  target_link_libraries(patcher_tests PRIVATE patcher_rust)
endif()

add_executable(patcher_graph_tests
  apps/patcher_graph_tests_main.cpp
  apps/patcher_abi.cpp
  apps/patcher_graph.cpp
  apps/patcher_preset_library.cpp
  apps/patcher_preset.cpp
)

target_include_directories(patcher_graph_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${Boost_INCLUDE_DIRS}
)

target_link_libraries(patcher_graph_tests PRIVATE platform_juce)

if (DAW_BUILD_PATCHER_RUST)
  target_link_libraries(patcher_graph_tests PRIVATE patcher_rust)
endif()

# M2.20: the deterministic linter. Shares the engine's project parser on purpose — a
# linter with its own parser lints a different document.
add_executable(daw_lint
  apps/daw_lint_main.cpp
  apps/event_log.cpp
  apps/project_file.cpp
  apps/device_chain.cpp
  apps/plugin_cache.cpp
  apps/patcher_preset.cpp
  apps/patcher_graph.cpp
  apps/patcher_abi.cpp
)
target_include_directories(daw_lint PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${Boost_INCLUDE_DIRS}
)

add_executable(project_file_tests
  apps/project_file_tests_main.cpp
  apps/event_log.cpp
  apps/project_file.cpp
  apps/device_chain.cpp
  apps/plugin_cache.cpp
  apps/patcher_preset.cpp
  apps/patcher_graph.cpp
  apps/patcher_abi.cpp
)

target_include_directories(project_file_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${Boost_INCLUDE_DIRS}
)

target_link_libraries(project_file_tests PRIVATE platform_juce)

# Header-only numeric tests for the waveform pyramid (no JUCE/Boost needed).
add_executable(waveform_pyramid_tests
  apps/waveform_pyramid_tests_main.cpp
)
target_include_directories(waveform_pyramid_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
)

# Header-only tests for the waveform source registry (content key + interning).
add_executable(waveform_store_tests
  apps/waveform_store_tests_main.cpp
)
target_include_directories(waveform_store_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
)

# M2.18: the multi-producer command ring, hammered by concurrent threads. The shell
# check exercises real processes but cannot reliably create the race; this can.
add_executable(event_ring_mpsc_tests
  apps/event_ring_mpsc_tests_main.cpp
  apps/event_ring.cpp
  apps/shared_memory.cpp
)
target_include_directories(event_ring_mpsc_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
)
find_package(Threads REQUIRED)
target_link_libraries(event_ring_mpsc_tests PRIVATE Threads::Threads)

# v29: markers — named ticks, and the ripple geometry that used to live with the spine.
add_executable(markers_tests
  apps/markers_tests_main.cpp
)
target_include_directories(markers_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
)

# Sampler S1: multipoint drawable envelopes. The claim under test is that ADSR is a VIEW of the
# same points (docs/SAMPLER_DESIGN.md R4) — if that is false, the sampler has two envelope models
# pretending to be one and every later feature has to ask which it is talking to.
add_executable(sampler_envelope_tests
  apps/sampler_envelope_tests_main.cpp
)
target_include_directories(sampler_envelope_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
)

# Sampler S1: the document — keymap resolution, derived facts, and the 15-char param-id wall.
add_executable(sampler_state_tests
  apps/sampler_state_tests_main.cpp
)
target_include_directories(sampler_state_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
)

# Sampler S1: the voice. The headline property is BLOCK-SIZE INVARIANCE — one note rendered at
# 64, 256 and 1024 frames must be bit-identical, which is what keeps intra-block starts honest.
add_executable(sampler_voice_tests
  apps/sampler_voice_tests_main.cpp
)
target_include_directories(sampler_voice_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
)

# S7: the .uni module container. Two properties carry the format and both are about what happens
# when something is WRONG — a corrupt entry must be REFUSED rather than returned, and identical
# content must produce an identical file so "did anything change?" stays answerable.
add_executable(zip_container_tests
  apps/zip_container_tests_main.cpp
)
target_include_directories(zip_container_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
)

# Sampler S5: slicing. The assertions are about IDENTITY, not audio — "slice 5 still plays the
# same frames after an insert at slice 2" is not a detail, it is the feature that lets a chop be
# re-cut while it plays.
add_executable(sampler_slice_tests
  apps/sampler_slice_tests_main.cpp
)
target_include_directories(sampler_slice_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
)

# Sampler S3: the DSP — mip-map, the three interpolators, and the filter. Every assertion here is
# ACOUSTIC, because the structural versions all pass against a feature that is built and never
# read: the levels exist, the setting is stored, the filter is constructed.
add_executable(sampler_dsp_tests
  apps/sampler_dsp_tests_main.cpp
)
target_include_directories(sampler_dsp_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
)

# Sampler S1: the polyphonic layer — choke, NNA, stealing, and sample-accurate note-OFF, which
# is the thing block segmentation exists for.
add_executable(sampler_engine_tests
  apps/sampler_engine_tests_main.cpp
)
target_include_directories(sampler_engine_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
)

# M3.24: `adds` are an appearance's own content — emitted once, not looped with the clip.
add_executable(placement_adds_tests
  apps/placement_adds_tests_main.cpp
  apps/project_file.cpp
  apps/device_chain.cpp
  apps/plugin_cache.cpp
  apps/patcher_preset.cpp
  apps/patcher_graph.cpp
  apps/patcher_abi.cpp
  apps/event_log.cpp
)
target_include_directories(placement_adds_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${Boost_INCLUDE_DIRS}
)

# M3.22: the song's time-signature map and bar numbering across changes.
add_executable(time_signature_tests
  apps/time_signature_tests_main.cpp
)
target_include_directories(time_signature_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
)

# M3.22: the tempo integral — absolute musical time <-> wall time across a tempo map.
add_executable(tempo_integral_tests
  apps/tempo_integral_tests_main.cpp
)
target_include_directories(tempo_integral_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
)

# M1.13: non-destructive lane quantize. Header-only, no deps.
add_executable(lane_quantize_tests
  apps/lane_quantize_tests_main.cpp
)
target_include_directories(lane_quantize_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
)

# THE ENGINE'S PURE HELPERS — eleven rules that were lambdas inside a 20,000-line main() and so had
# no name any other translation unit could say. Not one had a test; reaching `clampMidi` meant
# booting an engine and driving a chord through a command ring. Extracting them to
# apps/engine_pure.h is what made the tests possible, and the tests are the reason to extract them.
# THE FIRST UNIT TEST OF A TRACK REBUILD. rebuildFlatAndPublish was a lambda inside main() until
# it moved to apps/engine_track_rebuild.cpp; asking it a question meant booting an engine, mapping
# shared memory and driving a command ring. The mute-prune guard it checks — prune dead mutes, but
# only when the clip is actually installed — is the kind of condition that looks like an oversight
# and whose removal passes every process-level test.
add_executable(engine_track_rebuild_tests
  apps/engine_track_rebuild_tests_main.cpp
)

# locateEditTarget decides what EVERY note, chord and row edit acts on. The rule under test is the
# one that costs the user data when it goes: a remove landing outside every placement must not mint
# a clip to hold the nothing it removed.
add_executable(engine_clip_edit_tests
  apps/engine_clip_edit_tests_main.cpp
)

# The bulk carrier's size checks. A reassembled envelope that declares more points than it carries
# must be REFUSED rather than applied short: half an envelope is a valid envelope, so delivering it
# produces a wrong sound instead of an error.
add_executable(engine_bulk_edit_tests
  apps/engine_bulk_edit_tests_main.cpp
)

# reconcileChildTracks runs on EVERY consumer tick, so its idempotence is load-bearing: a broken
# "does this child exist" test would append another child per tick until the 64-track budget broke.
# Also pins the sampler-stem bus synthesis, which is the gap S6 left — a sampler has no host to ask
# for a bus layout, so a sampler-only multi-out track used to get no children at all.
add_executable(engine_track_setup_tests
  apps/engine_track_setup_tests_main.cpp
)
target_include_directories(engine_track_setup_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${Boost_INCLUDE_DIRS}
)
target_link_libraries(engine_track_setup_tests PRIVATE engine_commands)
target_include_directories(engine_bulk_edit_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${Boost_INCLUDE_DIRS}
)
target_link_libraries(engine_bulk_edit_tests PRIVATE engine_commands)
target_include_directories(engine_clip_edit_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${Boost_INCLUDE_DIRS}
)
target_link_libraries(engine_clip_edit_tests PRIVATE engine_commands)
target_include_directories(engine_track_rebuild_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${Boost_INCLUDE_DIRS}
)
target_link_libraries(engine_track_rebuild_tests PRIVATE engine_commands)

add_executable(engine_pure_tests
  apps/engine_pure_tests_main.cpp
)
target_include_directories(engine_pure_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${Boost_INCLUDE_DIRS}
)
target_link_libraries(engine_pure_tests PRIVATE engine_commands)

# THE FIRST UNIT TESTS OF A UI COMMAND. Until these handlers were extracted from handleUiEntry
# there was no way to ask a command a question without building an engine, mapping shared memory
# and writing a payload into a ring — which is why the REJECT paths, most of what these handlers
# do, had no direct coverage at all.
# 362 lines with no unit coverage: six distinct refusal reasons, all of them silent on this wire.
# The centrepiece is the forward-flow rule — a device modulating ITSELF is legal and used to be
# refused, so a preset could load a link no command could recreate.
# 521 lines, six refusal codes, no unit coverage. The cycle refusal is the one that matters: a
# patcher graph runs in topological order, so an edge closing a loop has no order to run in, and it
# is the refusal least likely to be reached by hand.
add_executable(engine_patcher_commands_tests
  apps/engine_patcher_commands_tests_main.cpp
)
target_include_directories(engine_patcher_commands_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${Boost_INCLUDE_DIRS}
)
target_link_libraries(engine_patcher_commands_tests PRIVATE engine_commands)

add_executable(engine_modlink_commands_tests
  apps/engine_modlink_commands_tests_main.cpp
)
target_include_directories(engine_modlink_commands_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${Boost_INCLUDE_DIRS}
)
target_link_libraries(engine_modlink_commands_tests PRIVATE engine_commands)

# The command line and the environment: 150 lines that could not be tested before they were a
# function, because reaching them meant starting an engine. Two of the assertions are past bugs —
# a valueless flag in LAST position, and an out-of-range --sample-rate that must be refused rather
# than quietly replaced by the device's.
add_executable(engine_startup_tests
  apps/engine_startup_tests_main.cpp
)
target_include_directories(engine_startup_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${Boost_INCLUDE_DIRS}
)
target_link_libraries(engine_startup_tests PRIVATE engine_commands)

add_executable(engine_sampler_commands_tests
  apps/engine_sampler_commands_tests_main.cpp
)
target_include_directories(engine_sampler_commands_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${Boost_INCLUDE_DIRS}
)
target_link_libraries(engine_sampler_commands_tests PRIVATE engine_commands)

# The automation handlers' four refusal paths are SILENT — a structured event and a return, with
# nothing on the wire. These tests point DAW_EVENT_LOG at a file and assert WHICH refusal fired,
# because "no lane was created" is otherwise identical across all four.
add_executable(engine_automation_commands_tests
  apps/engine_automation_commands_tests_main.cpp
)
target_include_directories(engine_automation_commands_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${Boost_INCLUDE_DIRS}
)
target_link_libraries(engine_automation_commands_tests PRIVATE engine_commands)

# THE FIRST TESTS OF REAL-TIME-PATH CODE. These three rules run on the producer thread, which
# calls elevateToAudioPriority(); until they were extracted, asking whether an unknown scaleId
# still produces an audible pitch meant booting a process and rendering audio.
add_executable(engine_rt_helpers_tests
  apps/engine_rt_helpers_tests_main.cpp
)
target_include_directories(engine_rt_helpers_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${Boost_INCLUDE_DIRS}
)
target_link_libraries(engine_rt_helpers_tests PRIVATE engine_commands)

# Header-only tests for the per-clip grid packed into UiClipExtent.flags.
add_executable(clip_grid_tests
  apps/clip_grid_tests_main.cpp
)
target_include_directories(clip_grid_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
)

# The pyramid verified against the real fixture WAVs (decodes through platform_juce).
add_executable(waveform_fixture_tests
  apps/waveform_fixture_tests_main.cpp
)
target_include_directories(waveform_fixture_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${Boost_INCLUDE_DIRS}
)
target_link_libraries(waveform_fixture_tests PRIVATE platform_juce)

add_executable(audio_io_tests
  apps/audio_io_tests_main.cpp
)
target_include_directories(audio_io_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(audio_io_tests PRIVATE platform_juce)

add_executable(device_chain_tests
  apps/device_chain_tests_main.cpp
  apps/device_chain.cpp
)

target_include_directories(device_chain_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(device_chain_tests PRIVATE platform_juce)

add_executable(device_chain_ui_tests
  apps/device_chain_ui_tests_main.cpp
  apps/device_chain.cpp
  apps/event_ring.cpp
  apps/shared_memory.cpp
  apps/ipc_io.cpp
  apps/plugin_cache.cpp
)

target_include_directories(device_chain_ui_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(device_chain_ui_tests PRIVATE platform_juce)

add_executable(device_chain_ui_live_tests
  apps/device_chain_ui_live_tests_main.cpp
  apps/audio_shm.cpp
  apps/event_ring.cpp
  apps/shared_memory.cpp
  apps/ipc_io.cpp
  apps/plugin_cache.cpp
)

target_include_directories(device_chain_ui_live_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(device_chain_ui_live_tests PRIVATE platform_juce)

enable_testing()

# FIRST, deliberately. If a binary is stale against the contract it is built from, every failure
# below it is a lie about a different subsystem — so this one should be the first thing you read.
add_test(NAME contract_freshness
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tools/contract_freshness_check.sh
)
set_tests_properties(contract_freshness PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

# Every hand-written Rust mirror is pinned to the C++ struct bindgen generates from the header.
add_test(NAME contract_layout
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tools/contract_layout_check.sh
)
set_tests_properties(contract_layout PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

# THE RUST SIDE'S OWN TESTS, which had never run here. contract_layout proves every mirror is
# LISTED in bindgen_matches_hand_written; that test is what actually compares the bytes, and it
# only ever ran under `cargo test`. So the byte-level guard was absent from the suite that gates
# every commit — the list was checked and the thing the list points at was not.
add_test(NAME bridge_layout
  COMMAND cargo test --manifest-path ${CMAKE_CURRENT_SOURCE_DIR}/ui/Cargo.toml -p daw-bridge
)
set_tests_properties(bridge_layout PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

add_test(NAME patcher_resolution
  COMMAND patcher_tests
)
set_tests_properties(patcher_resolution PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

add_test(NAME patcher_graph_edits
  COMMAND patcher_graph_tests
)
set_tests_properties(patcher_graph_edits PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

add_test(NAME project_file_round_trip
  COMMAND project_file_tests ${CMAKE_CURRENT_SOURCE_DIR}/presets/projects
)
set_tests_properties(project_file_round_trip PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

add_test(NAME waveform_pyramid
  COMMAND waveform_pyramid_tests
)
set_tests_properties(waveform_pyramid PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

add_test(NAME waveform_store
  COMMAND waveform_store_tests
)
set_tests_properties(waveform_store PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

add_test(NAME event_ring_mpsc
  COMMAND event_ring_mpsc_tests
)
set_tests_properties(event_ring_mpsc PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

# M2.20: the linter's own coverage — every rule fires, clean input is silent, output is
# deterministic. Pure file in / text out, so it needs no audio device and belongs here.
# M2.21: every opcode has a CLI path or a declared reason it does not. Pure source
# analysis, so it belongs in ctest.
add_test(NAME op_registry
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/op_registry_check.sh
)
set_tests_properties(op_registry PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

# CAN CMAKE STILL PROCESS THIS TREE. Every other test here runs against an already-configured
# build directory, so none of them can see a configure-time break — and main spent a day in one
# (two add_test entries named clip_grid) while every warm tree stayed green.
#
# Re-configures a persistent probe directory, which takes about a second in the steady state and
# still processes every add_test. The first run on a machine configures from empty and can take
# several minutes, which is what the generous timeout is for — see the script for what the cheap
# form covers and what it does not. Left in the automatic suite rather than made opt-in, because
# a manual "does the repo build" check is one nobody runs until it is already broken.
add_test(NAME configure
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/configure_check.sh
)
set_tests_properties(configure PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 1800)

# AND DOES EVERY CHECK IN tools/ ACTUALLY RUN. The configure check above proves cmake can read
# this file; it says nothing about what this file forgot to mention. 43 of 101 check scripts were
# never registered — 37 of them passing, so they were working checks nobody was running.
add_test(NAME doc_citation
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/doc_citation_check.sh
)
set_tests_properties(doc_citation PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

add_test(NAME meter_publish
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/meter_publish_check.sh
)
set_tests_properties(meter_publish PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

add_test(NAME pdc_window
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/pdc_window_check.sh
)
set_tests_properties(pdc_window PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 600)

add_test(NAME clip_reject
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/clip_reject_check.sh
)
set_tests_properties(clip_reject PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

add_test(NAME progress_doc
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/progress_check.sh
)
set_tests_properties(progress_doc PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

add_test(NAME hazard_order
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/hazard_order_check.sh
)
set_tests_properties(hazard_order PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

add_test(NAME deps_order
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/deps_order_check.sh
)
set_tests_properties(deps_order PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

add_test(NAME check_registry
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_registry_check.sh
)
set_tests_properties(check_registry PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

# SetClipText (98): a clip's name and its audio source path, the last two persisted_field_reach
# GAPs. Renders offline, so no audio device.
add_test(NAME clip_text
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/clip_text_check.sh
)
set_tests_properties(clip_text PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 600)

# SetMarkerColor (99): a marker's colour was write-once. Model and shared memory only.
add_test(NAME marker_color
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/marker_color_check.sh
)
set_tests_properties(marker_color PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 600)

# The sampler's voice counters — published in UiSamplerKitRegion and asserted by nothing until
# this check existed. Reads the published region, so it needs no working output device.
add_test(NAME voice_telemetry
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/voice_telemetry_check.sh
)
set_tests_properties(voice_telemetry PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

# THE SIX CAPTURE-DEPENDENT CHECKS, registered 2026-08-02 — the first time they could run.
# They assert on a LIVE capture through DAW_CAPTURE_WAV, so they need an output device that
# actually runs its playback callback. This machine's did not for days (coreaudiod had been up 85
# days; restarting it fixed the device), and they were declared unrunnable in
# tools/check_registry_check.sh rather than left silently unregistered.
#
# All six pass. They are device-dependent by nature, so they are the first thing to go red if the
# device dies again — which is correct: that is a real loss of coverage and should be loud.
add_test(NAME level_match_bypass
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/level_match_bypass_check.sh
)
set_tests_properties(level_match_bypass PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

add_test(NAME master_fx
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/master_fx_check.sh
)
set_tests_properties(master_fx PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

add_test(NAME midi_per_bus
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/midi_per_bus_check.sh
)
set_tests_properties(midi_per_bus PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

add_test(NAME panic
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/panic_check.sh
)
set_tests_properties(panic PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

add_test(NAME preview_note
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/preview_note_check.sh
)
set_tests_properties(preview_note PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

add_test(NAME sidechain
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/sidechain_check.sh
)
set_tests_properties(sidechain PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)


add_test(NAME lint
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/lint_check.sh
)
set_tests_properties(lint PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

add_test(NAME markers
  COMMAND markers_tests
)
set_tests_properties(markers PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

add_test(NAME sampler_envelope
  COMMAND sampler_envelope_tests
)
set_tests_properties(sampler_envelope PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

add_test(NAME sampler_check
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/sampler_check.sh
)
set_tests_properties(sampler_check PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

add_test(NAME sampler_load
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/sampler_load_check.sh
)
set_tests_properties(sampler_load PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

add_test(NAME module
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/module_check.sh
)
set_tests_properties(module PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

# A module carries the SOUND of its plugins, not just their names. `module` cannot see this: its
# fixture is a sampler, which has no opaque state, so every property it asserts passed while
# `.uni` packed no plugin state at all.
add_test(NAME module_state
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/module_state_check.sh
)
set_tests_properties(module_state PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

add_test(NAME sampler_chop
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/sampler_chop_check.sh
)
set_tests_properties(sampler_chop PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

add_test(NAME sampler_slice_stability
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/sampler_slice_stability_check.sh
)
set_tests_properties(sampler_slice_stability PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

add_test(NAME sampler_sound_address
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/sampler_sound_address_check.sh
)
set_tests_properties(sampler_sound_address PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

add_test(NAME sampler_kit
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/sampler_kit_check.sh
)
set_tests_properties(sampler_kit PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

# A sampler built entirely by commands sounds, with no envelope sent.
add_test(NAME sampler_default_sound
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tools/sampler_default_sound_check.sh
)
set_tests_properties(sampler_default_sound PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

# A sound-addressed-only track plays a DIFFERENT slot for the same note (section 8 Q2).
add_test(NAME sound_addressed
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tools/sound_addressed_check.sh
)
set_tests_properties(sound_addressed PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

# A refused sampler command tells the CALLER, not just the engine's log.
add_test(NAME sampler_reject
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tools/sampler_reject_check.sh
)
set_tests_properties(sampler_reject PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

# The retrigger ramp is heard, and a conditional trig skips the right passes — reproducibly.
add_test(NAME elektron_ops
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tools/elektron_ops_check.sh
)
set_tests_properties(elektron_ops PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

# The bank's gate default seeds a NEW slot and leaves the existing ones alone (opcode 88).
add_test(NAME sampler_default_gate
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tools/sampler_default_gate_check.sh
)
set_tests_properties(sampler_default_gate PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

# A pad can be repointed at another sample, and you hear the other sample.
add_test(NAME slot_repoint
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tools/slot_repoint_check.sh
)
set_tests_properties(slot_repoint PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

# A folded track stays folded — the state can be SET, not only drawn.
add_test(NAME track_collapse
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tools/track_collapse_check.sh
)
set_tests_properties(track_collapse PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

# Nothing the project format remembers is unwritable without a reason recorded.
add_test(NAME persisted_field_reach
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tools/persisted_field_reach_check.sh
)
set_tests_properties(persisted_field_reach PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

# The ops column's width is a property of the TRACK, not of what is on screen.
add_test(NAME ops_width
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tools/ops_width_check.sh
)
set_tests_properties(ops_width PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

# The wait library's own diagnosis is correct — thirty-five checks depend on it to say WHY.
add_test(NAME engine_wait_selftest
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tools/engine_wait_selftest.sh
)
set_tests_properties(engine_wait_selftest PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

# A new clip lands on the bar the RULER draws, including after the meter changes.
add_test(NAME clip_anchor_meter
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tools/clip_anchor_meter_check.sh
)
set_tests_properties(clip_anchor_meter PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

# A pad has a name, you can read it, and you can change it.
add_test(NAME slot_rename
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tools/slot_rename_check.sh
)
set_tests_properties(slot_rename PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

# The slices of a chop tile the source — no gap, no overlap, nothing off the end.
add_test(NAME slice_extent
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tools/slice_extent_check.sh
)
set_tests_properties(slice_extent PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

# A note's default length follows the SONG's meter, not a hardcoded 4/4 bar.
add_test(NAME meter_bar
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tools/meter_bar_check.sh
)
set_tests_properties(meter_bar PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

# Every published patcher node names the device it came from.
add_test(NAME patcher_node_owner
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tools/patcher_node_owner_check.sh
)
set_tests_properties(patcher_node_owner PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

# A gated note that fills its clip still sounds, and so does one shorter than a block.
add_test(NAME full_length_note
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tools/full_length_note_check.sh
)
set_tests_properties(full_length_note PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

# The sample loop is settable by command and audible when it is.
add_test(NAME sampler_loop
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tools/sampler_loop_check.sh
)
set_tests_properties(sampler_loop PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

# The READ direction's ratchet, the one op_registry and persisted_field_reach did not cover:
# every per-track field the engine publishes has a reader, and the values come back in the units
# they were SENT in — a name comparison would pass on a read-back that leaked the wire's swing
# bias, which is exactly what its negative control demonstrates.
# A CLIP's own subdivision and meter — task #43 phase 2. The load-bearing property is PUBLISHED
# rather than saved: the first version of this command saved correctly and drew nothing, because
# the extent publish is gated on the clip version and a grid change touches no note.
# NAMED FOR THE SCRIPT, not for the feature: `clip_grid` was already taken by the unit binary
# 200 lines down, and CMake refuses a duplicate test name at CONFIGURE time — so main did not
# configure at all from a cold cache, while every warm tree (mine) built and tested green.
add_test(NAME clip_grid_check
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/clip_grid_check.sh
)
set_tests_properties(clip_grid_check PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

# An AUDIO clip's in-point, gain and fades (opcode 95). Three layers have to move together and the
# check asserts each one separately: the descriptor table (which was a load-time snapshot until
# this command existed), the baked audio render (which the table cannot see), and the saved file.
# The HEARD property renders offline, so no sound card is involved.
add_test(NAME audio_clip_field
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/audio_clip_field_check.sh
)
set_tests_properties(audio_clip_field PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

# The MASTER strip is a track like the others: its fader moves uiMixerVersion, and its meter
# reports the sum it produced. Both were reported by the web-UI agent against PR #19 — a value
# published correctly with nothing saying it changed, and a meter hardcoded to zero. The meter
# property runs an OFFLINE RENDER, so no audio device is involved.
add_test(NAME master_mixer
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/master_mixer_check.sh
)
set_tests_properties(master_mixer PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

# An automation point can be DELETED (opcode 96). Opcode 60 creates one and re-values one, and
# nothing removed one, so a lane was draw-only and a UI drawing it had no eraser. Addressing is
# asserted from both ends because the handler walks lanes and points, and an emptied lane is
# removed rather than left as an empty clip that reappears on reload.
add_test(NAME automation_delete
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/automation_delete_check.sh
)
set_tests_properties(automation_delete PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

# A chord's STRUM and HUMANIZE. All three fields persist and the scheduler reads them; daw-cli
# sent the literal zero for every one, so no project could contain a strummed chord unless it was
# hand-edited. Asserts on chord.scheduled rather than on audio, because a chord currently renders
# SILENT — a defect that predates this and is documented in the check's header.
add_test(NAME chord_expression
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/chord_expression_check.sh
)
set_tests_properties(chord_expression PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

# One modulator's ENVELOPE SHAPE, on request (opcode 97, kShmVersion 37). Opcode 84 could write a
# full multi-segment envelope and nothing could read one back, so a pencil editor would have been
# write-only. Asserts every field the write takes — a read-back that returns a subset is how such
# an editor silently destroys the loop ranges and the release fade it was never told about.
add_test(NAME sampler_envelope_readback
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/sampler_envelope_readback_check.sh
)
set_tests_properties(sampler_envelope_readback PROPERTIES
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

# ---------------------------------------------------------------------------------------------
# THIRTY-SEVEN CHECKS THAT EXISTED AND NEVER RAN.
#
# `tools/` held 101 *_check.sh and CMakeLists registered 58. The other 43 ran only when somebody
# remembered them by name — which is how the web-UI agent found 27 of their own suites unrun on
# the same day, and it is the same defect: a check nobody runs is a check that rots, and it rots
# silently because nothing reports the absence of a result.
#
# All 37 below were run and PASS. They were not excluded for a reason; they were never wired up.
# The remaining 6 need a live audio capture and are DECLARED in tools/check_registry_check.sh
# with that reason, so the exclusion list can shrink and cannot silently grow.
#
# Registered with a generous timeout: several launch an engine with its own --run-seconds
# lifetime, and a timeout tighter than that lifetime turns a slow machine into a red suite.
add_test(NAME add_remove_track
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/add_remove_track_check.sh
)
set_tests_properties(add_remove_track PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 600)
add_test(NAME arrangement
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/arrangement_check.sh
)
set_tests_properties(arrangement PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 600)
add_test(NAME audio_clip_playback
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/audio_clip_playback_check.sh
)
set_tests_properties(audio_clip_playback PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 600)
add_test(NAME audio_loop
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/audio_loop_check.sh
)
set_tests_properties(audio_loop PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 600)
add_test(NAME audio_stability
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/audio_stability_check.sh
)
# SKIP_RETURN_CODE 77: this check compares a loaded run against a deeper-pipelined one, and every
# assertion it makes is satisfied by the load never biting. When depth 4 reports zero starves it
# now exits 77 and ctest prints SKIP — because "measured nothing" and "the lever works" must not
# both print PASS. It is the first entry here with a skip code; the rest still cannot say it.
set_tests_properties(audio_stability PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 600
                     SKIP_RETURN_CODE 77)
add_test(NAME automation
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/automation_check.sh
)
set_tests_properties(automation PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 600)
add_test(NAME automation_readback
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/automation_readback_check.sh
)
set_tests_properties(automation_readback PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 600)
add_test(NAME child_track
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/child_track_check.sh
)
set_tests_properties(child_track PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 600)
add_test(NAME cross_track_move
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/cross_track_move_check.sh
)
set_tests_properties(cross_track_move PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 600)
add_test(NAME edited_roundtrip
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/edited_roundtrip_check.sh
)
set_tests_properties(edited_roundtrip PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 600)
add_test(NAME extent_truncation
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/extent_truncation_check.sh
)
set_tests_properties(extent_truncation PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 600)
add_test(NAME instrument_meter
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/instrument_meter_check.sh
)
set_tests_properties(instrument_meter PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 600)
# NAMED FOR THE SCRIPT: `lane_quantize` is already the unit binary's test name, and CMake refuses
# a duplicate at CONFIGURE time — the same collision `clip_grid` had, caught here by
# configure_check on the first try rather than by a colleague on a cold clone.
add_test(NAME lane_quantize_check
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/lane_quantize_check.sh
)
set_tests_properties(lane_quantize_check PROPERTIES
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 600)
add_test(NAME lock_order
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/lock_order_check.sh
)
set_tests_properties(lock_order PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 600)
add_test(NAME master_track
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/master_track_check.sh
)
set_tests_properties(master_track PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 600)
add_test(NAME modlink
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/modlink_check.sh
)
set_tests_properties(modlink PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 600)
add_test(NAME multi_producer_ring
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/multi_producer_ring_check.sh
)
set_tests_properties(multi_producer_ring PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 600)
add_test(NAME multiout
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/multiout_check.sh
)
set_tests_properties(multiout PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 600)
add_test(NAME multiout_persist
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/multiout_persist_check.sh
)
set_tests_properties(multiout_persist PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 600)
add_test(NAME offline_render
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/offline_render_check.sh
)
set_tests_properties(offline_render PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 600)
add_test(NAME override
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/override_check.sh
)
set_tests_properties(override PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 600)
add_test(NAME param_metadata
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/param_metadata_check.sh
)
set_tests_properties(param_metadata PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 600)
add_test(NAME patcher_device_edit
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/patcher_device_edit_check.sh
)
set_tests_properties(patcher_device_edit PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 600)
add_test(NAME patcher_device_migration
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/patcher_device_migration_check.sh
)
set_tests_properties(patcher_device_migration PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 600)
add_test(NAME patcher_pool_scoping
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/patcher_pool_scoping_check.sh
)
set_tests_properties(patcher_pool_scoping PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 600)
add_test(NAME patcher_save_guard
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/patcher_save_guard_check.sh
)
set_tests_properties(patcher_save_guard PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 600)
add_test(NAME pdc_alignment
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/pdc_alignment_check.sh
)
set_tests_properties(pdc_alignment PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 600)
add_test(NAME per_track_version
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/per_track_version_check.sh
)
set_tests_properties(per_track_version PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 600)
add_test(NAME placement_ops
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/placement_ops_check.sh
)
set_tests_properties(placement_ops PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 600)
add_test(NAME ripple_undo
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/ripple_undo_check.sh
)
set_tests_properties(ripple_undo PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 600)
add_test(NAME save_roundtrip
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/save_roundtrip_check.sh
)
set_tests_properties(save_roundtrip PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 600)
add_test(NAME scratch_clip
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/scratch_clip_check.sh
)
set_tests_properties(scratch_clip PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 600)
add_test(NAME song_end_loop
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/song_end_loop_check.sh
)
set_tests_properties(song_end_loop PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 600)
add_test(NAME stereo_audio
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/stereo_audio_check.sh
)
set_tests_properties(stereo_audio PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 600)
add_test(NAME surround
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/surround_check.sh
)
set_tests_properties(surround PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 600)
add_test(NAME tempo_map_audio
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/tempo_map_audio_check.sh
)
set_tests_properties(tempo_map_audio PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 600)
add_test(NAME track_edit
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/track_edit_check.sh
)
set_tests_properties(track_edit PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 600)


# The kit read-back covers the slot the engine publishes. A NAMES-only ratchet, deliberately —
# the behaviour is driven by sampler_kit, sampler_vintage, slice_extent and slot_rename, and this
# is the guard those do not provide. Costs no engine, so it is nearly free.
add_test(NAME sampler_kit_fields
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/sampler_kit_fields_check.sh
)
set_tests_properties(sampler_kit_fields PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

# Every published ROW OP can be read back, and the note id that addresses one is visible —
# set-row-ops is addressed by an id `get notes` did not print, so the write path needed an
# identifier the read path would not give you.
add_test(NAME note_readback
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/note_readback_check.sh
)
set_tests_properties(note_readback PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

add_test(NAME track_readback
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/track_readback_check.sh
)
set_tests_properties(track_readback PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

# Save and load are two hand-written lists joined by a string literal typed twice. A field saved and
# never loaded round-trips as its DEFAULT, and the next save writes that default back — the user's
# setting destroyed by opening and saving, with the save side working perfectly throughout.
add_test(NAME roundtrip_reach
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/roundtrip_reach_check.sh
)
set_tests_properties(roundtrip_reach PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 120)

# The C++ -> JS bit constants were the one contract boundary with no mechanical check, and one had
# diverged: FLAG_ALLOW_OVERLAP held bit 3 (sound-addressed) while claiming bit 4. Rust was right.
add_test(NAME js_flag_mirror
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/js_flag_mirror_check.sh
)
set_tests_properties(js_flag_mirror PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 120)

# The patcher pool is GLOBAL and ownership is a render-time filter, so a track that owns no node
# must run none. It ran all of them: a two-track project played track 0's generated notes out of
# track 1's sampler, which held no patcher, no clip and no note.
add_test(NAME patcher_track_scope
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/patcher_track_scope_check.sh
)
set_tests_properties(patcher_track_scope PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

# A control the UI cannot READ is write-only, whatever the command does. Covers the published
# readback of SetTrackHarmonyQuantize (10) and the reported outcome of SavePatcherPreset (29).
#
# THIS ENTRY IS THE POINT OF THE COMMIT THAT ADDED IT. The script has existed and PASSED for
# weeks while being named in this file zero times: check_registry_check.sh asked "is the name a
# substring of the CMake text", and `readback_check.sh` is a suffix of the four *_readback_check.sh
# entries around it. The ratchet built to find unrun checks reported the only unrun check as
# registered, and three separate commits improved a script that produced no result.
add_test(NAME readback
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/readback_check.sh
)
set_tests_properties(readback PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

# A sampler's audio is addressable for drawing, and it is the RIGHT audio. The load-bearing
# property is the source NO audio clip names: every other assertion here passes with the sampler's
# interning deleted, because the clip path covers for it.
add_test(NAME sampler_waveform
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/sampler_waveform_check.sh
)
set_tests_properties(sampler_waveform PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

# Entering a note over a sounding one no longer has to destroy it. The last property renders the
# default-NNA case on purpose: turning the lane flag on and testing with a Cut slot sounds like no
# change at all, which is the false negative this check exists to pin down.
add_test(NAME note_overlap
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/note_overlap_check.sh
)
set_tests_properties(note_overlap PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

# A lane's subdivision is settable at last, and it is the LANE's — a one-track fixture cannot
# tell "set the track I asked for" from "set all of them".
add_test(NAME lines_per_beat
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/lines_per_beat_check.sh
)
set_tests_properties(lines_per_beat PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

# A generated note chooses its slice, and chooses the same one at any buffer size.
add_test(NAME slice_select
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tools/slice_select_check.sh
)
set_tests_properties(slice_select PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

# Zero is not a device id: a sampler that is the FIRST device on its track must be played.
add_test(NAME device_id_zero
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tools/device_id_zero_check.sh
)
set_tests_properties(device_id_zero PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

# The version on a kit answer describes that answer, and a reader gets the track it asked for.
add_test(NAME kit_readback_version
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tools/kit_readback_version_check.sh
)
set_tests_properties(kit_readback_version PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

# Every part of a chopped file is reachable, and a new sampler exists before it is loaded into.
add_test(NAME sampler_chop_reach
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tools/sampler_chop_reach_check.sh
)
set_tests_properties(sampler_chop_reach PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

# A sampler edit must not free the snapshot its sounding voices are reading.
add_test(NAME sampler_edit_while_playing
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tools/sampler_edit_while_playing_check.sh
)
set_tests_properties(sampler_edit_while_playing PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

# The filter can be turned on at all — the field nothing could write — and it is audible.
add_test(NAME sampler_filter
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tools/sampler_filter_check.sh
)
set_tests_properties(sampler_filter PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

# Bit depth and rate reduction are two DIFFERENT effects, identified by opposite fingerprints:
# bits collapse the distinct-value count, rate collapses the transition count. Asserting only
# that vintage "sounds different" would pass on an engine that applied one where the other was
# asked for.
add_test(NAME sampler_vintage
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/sampler_vintage_check.sh
)
set_tests_properties(sampler_vintage PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

add_test(NAME sampler_determinism
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/sampler_determinism_check.sh
)
set_tests_properties(sampler_determinism PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

# Four offline renders, one of which is the negative control that gives the others teeth.
add_test(NAME sampler_stem
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/sampler_stem_check.sh
)
set_tests_properties(sampler_stem PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

# 8 is the ctest size: enough tracks to show whether cost grows linearly, small enough not to
# dominate the suite. Run it by hand with a larger argument to find the actual ceiling.
add_test(NAME producer_load
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/producer_load_check.sh 8
)
set_tests_properties(producer_load PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

# A patcher generator driving the built-in sampler — a path that did not exist at all until the
# note tee was added to the patcher's event-resolution route.
add_test(NAME patcher_plays_sampler
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/patcher_plays_sampler_check.sh
)
set_tests_properties(patcher_plays_sampler PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

# The pool through a REAL DEVICE in real time — the one place a late thread actually costs
# something. Skips cleanly (exit 0, with a reason) on a machine with no audio device.
# 24 tracks, not 8: the pool engages on the WORK now, and at 8 one thread has room to spare so
# the adaptive rule correctly declines — the check would be comparing serial against serial.
add_test(NAME realtime_pool
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/realtime_pool_check.sh 24
)
set_tests_properties(realtime_pool PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

# The inward bulk carrier, verified by something AUDIBLE that came across it — a byte count
# proves the transport, a drawn shape you can hear proves the whole path.
add_test(NAME bulk_carrier
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/bulk_carrier_check.sh
)
set_tests_properties(bulk_carrier PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

# A live engine sets the envelope, then the saved project is rendered OFFLINE and the audio is
# measured — a check that read the numbers back would pass with the envelope never reaching a voice.
add_test(NAME sampler_envelope_write
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/sampler_envelope_write_check.sh
)
set_tests_properties(sampler_envelope_write PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

# A live engine, but it asserts on the DOCUMENT rather than on audio, so it needs no device.
add_test(NAME row_ops_write
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/row_ops_write_check.sh
)
set_tests_properties(row_ops_write PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

# The thread count must not be observable in the output. Byte-for-byte, not a tolerance.
add_test(NAME render_pool
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/render_pool_check.sh
)
set_tests_properties(render_pool PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

add_test(NAME sampler_state
  COMMAND sampler_state_tests
)
set_tests_properties(sampler_state PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

add_test(NAME sampler_voice
  COMMAND sampler_voice_tests
)
set_tests_properties(sampler_voice PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

add_test(NAME zip_container
  COMMAND zip_container_tests
)
set_tests_properties(zip_container PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

add_test(NAME sampler_slice
  COMMAND sampler_slice_tests
)
set_tests_properties(sampler_slice PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

add_test(NAME sampler_dsp
  COMMAND sampler_dsp_tests
)
set_tests_properties(sampler_dsp PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

add_test(NAME sampler_engine
  COMMAND sampler_engine_tests
)
set_tests_properties(sampler_engine PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

add_test(NAME placement_adds
  COMMAND placement_adds_tests
)
set_tests_properties(placement_adds PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

add_test(NAME time_signature
  COMMAND time_signature_tests
)
set_tests_properties(time_signature PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

add_test(NAME tempo_integral
  COMMAND tempo_integral_tests
)
set_tests_properties(tempo_integral PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

add_test(NAME lane_quantize
  COMMAND lane_quantize_tests
)
set_tests_properties(lane_quantize PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

add_test(NAME engine_track_rebuild
  COMMAND engine_track_rebuild_tests
)

add_test(NAME engine_clip_edit
  COMMAND engine_clip_edit_tests
)

add_test(NAME engine_bulk_edit
  COMMAND engine_bulk_edit_tests
)

add_test(NAME engine_track_setup
  COMMAND engine_track_setup_tests
)

add_test(NAME engine_pure
  COMMAND engine_pure_tests
)
set_tests_properties(engine_pure PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

add_test(NAME engine_patcher_commands
  COMMAND engine_patcher_commands_tests
)
set_tests_properties(engine_patcher_commands PROPERTIES
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

add_test(NAME engine_modlink_commands
  COMMAND engine_modlink_commands_tests
)
set_tests_properties(engine_modlink_commands PROPERTIES
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

add_test(NAME engine_startup
  COMMAND engine_startup_tests
)
set_tests_properties(engine_startup PROPERTIES
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

add_test(NAME engine_sampler_commands
  COMMAND engine_sampler_commands_tests
)
set_tests_properties(engine_sampler_commands PROPERTIES
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

add_test(NAME engine_automation_commands
  COMMAND engine_automation_commands_tests
)
set_tests_properties(engine_automation_commands PROPERTIES
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

add_test(NAME engine_rt_helpers
  COMMAND engine_rt_helpers_tests
)
set_tests_properties(engine_rt_helpers PROPERTIES
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

add_test(NAME clip_grid
  COMMAND clip_grid_tests
)
set_tests_properties(clip_grid PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

add_test(NAME waveform_fixture
  COMMAND waveform_fixture_tests ${CMAKE_CURRENT_SOURCE_DIR}/presets/audio
)
set_tests_properties(waveform_fixture PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

add_test(NAME device_chain_defaults
  COMMAND device_chain_tests
)
set_tests_properties(device_chain_defaults PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

add_test(NAME device_chain_ui
  COMMAND device_chain_ui_tests
)
set_tests_properties(device_chain_ui PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

add_test(NAME device_chain_ui_live
  COMMAND device_chain_ui_live_tests
)
set_tests_properties(device_chain_ui_live PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

# Where JUCE actually put the Identity bundle.
#
# This was spelled ${CMAKE_BINARY_DIR}/identity_plugin_artefacts/VST3/Identity.vst3
# — the single-configuration layout. A build that carries a configuration (this
# one writes RelWithDebInfo/) puts the bundle one directory deeper, so the path
# named a file that was not there, the host loaded 0 plugins from 1 path, and
# eight of the thirty-two tests failed with symptoms about parameters and notes
# rather than about a missing file.
#
# daw_engine_main.cpp already learned this — it searches several roots, and its
# comment mentions "a leftover identity_plugin_artefacts/VST3 from a much earlier
# build". The test registration never did. Asking the target where it put itself
# is the version that cannot drift: it is correct under every generator, and it
# fails at configure time rather than at run time if the target is renamed.
set(IDENTITY_VST3 "$<TARGET_BUNDLE_DIR:identity_plugin_VST3>")

add_test(NAME phase2_impulse
  COMMAND phase2_tests --test impulse
          --plugin ${IDENTITY_VST3}
)
set_tests_properties(phase2_impulse PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

add_test(NAME phase2_param_priority
  COMMAND phase2_tests --test param_priority
          --plugin ${IDENTITY_VST3}
)
set_tests_properties(phase2_param_priority PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

add_test(NAME phase2_param_targeting
  COMMAND phase2_tests --test param_targeting
          --plugin ${IDENTITY_VST3}
)
set_tests_properties(phase2_param_targeting PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

add_test(NAME phase2_chaos
  COMMAND phase2_tests --test chaos
          --plugin ${IDENTITY_VST3}
)
set_tests_properties(phase2_chaos PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

add_test(NAME phase2_ui_visual
  COMMAND phase2_tests --test ui_visual
          --plugin ${IDENTITY_VST3}
)
set_tests_properties(phase2_ui_visual PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

add_executable(phase3_tests
  apps/phase3_tests_main.cpp
  apps/audio_shm.cpp
  apps/clip_edit.cpp
  apps/chord_resolver.cpp
  apps/event_ring.cpp
  apps/host_controller.cpp
  apps/ipc_io.cpp
  apps/harmony_timeline.cpp
  apps/scale_library.cpp
  apps/ui_snapshot.cpp
  apps/shared_memory.cpp
)

target_include_directories(phase3_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(phase3_tests PRIVATE platform_juce)

add_test(NAME phase3_timebase
  COMMAND phase3_tests --test timebase
          --plugin ${IDENTITY_VST3}
)
set_tests_properties(phase3_timebase PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

add_test(NAME phase3_scheduler_ring
  COMMAND phase3_tests --test scheduler_ring
          --plugin ${IDENTITY_VST3}
)
set_tests_properties(phase3_scheduler_ring PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

add_test(NAME phase3_automation_ring
  COMMAND phase3_tests --test automation_ring
          --plugin ${IDENTITY_VST3}
)
set_tests_properties(phase3_automation_ring PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

add_test(NAME phase3_audio_region
  COMMAND phase3_tests --test audio_region
)
set_tests_properties(phase3_audio_region PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

add_test(NAME audio_io
  COMMAND audio_io_tests
)
set_tests_properties(audio_io PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

add_test(NAME phase3_harmony_quantize
  COMMAND phase3_tests --test harmony_quantize
)
set_tests_properties(phase3_harmony_quantize PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

add_test(NAME phase3_chord_expansion
  COMMAND phase3_tests --test chord_expansion
)
set_tests_properties(phase3_chord_expansion PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

add_test(NAME phase3_humanize_determinism
  COMMAND phase3_tests --test humanize_determinism
)
set_tests_properties(phase3_humanize_determinism PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

add_test(NAME phase3_harmony_order
  COMMAND phase3_tests --test harmony_order
)
set_tests_properties(phase3_harmony_order PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

add_test(NAME phase3_snapshot
  COMMAND phase3_tests --test snapshot
)
set_tests_properties(phase3_snapshot PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

add_test(NAME phase3_undo_stack
  COMMAND phase3_tests --test undo_stack
)
set_tests_properties(phase3_undo_stack PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

add_test(NAME phase3_note_duration
  COMMAND phase3_tests --test note_duration
)
set_tests_properties(phase3_note_duration PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

add_test(NAME phase3_row_op_probability
  COMMAND phase3_tests --test row_op_probability
)
set_tests_properties(phase3_row_op_probability PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

add_test(NAME phase3_row_op_expand
  COMMAND phase3_tests --test row_op_expand
)
set_tests_properties(phase3_row_op_expand PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

add_test(NAME phase3_placement_schedule
  COMMAND phase3_tests --test placement_schedule
)
set_tests_properties(phase3_placement_schedule PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

add_test(NAME phase3_note_entry
  COMMAND phase3_tests --test note_entry
)
set_tests_properties(phase3_note_entry PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

add_test(NAME phase3_resync_mismatch
  COMMAND phase3_tests --test resync_mismatch
)
set_tests_properties(phase3_resync_mismatch PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

add_test(NAME phase3_pulse_full
  COMMAND phase3_tests --test pulse_full
          --plugin ${IDENTITY_VST3}
)
set_tests_properties(phase3_pulse_full PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

add_test(NAME phase3_note_off_full
  COMMAND phase3_tests --test note_off_full
          --plugin ${IDENTITY_VST3}
)
set_tests_properties(phase3_note_off_full PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

add_test(NAME phase3_resurrection_full
  COMMAND phase3_tests --test resurrection_full
          --plugin ${IDENTITY_VST3}
)
set_tests_properties(phase3_resurrection_full PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)

add_test(NAME phase3_composition_full
  COMMAND phase3_tests --test composition_full
          --plugin ${IDENTITY_VST3}
)
set_tests_properties(phase3_composition_full PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR} TIMEOUT 300)


# NOTE: this MUST stay at the very END of this file. `if (TARGET ...)` is false for a
# target that has not been declared yet and silently skips it — when this loop lived
# above enable_testing(), phase3_tests (declared 130 lines later, and the largest test
# target here) was skipped, so the fix for "assertions were compiled out" left the
# biggest suite still building with -DNDEBUG while the CMake read as though it did not.
# assert() is the assertion mechanism in several of these test mains, and CMake's
# RelWithDebInfo (the default build here) passes -DNDEBUG, which compiles every one of
# them away. Those binaries then run to completion and exit 0 having verified NOTHING —
# 161 assertions across four files were dead when this was noticed. Undefine NDEBUG for
# test targets only, so a test that asserts actually asserts.
foreach(daw_test_target
    patcher_tests
    patcher_graph_tests
    project_file_tests
    waveform_pyramid_tests
    waveform_store_tests
    waveform_fixture_tests
    clip_grid_tests
    lane_quantize_tests
    tempo_integral_tests
    time_signature_tests
    placement_adds_tests
    markers_tests
    sampler_envelope_tests
    sampler_state_tests
    sampler_voice_tests
    sampler_engine_tests
    sampler_dsp_tests
    sampler_slice_tests
    zip_container_tests
    event_ring_mpsc_tests
    audio_io_tests
    device_chain_tests
    device_chain_ui_tests
    device_chain_ui_live_tests
    phase2_tests
    phase3_tests)
  if (TARGET ${daw_test_target})
    target_compile_options(${daw_test_target} PRIVATE -UNDEBUG)
  endif()
endforeach()