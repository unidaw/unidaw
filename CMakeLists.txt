cmake_minimum_required(VERSION 3.18)

project(daw VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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
  apps/event_ring.cpp
  apps/host_controller.cpp
  apps/ipc_io.cpp
  apps/plugin_cache.cpp
  apps/harmony_timeline.cpp
  apps/scale_library.cpp
  apps/ui_snapshot.cpp
  apps/shared_memory.cpp
)

target_include_directories(daw_engine PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(daw_engine PRIVATE platform_juce)

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

enable_testing()

add_test(NAME phase2_impulse
  COMMAND phase2_tests --test impulse
          --plugin ${CMAKE_BINARY_DIR}/identity_plugin_artefacts/VST3/Identity.vst3
)
set_tests_properties(phase2_impulse PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME phase2_param_priority
  COMMAND phase2_tests --test param_priority
          --plugin ${CMAKE_BINARY_DIR}/identity_plugin_artefacts/VST3/Identity.vst3
)
set_tests_properties(phase2_param_priority PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME phase2_chaos
  COMMAND phase2_tests --test chaos
          --plugin ${CMAKE_BINARY_DIR}/identity_plugin_artefacts/VST3/Identity.vst3
)
set_tests_properties(phase2_chaos PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME phase2_ui_visual
  COMMAND phase2_tests --test ui_visual
          --plugin ${CMAKE_BINARY_DIR}/identity_plugin_artefacts/VST3/Identity.vst3
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
          --plugin ${CMAKE_BINARY_DIR}/identity_plugin_artefacts/VST3/Identity.vst3
)
set_tests_properties(phase3_timebase PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME phase3_scheduler_ring
  COMMAND phase3_tests --test scheduler_ring
          --plugin ${CMAKE_BINARY_DIR}/identity_plugin_artefacts/VST3/Identity.vst3
)
set_tests_properties(phase3_scheduler_ring PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME phase3_automation_ring
  COMMAND phase3_tests --test automation_ring
          --plugin ${CMAKE_BINARY_DIR}/identity_plugin_artefacts/VST3/Identity.vst3
)
set_tests_properties(phase3_automation_ring PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

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

add_test(NAME phase3_resync_mismatch
  COMMAND phase3_tests --test resync_mismatch
)
set_tests_properties(phase3_resync_mismatch PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME phase3_pulse_full
  COMMAND phase3_tests --test pulse_full
          --plugin ${CMAKE_BINARY_DIR}/identity_plugin_artefacts/VST3/Identity.vst3
)
set_tests_properties(phase3_pulse_full PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME phase3_note_off_full
  COMMAND phase3_tests --test note_off_full
          --plugin ${CMAKE_BINARY_DIR}/identity_plugin_artefacts/VST3/Identity.vst3
)
set_tests_properties(phase3_note_off_full PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME phase3_resurrection_full
  COMMAND phase3_tests --test resurrection_full
          --plugin ${CMAKE_BINARY_DIR}/identity_plugin_artefacts/VST3/Identity.vst3
)
set_tests_properties(phase3_resurrection_full PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME phase3_composition_full
  COMMAND phase3_tests --test composition_full
          --plugin ${CMAKE_BINARY_DIR}/identity_plugin_artefacts/VST3/Identity.vst3
)
set_tests_properties(phase3_composition_full PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
