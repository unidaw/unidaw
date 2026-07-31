cmake_minimum_required(VERSION 3.18)

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

add_executable(daw_engine
  apps/daw_engine_main.cpp
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

target_include_directories(daw_engine PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${Boost_INCLUDE_DIRS}
)

target_link_libraries(daw_engine PRIVATE platform_juce)

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
set_tests_properties(contract_freshness PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# Every hand-written Rust mirror is pinned to the C++ struct bindgen generates from the header.
add_test(NAME contract_layout
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tools/contract_layout_check.sh
)
set_tests_properties(contract_layout PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# THE RUST SIDE'S OWN TESTS, which had never run here. contract_layout proves every mirror is
# LISTED in bindgen_matches_hand_written; that test is what actually compares the bytes, and it
# only ever ran under `cargo test`. So the byte-level guard was absent from the suite that gates
# every commit — the list was checked and the thing the list points at was not.
add_test(NAME bridge_layout
  COMMAND cargo test --manifest-path ${CMAKE_CURRENT_SOURCE_DIR}/ui/Cargo.toml -p daw-bridge
)
set_tests_properties(bridge_layout PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME patcher_resolution
  COMMAND patcher_tests
)
set_tests_properties(patcher_resolution PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME patcher_graph_edits
  COMMAND patcher_graph_tests
)
set_tests_properties(patcher_graph_edits PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME project_file_round_trip
  COMMAND project_file_tests ${CMAKE_CURRENT_SOURCE_DIR}/presets/projects
)
set_tests_properties(project_file_round_trip PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME waveform_pyramid
  COMMAND waveform_pyramid_tests
)
set_tests_properties(waveform_pyramid PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME waveform_store
  COMMAND waveform_store_tests
)
set_tests_properties(waveform_store PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME event_ring_mpsc
  COMMAND event_ring_mpsc_tests
)
set_tests_properties(event_ring_mpsc PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# M2.20: the linter's own coverage — every rule fires, clean input is silent, output is
# deterministic. Pure file in / text out, so it needs no audio device and belongs here.
# M2.21: every opcode has a CLI path or a declared reason it does not. Pure source
# analysis, so it belongs in ctest.
add_test(NAME op_registry
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/op_registry_check.sh
)
set_tests_properties(op_registry PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME lint
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/lint_check.sh
)
set_tests_properties(lint PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME markers
  COMMAND markers_tests
)
set_tests_properties(markers PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME sampler_envelope
  COMMAND sampler_envelope_tests
)
set_tests_properties(sampler_envelope PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME sampler_check
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/sampler_check.sh
)
set_tests_properties(sampler_check PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME sampler_load
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/sampler_load_check.sh
)
set_tests_properties(sampler_load PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME module
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/module_check.sh
)
set_tests_properties(module PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME sampler_chop
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/sampler_chop_check.sh
)
set_tests_properties(sampler_chop PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME sampler_slice_stability
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/sampler_slice_stability_check.sh
)
set_tests_properties(sampler_slice_stability PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME sampler_sound_address
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/sampler_sound_address_check.sh
)
set_tests_properties(sampler_sound_address PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME sampler_kit
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/sampler_kit_check.sh
)
set_tests_properties(sampler_kit PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# A gated note that fills its clip still sounds, and so does one shorter than a block.
add_test(NAME full_length_note
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tools/full_length_note_check.sh
)
set_tests_properties(full_length_note PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# The sample loop is settable by command and audible when it is.
add_test(NAME sampler_loop
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tools/sampler_loop_check.sh
)
set_tests_properties(sampler_loop PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# A generated note chooses its slice, and chooses the same one at any buffer size.
add_test(NAME slice_select
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tools/slice_select_check.sh
)
set_tests_properties(slice_select PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# Zero is not a device id: a sampler that is the FIRST device on its track must be played.
add_test(NAME device_id_zero
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tools/device_id_zero_check.sh
)
set_tests_properties(device_id_zero PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# The version on a kit answer describes that answer, and a reader gets the track it asked for.
add_test(NAME kit_readback_version
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tools/kit_readback_version_check.sh
)
set_tests_properties(kit_readback_version PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# Every part of a chopped file is reachable, and a new sampler exists before it is loaded into.
add_test(NAME sampler_chop_reach
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tools/sampler_chop_reach_check.sh
)
set_tests_properties(sampler_chop_reach PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# A sampler edit must not free the snapshot its sounding voices are reading.
add_test(NAME sampler_edit_while_playing
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tools/sampler_edit_while_playing_check.sh
)
set_tests_properties(sampler_edit_while_playing PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# The filter can be turned on at all — the field nothing could write — and it is audible.
add_test(NAME sampler_filter
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tools/sampler_filter_check.sh
)
set_tests_properties(sampler_filter PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME sampler_determinism
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/sampler_determinism_check.sh
)
set_tests_properties(sampler_determinism PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# Four offline renders, one of which is the negative control that gives the others teeth.
add_test(NAME sampler_stem
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/sampler_stem_check.sh
)
set_tests_properties(sampler_stem PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# 8 is the ctest size: enough tracks to show whether cost grows linearly, small enough not to
# dominate the suite. Run it by hand with a larger argument to find the actual ceiling.
add_test(NAME producer_load
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/producer_load_check.sh 8
)
set_tests_properties(producer_load PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# A patcher generator driving the built-in sampler — a path that did not exist at all until the
# note tee was added to the patcher's event-resolution route.
add_test(NAME patcher_plays_sampler
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/patcher_plays_sampler_check.sh
)
set_tests_properties(patcher_plays_sampler PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# The pool through a REAL DEVICE in real time — the one place a late thread actually costs
# something. Skips cleanly (exit 0, with a reason) on a machine with no audio device.
# 24 tracks, not 8: the pool engages on the WORK now, and at 8 one thread has room to spare so
# the adaptive rule correctly declines — the check would be comparing serial against serial.
add_test(NAME realtime_pool
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/realtime_pool_check.sh 24
)
set_tests_properties(realtime_pool PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# The inward bulk carrier, verified by something AUDIBLE that came across it — a byte count
# proves the transport, a drawn shape you can hear proves the whole path.
add_test(NAME bulk_carrier
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/bulk_carrier_check.sh
)
set_tests_properties(bulk_carrier PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# A live engine sets the envelope, then the saved project is rendered OFFLINE and the audio is
# measured — a check that read the numbers back would pass with the envelope never reaching a voice.
add_test(NAME sampler_envelope_write
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/sampler_envelope_write_check.sh
)
set_tests_properties(sampler_envelope_write PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# A live engine, but it asserts on the DOCUMENT rather than on audio, so it needs no device.
add_test(NAME row_ops_write
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/row_ops_write_check.sh
)
set_tests_properties(row_ops_write PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# The thread count must not be observable in the output. Byte-for-byte, not a tolerance.
add_test(NAME render_pool
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/render_pool_check.sh
)
set_tests_properties(render_pool PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME sampler_state
  COMMAND sampler_state_tests
)
set_tests_properties(sampler_state PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME sampler_voice
  COMMAND sampler_voice_tests
)
set_tests_properties(sampler_voice PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME zip_container
  COMMAND zip_container_tests
)
set_tests_properties(zip_container PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME sampler_slice
  COMMAND sampler_slice_tests
)
set_tests_properties(sampler_slice PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME sampler_dsp
  COMMAND sampler_dsp_tests
)
set_tests_properties(sampler_dsp PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME sampler_engine
  COMMAND sampler_engine_tests
)
set_tests_properties(sampler_engine PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME placement_adds
  COMMAND placement_adds_tests
)
set_tests_properties(placement_adds PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME time_signature
  COMMAND time_signature_tests
)
set_tests_properties(time_signature PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME tempo_integral
  COMMAND tempo_integral_tests
)
set_tests_properties(tempo_integral PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME lane_quantize
  COMMAND lane_quantize_tests
)
set_tests_properties(lane_quantize PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME clip_grid
  COMMAND clip_grid_tests
)
set_tests_properties(clip_grid PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME waveform_fixture
  COMMAND waveform_fixture_tests ${CMAKE_CURRENT_SOURCE_DIR}/presets/audio
)
set_tests_properties(waveform_fixture PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME device_chain_defaults
  COMMAND device_chain_tests
)
set_tests_properties(device_chain_defaults PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME device_chain_ui
  COMMAND device_chain_ui_tests
)
set_tests_properties(device_chain_ui PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME device_chain_ui_live
  COMMAND device_chain_ui_live_tests
)
set_tests_properties(device_chain_ui_live PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

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
set_tests_properties(phase2_impulse PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME phase2_param_priority
  COMMAND phase2_tests --test param_priority
          --plugin ${IDENTITY_VST3}
)
set_tests_properties(phase2_param_priority PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME phase2_param_targeting
  COMMAND phase2_tests --test param_targeting
          --plugin ${IDENTITY_VST3}
)
set_tests_properties(phase2_param_targeting PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME phase2_chaos
  COMMAND phase2_tests --test chaos
          --plugin ${IDENTITY_VST3}
)
set_tests_properties(phase2_chaos PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME phase2_ui_visual
  COMMAND phase2_tests --test ui_visual
          --plugin ${IDENTITY_VST3}
)
set_tests_properties(phase2_ui_visual PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

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
set_tests_properties(phase3_timebase PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME phase3_scheduler_ring
  COMMAND phase3_tests --test scheduler_ring
          --plugin ${IDENTITY_VST3}
)
set_tests_properties(phase3_scheduler_ring PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME phase3_automation_ring
  COMMAND phase3_tests --test automation_ring
          --plugin ${IDENTITY_VST3}
)
set_tests_properties(phase3_automation_ring PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME phase3_audio_region
  COMMAND phase3_tests --test audio_region
)
set_tests_properties(phase3_audio_region PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME audio_io
  COMMAND audio_io_tests
)
set_tests_properties(audio_io PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME phase3_harmony_quantize
  COMMAND phase3_tests --test harmony_quantize
)
set_tests_properties(phase3_harmony_quantize PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME phase3_chord_expansion
  COMMAND phase3_tests --test chord_expansion
)
set_tests_properties(phase3_chord_expansion PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME phase3_humanize_determinism
  COMMAND phase3_tests --test humanize_determinism
)
set_tests_properties(phase3_humanize_determinism PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME phase3_harmony_order
  COMMAND phase3_tests --test harmony_order
)
set_tests_properties(phase3_harmony_order PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME phase3_snapshot
  COMMAND phase3_tests --test snapshot
)
set_tests_properties(phase3_snapshot PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME phase3_undo_stack
  COMMAND phase3_tests --test undo_stack
)
set_tests_properties(phase3_undo_stack PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME phase3_note_duration
  COMMAND phase3_tests --test note_duration
)
set_tests_properties(phase3_note_duration PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME phase3_row_op_probability
  COMMAND phase3_tests --test row_op_probability
)
set_tests_properties(phase3_row_op_probability PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME phase3_row_op_expand
  COMMAND phase3_tests --test row_op_expand
)
set_tests_properties(phase3_row_op_expand PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME phase3_placement_schedule
  COMMAND phase3_tests --test placement_schedule
)
set_tests_properties(phase3_placement_schedule PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME phase3_note_entry
  COMMAND phase3_tests --test note_entry
)
set_tests_properties(phase3_note_entry PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME phase3_resync_mismatch
  COMMAND phase3_tests --test resync_mismatch
)
set_tests_properties(phase3_resync_mismatch PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME phase3_pulse_full
  COMMAND phase3_tests --test pulse_full
          --plugin ${IDENTITY_VST3}
)
set_tests_properties(phase3_pulse_full PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME phase3_note_off_full
  COMMAND phase3_tests --test note_off_full
          --plugin ${IDENTITY_VST3}
)
set_tests_properties(phase3_note_off_full PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME phase3_resurrection_full
  COMMAND phase3_tests --test resurrection_full
          --plugin ${IDENTITY_VST3}
)
set_tests_properties(phase3_resurrection_full PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME phase3_composition_full
  COMMAND phase3_tests --test composition_full
          --plugin ${IDENTITY_VST3}
)
set_tests_properties(phase3_composition_full PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})


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