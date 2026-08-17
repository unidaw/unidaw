#!/usr/bin/env python3
"""Structural and exact-evidence checker for AE-P1.2 G2-B item-18 schema v3."""

from __future__ import annotations

import copy
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
MANIFEST_PATH = ROOT / "docs/architecture/tasks/AE-P1.2-g2b-item18-manifest.json"
PROSE_PATH = ROOT / "docs/architecture/tasks/AE-P1.2-g2b-item18.md"
PREDECESSOR_MANIFEST = "docs/architecture/tasks/AE-P1.2-manifest.json"
ITEM15_MANIFEST = "docs/architecture/tasks/AE-P1.2-g2b-item15-manifest.json"
TOP_LEVEL_KEYS = {
    "schema", "ticket", "status", "owner", "revision_predecessor", "review_history",
    "predecessor", "item15", "program_source", "frozen_product", "scope",
    "implementation_authorization", "non_goals", "governed_files", "populations",
    "changed_records", "records", "test_cases",
}

EXPECTED_META = json.loads(r'''{"schema":"ae-p1.2-g2b-item18-packet/3","ticket":"AE-P1.2-G2B-ITEM18","status":"REVIEW_CANDIDATE","owner":"backend","revision_predecessor":{"packet_commit":"076fa9e8eb2161f3849473b4e7267fe59755baf4","packet_tree":"8d23e1ca83f06fb1f782f4b179eaee5df7435bb0","manifest_sha256":"cf450caa54dc102aada3fb0d681a1a24b85ba382411777ccd144088d52f84314"},"review_history":[{"packet_commit":"886544f1b933007021c1cb7a7bee65ea982fcf7f","semantic":"BLOCKED","evidence":"BLOCKED","resolution":"Schema v2 adds the omitted setDevicePatcherNodeId execution-plan mutation and Euclidean internal family, then replaces self-declared completeness with a 44-line frozen-source lexical scan and deletion/substitution controls. It also makes mirror transitions one controller-locked ticket/epoch transaction and adds fail-closed exhaustion and primer-capacity requirements."},{"packet_commit":"076fa9e8eb2161f3849473b4e7267fe59755baf4","semantic":"BLOCKED","evidence":"BLOCKED","resolution":"Schema v3 replaces the per-track segmentation-only authority with one session ExecutionSnapshot, adds machine-readable execution-consumer, sender/receiver, mapping, offline, and capacity populations, correlates ReplayComplete with a monotonic payload token, requires atomic ring batch reservation and permanent over-capacity failure, and binds track plus master dispatch to the same production gate."}],"predecessor":{"packet_commit":"2b5f0747f1b7dde79ae788af3826c49c78df5d2a","packet_tree":"7c75beb7b941c06a6099292fbf4dac6ade503a6a","manifest_sha256":"c321130b860fda73991f04d1035bea7af03faf6e030fce5565664c1657ce093e"},"item15":{"packet_commit":"8ee5b3cdd34ef6c5538fac19074b4f442c0a8514","packet_tree":"44ce562e2463ba0b6fcc291077b38fc7d87c07a1","manifest_sha256":"a9583a4cb8a8fd45d1dc2ccfb683cf06f8758c55407bfdf6f3d5a424eea4465f"},"program_source":{"commit":"4f343790562c5c9e6b43e07cacf647789856d2f4","tree":"4ff89dbca12d20a6ce46f8099a0db8b059b3af21"},"frozen_product":{"commit":"92dfdfe23cc7ff93f2ce14894a35d089e3d9e2b8","tree":"238ac970b5d61fe16055ede4c43a2978ddb11da7"},"scope":"Close AE-P1.2 G2-B item 18 with one session-level immutable ExecutionSnapshot, complete execution-consumer and dispatch-protocol populations, a production sender/receiver primer oracle, correlated replay acknowledgement, deterministic capacity failure, and track/master/offline output gates bound to the item-15 lock contract.","implementation_authorization":{"before_dual_pass":false,"after_same_sha_semantic_and_evidence_pass":true,"scope":"Only the item-15 staging change and the item-18 ExecutionSnapshot, stable-target, replay protocol, track/master dispatch, and offline-coordinator changes declared by this packet."},"non_goals":["No product source is changed by this packet.","No SetBypass host-applied acknowledgement is added; successful full-frame transmission on the ordered control stream remains the defined bypass-staging boundary.","No global mirrorOnly gate may silence unrelated tracks.","Authored TrackChain and Device values remain the document/UI authority; execution modules may not consume them after migration.","Compact host indexes, pooled patcher node ids, and owner aliases are plan-local derived values and are not persisted as authored identities.","The ReplayComplete payload and mailbox acknowledgement meaning change without moving bytes, so kControlVersion advances 14 to 15 and kShmVersion advances 41 to 42 in C++, Rust, and SHM_LAYOUT.md.","Unrelated non-chain TrackStateSnapshot fields remain outside this packet; chainDevices is removed rather than retained as a second execution authority."],"changed_records":["CTRL-MUTATIONS","CTRL-PACKET","D-PRODUCTION-FIXTURE","D-PRODUCTION-RECEIVER","DEP-FROZEN-BASE","DEP-ITEM15","DEP-PREDECESSOR","G-ITEM18","P-DISPATCH-PROTOCOL-SURFACES","P-EXECUTION-AUTHORITY-CONSUMERS","P-HOST-PLAN-MUTATIONS","P-OFFLINE-OUTPUT-SURFACES","P-READINESS-PUBLISHERS","P-SNAPSHOT-PUBLISHERS","R-ATOMIC-PRIMER-CAPACITY","R-BYPASS-STAGED","R-CORRELATED-REPLAY-ACK","R-DISPATCH-TICKET","R-G4-WITNESS","R-HOST-PLAN-AUTHORITY","R-MASTER-CORRELATION","R-MIRROR-EPOCH","R-OFFLINE-PRIMER","R-PASS4-REPLACEMENT","R-PROTOCOL-VERSION","R-R13-RECONCILIATION","R-REVIEW-GATED-AUTH","R-STABLE-DEVICE-TARGETS"]}''')
EXPECTED_GOVERNED = json.loads(r'''[{"path":"apps/automation_clip.h","sha256":"c3e1c98b910e11b94c1274450fbc104e4dd1b05c5515a52afc9ff225846e1edc"},{"path":"apps/daw_engine_main.cpp","sha256":"d0fba26cf41f2888cea989c4d5d89b8ee68ea2208dc11e76df9ef4627e085d01"},{"path":"apps/daw_lint_main.cpp","sha256":"5e50e0be8b7f50135ce27406a531f9333ebc6e92e7e3c61a6e038b3ab946fa03"},{"path":"apps/device_chain.cpp","sha256":"10e33241456abba319cbceb329cfd2fbcd53542a8dec245821f307cf736af2cb"},{"path":"apps/engine_arrangetime_commands.cpp","sha256":"c057ac38869693389551e0ab3730449af42a687ea6a1768e0ccac918e7669129"},{"path":"apps/engine_audio_callback.h","sha256":"56c9de6934744d85c0b9ddc56e9371dde29504f4434036096d51e845c6a06ef3"},{"path":"apps/engine_audio_start.cpp","sha256":"9ee3a9d275d10c1a976ea86bc2b39a273192d4d701c85f7f39eac9faffcf93ba"},{"path":"apps/engine_automation_commands.cpp","sha256":"eb2aac2707ea2b3a49009072fd4814b06ac4a29cc8d3ff33ff51f51d30bc4e3f"},{"path":"apps/engine_bulk_edit.cpp","sha256":"8b13fb2c53bb20aa3c43e0b88e272856633aa27baba81e4bb64c038d1dce5c33"},{"path":"apps/engine_chain_commands.cpp","sha256":"9b0ba55b3bca79d9fb9cc230888179cfe5779913efac131f08c9479fd32c713c"},{"path":"apps/engine_chain_host.cpp","sha256":"75bf2d3121ba4be2f81c03b2ba5ec09e99be61c56eb60c89ddc845f93d1d52fe"},{"path":"apps/engine_consumer.cpp","sha256":"6afb4be04b3fa7cc18baa9d0600cbdd814148186c6b81310c7b68a9995e4ecbc"},{"path":"apps/engine_device_commands.cpp","sha256":"f1de7d8b2ca2eee38b6cd2c26b950a3b929c046a3e677cd0593c72cf41bd3ec6"},{"path":"apps/engine_device_commands.h","sha256":"e4c4d7abb17418f2c956e89e933d26d6d14812629633750fb83e54cd49a380bd"},{"path":"apps/engine_emit_notes.cpp","sha256":"d16a0450c6f88673598bbd4a738e70b25ba030916eb6a7b5aee335c64bad4916"},{"path":"apps/engine_emit_notes.h","sha256":"e2629fb82a4cdfb813dc3ae4deb42c3b5a3743518ad37de84363d69db8d5a8cb"},{"path":"apps/engine_load_patcher_pool.cpp","sha256":"9d7ea10f49daca10224c698e6ae044c13eeddd3bbe590dc864ec11890527b532"},{"path":"apps/engine_load_project.cpp","sha256":"a7e7bee61031657c256a7ea286ad57683db44482b5dbeb4d38785c5d13d49a15"},{"path":"apps/engine_load_track.cpp","sha256":"ba1a671a82fbc81a1fd3da7edf95a82965e8b69d61bdacf5d91daf91e67f9726"},{"path":"apps/engine_master_render.cpp","sha256":"39de8ab43c58d376dbcf651a550d5513789d09c4856471ae858b381b0b5790e5"},{"path":"apps/engine_master_render.h","sha256":"a861dfc4a534d678cf634ed8a929fd375cc9a96302f6ebc019034f7e4386b662"},{"path":"apps/engine_mirror_replay.h","sha256":"08a14fa5a899666aa1f4a3410ff91c916661900e442a53acd28045819819e5c4"},{"path":"apps/engine_modlink_commands.cpp","sha256":"43c6b35a9524dc3b1648d53ef458d885c7c7dbead3b87a4e6b05fbc08cfc4af0"},{"path":"apps/engine_offline_render.cpp","sha256":"88faa34abef4b14fbb4118944b0029857b877465e9bacfe1a09c1c711bf5b22b"},{"path":"apps/engine_offline_render.h","sha256":"86329abd7ef5d21aa47933fa4dcd88c1af01d648f314ede78e5fbcc40f9f475a"},{"path":"apps/engine_patcher_assemble.cpp","sha256":"29f4828444eb11e7797115551a096dc9281faf58c0cd38b1bf713b7c55d4f913"},{"path":"apps/engine_patcher_commands.cpp","sha256":"233b687ff0e38c08746ac8182c1cc62620fee2dbea08bbb47e412478786cc767"},{"path":"apps/engine_plugin_state_capture.cpp","sha256":"bd046075ff4ead1c28f89926dab4155e9f28daddc73a74ddb241f08fbc75e734"},{"path":"apps/engine_plugin_state_capture.h","sha256":"97711cc2899d0e3d849af78d3d437667092570c38cc900375fcb269bfd551d60"},{"path":"apps/engine_produce_block.cpp","sha256":"1a23dc37dbe062c6d0b4c3cc4846e9544f2ad6969a053ef476b69400d404b6f5"},{"path":"apps/engine_produce_block.h","sha256":"da9666f206cbdc36df8f29eccd40ef650504b5ae2d85b6df1102ead41c4da282"},{"path":"apps/engine_producer_thread.cpp","sha256":"0b62556a81bdba5050afd73953f128e63cd2ceef4f35a80179ae370838034077"},{"path":"apps/engine_pure.cpp","sha256":"0e862cb3d1913d2fe7e10156e362402064da2c400e6c88f71ca20df812ca4162"},{"path":"apps/engine_readiness_level.h","sha256":"c5b725ae129e6951929554313f58e8897d40468919e4673fbe32de20133376e0"},{"path":"apps/engine_readiness_tests_main.cpp","sha256":"182d3ad792d59d59b4661287f2c6339a854710f21e1e3341e427b69f97245249"},{"path":"apps/engine_render_track.cpp","sha256":"af19d3855bb3ea25957bd06ec813130908860e900aa5123827b2923b09c378eb"},{"path":"apps/engine_render_track.h","sha256":"85bbc85374faa562fbf90cd558d5da627e81190fdb989ad4729d690062bc3b16"},{"path":"apps/engine_request_commands.cpp","sha256":"06a327c525822c538bd54e26a42688cacb72c4e75603aea0d147bcbc696128df"},{"path":"apps/engine_request_commands.h","sha256":"a3fc7ef31e8d9dc422f4b8dc320d34aa15efc0ee9addeaa42f0f6038f4e419c4"},{"path":"apps/engine_resolve_events.cpp","sha256":"77b4ff92dd58e786418bcae8fd0f92bf70dc886baea980a061b2556652c7811d"},{"path":"apps/engine_resolve_events.h","sha256":"42fd9d3f783c2259fad1e3728e4a2fcb271bc040efdeac307eb58132fa6903c5"},{"path":"apps/engine_restart_worker.cpp","sha256":"053a09d70f1d707b00805635b89ea08103c5b114ebc31e8b2c7a4c2dc07c3293"},{"path":"apps/engine_rt_helpers.cpp","sha256":"1625883fc0ce28060547839072369633df0b89e2a4a33a0eb4b4a8d7b03e7c17"},{"path":"apps/engine_rt_helpers.h","sha256":"d7aeff7793a31ac56bb23d852b001af1d74dcdd428d045ebe97edf52ce9a1643"},{"path":"apps/engine_run_patcher_node.cpp","sha256":"93f0acb74d031c2efbace39352d2283e7ab709b661b51216394969786e9384a3"},{"path":"apps/engine_run_patcher_node.h","sha256":"91e78f49c4cbd8d6b515c12bb1fe52faf5abb53fbff755d0f366d9ab7616a5e9"},{"path":"apps/engine_sampler_commands.cpp","sha256":"d19679a74c1069a1e2b752ce3526894c7b8df7b90992a4dd1afa6a068cdaa935"},{"path":"apps/engine_save_project.cpp","sha256":"bcefb06d30e16d7d04357cf3a29327db0ddb52a07eea3329144584565f26260e"},{"path":"apps/engine_shutdown.cpp","sha256":"03e5164af33aa3a85d9dd5da1012f5f7a5a7bbd3d103a644504ab1bf9976c502"},{"path":"apps/engine_song_store.cpp","sha256":"b9350b92a3c77a12b27a017428872c7ccd0cdfbfb0fa43065e3a0652fcc72854"},{"path":"apps/engine_startup.cpp","sha256":"07ec215ef0343386c6587093fea6119fa53bb72e9582b22ce74b393eed242bf4"},{"path":"apps/engine_track_commands.cpp","sha256":"2c4d8ce1a5bba50d6e3c1faaeacec971424a914970966a8d8d3c0395dbb84bc0"},{"path":"apps/engine_track_setup.cpp","sha256":"74d0f9ee21864116e386bbd87f750b7e359c020fd6d720081512c64e0289606e"},{"path":"apps/engine_trackprops_commands.cpp","sha256":"aaa382f1529d67122eeec1ccd93eca9574ff6473a9e0059d461d54a1b4b1d449"},{"path":"apps/engine_types.h","sha256":"e5023f9e4aa7e39174c692c1952db71fe1889d9055efba444ba5ce5fa9eab7e2"},{"path":"apps/engine_ui_publish.cpp","sha256":"c86d969174553f80af0517834a54d1c3a630f660ea486c81453c780bd8070d3c"},{"path":"apps/engine_ui_publish.h","sha256":"a51770bc094e4e27b961ce42ab174eef7cc504f9dadd1fe95140fa2f6a48ec86"},{"path":"apps/event_payloads.h","sha256":"1b5db6827706862320ea5651c05dc8c8102bf0cf848eae2a8d4114e1536f1504"},{"path":"apps/event_ring.cpp","sha256":"f8f2aebf11665c9aaac34ecc90e5fa307d89ed03c39250ee12f359f53087b548"},{"path":"apps/event_ring.h","sha256":"1bdf647ca0d1d1aa6234bba91917e736946130e5be6800442ebcfd9f28db4513"},{"path":"apps/host_controller.cpp","sha256":"a79f7df8136414f8f19bdc1dbcad4bbab0c26f6cfe1839962ebf0c9c5d2d90a4"},{"path":"apps/host_controller.h","sha256":"95be8143ebd7186e1f82a63315c910a312e1ea4b39cc3a6d62b69fe5526c0d6c"},{"path":"apps/ipc_io.cpp","sha256":"5576603cd2355d97863e473ad89191e11ff700ecc52d4b54d2bc0eb593339402"},{"path":"apps/ipc_protocol.h","sha256":"bff82aa0f4887301f7666662293a50a04822429d01d600cb64fffb0e58573772"},{"path":"apps/juce_host_process_main.cpp","sha256":"1199fe3897b46a47ae52790250ae2b0ab6c83f6ba7547c815e55f84e4a90f46e"},{"path":"apps/patcher_assemble.h","sha256":"fc09fe967ae4c1d78bc10c7bc9677a9dd2bfe478cb09fad5dd9fc23f35b055bc"},{"path":"apps/project_file.cpp","sha256":"3f6b616c3ee4d82f46873f8b207a388d610c91aa8665b76e8fe6e2ccbdd11fda"},{"path":"apps/shared_memory.h","sha256":"765bde1d7a98db962c07fff7f91ccdcab79d50dc57b40de4311787e9b876dd69"},{"path":"CMakeLists.txt","sha256":"c3c1447eb12dde676be48d39d6e1c68be22de138417768e71c429e09983f9a58"},{"path":"SHM_LAYOUT.md","sha256":"8cb75c99e277fb185f966890a678ac29b749ffc885ba854e05985de630ef4b61"},{"path":"ui/daw-bridge/src/layout.rs","sha256":"6df8bd15c297df240b61891f87e88f155879e18e93bcb89d25ad4678b64ebe36"}]''')
EXPECTED_POPULATIONS = json.loads(r'''{"host_ready_true_sites":[{"path":"apps/engine_track_setup.cpp","line":64,"symbol":"setupTrackRuntime","classification":"prepublication object; publishes before the chain is constructed"},{"path":"apps/engine_track_setup.cpp","line":429,"symbol":"restartTrackHost","classification":"published runtime under controllerMutex; no bypass staging"},{"path":"apps/engine_restart_worker.cpp","line":143,"symbol":"runRestartWorker","classification":"published runtime; store occurs after controller unlock and before bypass staging"}],"track_snapshot_publications":[{"path":"apps/daw_engine_main.cpp","line":414,"kind":"plain_prepublication"},{"path":"apps/daw_engine_main.cpp","line":1100,"kind":"plain_prepublication"},{"path":"apps/engine_arrangetime_commands.cpp","line":380,"kind":"atomic"},{"path":"apps/engine_automation_commands.cpp","line":56,"kind":"atomic"},{"path":"apps/engine_automation_commands.cpp","line":237,"kind":"atomic"},{"path":"apps/engine_automation_commands.cpp","line":333,"kind":"atomic"},{"path":"apps/engine_chain_commands.cpp","line":168,"kind":"atomic"},{"path":"apps/engine_consumer.cpp","line":592,"kind":"atomic"},{"path":"apps/engine_load_project.cpp","line":392,"kind":"atomic"},{"path":"apps/engine_load_project.cpp","line":504,"kind":"atomic"},{"path":"apps/engine_load_track.cpp","line":139,"kind":"atomic"},{"path":"apps/engine_modlink_commands.cpp","line":100,"kind":"atomic"},{"path":"apps/engine_modlink_commands.cpp","line":231,"kind":"atomic"},{"path":"apps/engine_modlink_commands.cpp","line":283,"kind":"atomic"},{"path":"apps/engine_patcher_commands.cpp","line":192,"kind":"atomic"},{"path":"apps/engine_patcher_commands.cpp","line":421,"kind":"atomic"},{"path":"apps/engine_song_store.cpp","line":117,"kind":"atomic"},{"path":"apps/engine_track_commands.cpp","line":95,"kind":"atomic"},{"path":"apps/engine_track_commands.cpp","line":184,"kind":"atomic"},{"path":"apps/engine_track_commands.cpp","line":238,"kind":"atomic"},{"path":"apps/engine_track_setup.cpp","line":100,"kind":"plain_prepublication"},{"path":"apps/engine_track_setup.cpp","line":281,"kind":"atomic"},{"path":"apps/engine_trackprops_commands.cpp","line":70,"kind":"atomic_unlocked_build"},{"path":"apps/engine_trackprops_commands.cpp","line":99,"kind":"atomic_unlocked_build"}],"host_plan_mutation_roots":[{"path":"apps/engine_track_setup.cpp","line":69,"symbol":"setupTrackRuntime","classification":"host_plan"},{"path":"apps/engine_chain_commands.cpp","line":100,"symbol":"handleAddDevice/addDevice","classification":"execution_plan_topology"},{"path":"apps/engine_chain_commands.cpp","line":108,"symbol":"handleAddDevice/removeDeviceById","classification":"execution_plan_topology"},{"path":"apps/engine_chain_commands.cpp","line":115,"symbol":"handleAddDevice/moveDeviceById","classification":"execution_plan_topology"},{"path":"apps/engine_chain_commands.cpp","line":125,"symbol":"handleAddDevice/setDeviceBypass","classification":"execution_plan_vst_or_patcher_audio"},{"path":"apps/engine_chain_commands.cpp","line":130,"symbol":"handleAddDevice/setDevicePatcherNodeId","classification":"execution_plan_patcher_audio_node"},{"path":"apps/engine_chain_commands.cpp","line":135,"symbol":"handleAddDevice/setDeviceHostSlotIndex","classification":"host_plan_when_vst"},{"path":"apps/daw_engine_main.cpp","line":1209,"symbol":"updateTrackChainForInstrument","classification":"host_plan_missing_snapshot_publication"},{"path":"apps/engine_load_track.cpp","line":86,"symbol":"loadTrackFromDocument","classification":"host_plan"},{"path":"apps/engine_load_project.cpp","line":498,"symbol":"applyDocument/master","classification":"host_plan"},{"path":"apps/engine_rt_helpers.cpp","line":76,"symbol":"resetTrackContent","classification":"host_plan_missing_snapshot_on_leftover_clear"},{"path":"apps/engine_track_commands.cpp","line":344,"symbol":"handleRemoveTrack","classification":"host_plan_missing_snapshot_publication"},{"path":"apps/engine_patcher_assemble.cpp","line":90,"symbol":"reassemblePatcherFromDevices","classification":"execution_plan_patcher_node_missing_publication"},{"path":"apps/engine_load_patcher_pool.cpp","line":103,"symbol":"loadPatcherPool/runtime repoint","classification":"execution_plan_patcher_node_missing_publication"},{"path":"apps/engine_consumer.cpp","line":573,"symbol":"reconcileChildTracks/restore","classification":"hostless_aux_explicit_exclusion"},{"path":"apps/daw_engine_main.cpp","line":1099,"symbol":"makeAuxChild","classification":"hostless_aux_prepublication_explicit_exclusion"}],"classified_non_host_chain_mutations":[{"path":"apps/engine_sampler_commands.cpp","classification":"sampler document internals; samplerSnapshot authority"},{"path":"apps/engine_bulk_edit.cpp","classification":"sampler document internals"},{"path":"apps/engine_patcher_commands.cpp","classification":"patcher graph/config internals; resulting patcherNodeId repoint is included separately"},{"path":"apps/engine_track_commands.cpp","classification":"Euclidean patcher configuration internals"}],"chain_mutation_scan":[{"path":"apps/daw_engine_main.cpp","line":1099,"classification":"hostless_aux_prepublication"},{"path":"apps/daw_engine_main.cpp","line":1209,"classification":"live_execution_plan_alias"},{"path":"apps/daw_engine_main.cpp","line":1217,"classification":"live_execution_plan_mutation"},{"path":"apps/engine_bulk_edit.cpp","line":91,"classification":"live_sampler_internal"},{"path":"apps/engine_bulk_edit.cpp","line":240,"classification":"live_sampler_internal"},{"path":"apps/engine_chain_commands.cpp","line":100,"classification":"live_execution_plan_mutation"},{"path":"apps/engine_chain_commands.cpp","line":108,"classification":"live_execution_plan_mutation"},{"path":"apps/engine_chain_commands.cpp","line":115,"classification":"live_execution_plan_mutation"},{"path":"apps/engine_chain_commands.cpp","line":125,"classification":"live_execution_plan_mutation"},{"path":"apps/engine_chain_commands.cpp","line":130,"classification":"live_execution_plan_mutation"},{"path":"apps/engine_chain_commands.cpp","line":135,"classification":"live_execution_plan_mutation"},{"path":"apps/engine_chain_host.cpp","line":159,"classification":"read_only_false_positive"},{"path":"apps/engine_consumer.cpp","line":573,"classification":"hostless_aux_restore"},{"path":"apps/engine_load_patcher_pool.cpp","line":76,"classification":"comment_false_positive"},{"path":"apps/engine_load_patcher_pool.cpp","line":88,"classification":"document_transform"},{"path":"apps/engine_load_patcher_pool.cpp","line":101,"classification":"live_execution_plan_mutation"},{"path":"apps/engine_load_project.cpp","line":105,"classification":"document_transform"},{"path":"apps/engine_load_project.cpp","line":498,"classification":"live_execution_plan_mutation"},{"path":"apps/engine_load_project.cpp","line":617,"classification":"live_execution_plan_helper_call"},{"path":"apps/engine_load_track.cpp","line":86,"classification":"live_execution_plan_mutation"},{"path":"apps/engine_patcher_assemble.cpp","line":88,"classification":"live_execution_plan_mutation"},{"path":"apps/engine_patcher_commands.cpp","line":135,"classification":"live_patcher_internal"},{"path":"apps/engine_patcher_commands.cpp","line":407,"classification":"live_patcher_internal"},{"path":"apps/engine_rt_helpers.cpp","line":75,"classification":"execution_plan_helper_declaration"},{"path":"apps/engine_rt_helpers.cpp","line":76,"classification":"live_execution_plan_mutation"},{"path":"apps/engine_sampler_commands.cpp","line":206,"classification":"live_sampler_internal"},{"path":"apps/engine_sampler_commands.cpp","line":724,"classification":"live_sampler_internal"},{"path":"apps/engine_sampler_commands.cpp","line":924,"classification":"live_sampler_internal"},{"path":"apps/engine_sampler_commands.cpp","line":1032,"classification":"live_sampler_internal"},{"path":"apps/engine_sampler_commands.cpp","line":1124,"classification":"live_sampler_internal"},{"path":"apps/engine_sampler_commands.cpp","line":1199,"classification":"live_sampler_internal"},{"path":"apps/engine_sampler_commands.cpp","line":1307,"classification":"live_sampler_internal"},{"path":"apps/engine_sampler_commands.cpp","line":1429,"classification":"live_sampler_internal"},{"path":"apps/engine_save_project.cpp","line":249,"classification":"save_copy_transform"},{"path":"apps/engine_save_project.cpp","line":380,"classification":"save_copy_transform"},{"path":"apps/engine_track_commands.cpp","line":174,"classification":"live_euclidean_internal"},{"path":"apps/engine_track_commands.cpp","line":233,"classification":"live_execution_plan_helper_call"},{"path":"apps/engine_track_commands.cpp","line":344,"classification":"live_execution_plan_mutation"},{"path":"apps/engine_track_setup.cpp","line":69,"classification":"live_execution_plan_mutation"},{"path":"apps/engine_track_setup.cpp","line":75,"classification":"live_execution_plan_mutation"},{"path":"apps/engine_track_setup.cpp","line":79,"classification":"live_execution_plan_mutation"},{"path":"apps/engine_track_setup.cpp","line":268,"classification":"live_execution_plan_helper_call"},{"path":"apps/project_file.cpp","line":1315,"classification":"document_parser_transform"},{"path":"apps/project_file.cpp","line":1437,"classification":"document_parser_transform"}],"execution_authority_lexical_scan":[{"path":"apps/engine_ui_publish.cpp","line":264,"classification":"execution_or_control_consumer"},{"path":"apps/engine_render_track.cpp","line":41,"classification":"execution_or_control_consumer"},{"path":"apps/engine_render_track.cpp","line":215,"classification":"execution_or_control_consumer"},{"path":"apps/engine_render_track.cpp","line":266,"classification":"execution_or_control_consumer"},{"path":"apps/engine_render_track.cpp","line":306,"classification":"execution_or_control_consumer"},{"path":"apps/engine_render_track.cpp","line":335,"classification":"execution_or_control_consumer"},{"path":"apps/engine_render_track.cpp","line":387,"classification":"execution_or_control_consumer"},{"path":"apps/engine_render_track.cpp","line":395,"classification":"execution_or_control_consumer"},{"path":"apps/engine_render_track.cpp","line":703,"classification":"execution_or_control_consumer"},{"path":"apps/engine_render_track.cpp","line":706,"classification":"execution_or_control_consumer"},{"path":"apps/engine_request_commands.h","line":45,"classification":"interface_dependency"},{"path":"apps/engine_chain_host.h","line":7,"classification":"comment_false_positive"},{"path":"apps/engine_chain_host.h","line":34,"classification":"interface_dependency"},{"path":"apps/engine_chain_host.h","line":39,"classification":"interface_dependency"},{"path":"apps/engine_rt_helpers.h","line":414,"classification":"interface_dependency"},{"path":"apps/engine_rt_helpers.h","line":424,"classification":"interface_dependency"},{"path":"apps/engine_consumer.cpp","line":1072,"classification":"execution_or_control_consumer"},{"path":"apps/engine_consumer.cpp","line":1115,"classification":"execution_or_control_consumer"},{"path":"apps/engine_consumer.cpp","line":1119,"classification":"execution_or_control_consumer"},{"path":"apps/engine_consumer.cpp","line":1127,"classification":"execution_or_control_consumer"},{"path":"apps/engine_consumer.cpp","line":1181,"classification":"execution_or_control_consumer"},{"path":"apps/engine_device_commands.cpp","line":47,"classification":"execution_or_control_consumer"},{"path":"apps/engine_device_commands.cpp","line":70,"classification":"execution_or_control_consumer"},{"path":"apps/engine_device_commands.cpp","line":133,"classification":"execution_or_control_consumer"},{"path":"apps/engine_device_commands.cpp","line":157,"classification":"execution_or_control_consumer"},{"path":"apps/engine_chain_host.cpp","line":13,"classification":"execution_or_control_consumer"},{"path":"apps/engine_chain_host.cpp","line":53,"classification":"execution_or_control_consumer"},{"path":"apps/engine_chain_host.cpp","line":144,"classification":"execution_or_control_consumer"},{"path":"apps/engine_chain_host.cpp","line":179,"classification":"execution_or_control_consumer"},{"path":"apps/engine_types.h","line":140,"classification":"withdrawn_authority_field"},{"path":"apps/daw_engine_main.cpp","line":322,"classification":"snapshot_authority_copy"},{"path":"apps/daw_engine_main.cpp","line":1037,"classification":"execution_or_control_consumer"},{"path":"apps/daw_engine_main.cpp","line":1080,"classification":"execution_or_control_consumer"},{"path":"apps/daw_engine_main.cpp","line":1259,"classification":"execution_or_control_consumer"},{"path":"apps/daw_engine_main.cpp","line":1932,"classification":"execution_or_control_consumer"},{"path":"apps/daw_engine_main.cpp","line":1979,"classification":"execution_or_control_consumer"},{"path":"apps/daw_engine_main.cpp","line":2037,"classification":"execution_or_control_consumer"},{"path":"apps/engine_produce_block.cpp","line":80,"classification":"execution_or_control_consumer"},{"path":"apps/engine_produce_block.cpp","line":163,"classification":"execution_or_control_consumer"},{"path":"apps/engine_produce_block.cpp","line":653,"classification":"execution_or_control_consumer"},{"path":"apps/engine_produce_block.cpp","line":655,"classification":"execution_or_control_consumer"},{"path":"apps/engine_produce_block.cpp","line":660,"classification":"execution_or_control_consumer"},{"path":"apps/engine_produce_block.cpp","line":664,"classification":"execution_or_control_consumer"},{"path":"apps/engine_producer_thread.cpp","line":33,"classification":"execution_or_control_consumer"},{"path":"apps/engine_producer_thread.cpp","line":131,"classification":"execution_or_control_consumer"},{"path":"apps/engine_render_track.h","line":91,"classification":"interface_dependency"},{"path":"apps/engine_rt_helpers.cpp","line":395,"classification":"execution_or_control_consumer"},{"path":"apps/engine_rt_helpers.cpp","line":397,"classification":"execution_or_control_consumer"},{"path":"apps/engine_rt_helpers.cpp","line":398,"classification":"execution_or_control_consumer"},{"path":"apps/engine_rt_helpers.cpp","line":399,"classification":"execution_or_control_consumer"},{"path":"apps/engine_rt_helpers.cpp","line":412,"classification":"execution_or_control_consumer"},{"path":"apps/engine_rt_helpers.cpp","line":417,"classification":"execution_or_control_consumer"},{"path":"apps/engine_produce_block.h","line":87,"classification":"interface_dependency"},{"path":"apps/engine_device_commands.h","line":34,"classification":"interface_dependency"},{"path":"apps/engine_request_commands.cpp","line":52,"classification":"execution_or_control_consumer"},{"path":"apps/engine_request_commands.cpp","line":75,"classification":"execution_or_control_consumer"},{"path":"apps/engine_producer_thread.h","line":74,"classification":"interface_dependency"}],"execution_authority_consumers":[{"path":"apps/daw_engine_main.cpp","line":319,"symbol":"buildTrackSnapshot","authority":"chain snapshot copy","disposition":"migrate_to_execution_snapshot"},{"path":"apps/device_chain.cpp","line":211,"symbol":"resolveDeviceSlot","authority":"VST durable identity resolution","disposition":"plan_compiler_input"},{"path":"apps/patcher_assemble.h","line":63,"symbol":"assemblePatcherPool","authority":"global patcher graph and ownership","disposition":"plan_compiler"},{"path":"apps/engine_patcher_assemble.cpp","line":20,"symbol":"reassemblePatcherFromDevices","authority":"pooled node identity and graph publication","disposition":"migrate_to_execution_snapshot"},{"path":"apps/engine_load_patcher_pool.cpp","line":15,"symbol":"loadPatcherGraphsFromDocument","authority":"load-time patcher compiler","disposition":"migrate_to_execution_snapshot"},{"path":"apps/engine_load_project.cpp","line":94,"symbol":"applyDocument","authority":"patcher output sentinel resolution","disposition":"migrate_to_execution_snapshot"},{"path":"apps/engine_render_track.cpp","line":215,"symbol":"renderTrack","authority":"patcher ownership/filter/order","disposition":"migrate_to_execution_snapshot"},{"path":"apps/engine_render_track.cpp","line":385,"symbol":"renderTrack","authority":"default compact VST target","disposition":"migrate_to_execution_snapshot"},{"path":"apps/engine_produce_block.cpp","line":653,"symbol":"produceBlock/processTrack","authority":"VST segments and PatcherAudio placement","disposition":"migrate_to_execution_snapshot"},{"path":"apps/engine_rt_helpers.cpp","line":395,"symbol":"applyBlockRateModulation","authority":"chain order and compact VST target","disposition":"migrate_to_execution_snapshot"},{"path":"apps/engine_run_patcher_node.cpp","line":15,"symbol":"runPatcherNode","authority":"plan-derived patcher ownership","disposition":"migrate_to_execution_snapshot"},{"path":"apps/engine_emit_notes.cpp","line":19,"symbol":"emitNotesInRange","authority":"sampler identity and parameter target","disposition":"migrate_to_execution_snapshot"},{"path":"apps/engine_resolve_events.cpp","line":160,"symbol":"resolveMusicalLogicAndSort","authority":"sampler execution alias","disposition":"migrate_to_execution_snapshot"},{"path":"apps/engine_rt_helpers.cpp","line":236,"symbol":"cutActiveNotes","authority":"sampler execution alias","disposition":"migrate_to_execution_snapshot"},{"path":"apps/engine_sampler_commands.cpp","line":1550,"symbol":"refreshSamplerForTrack","authority":"sampler identity publication","disposition":"migrate_to_execution_snapshot"},{"path":"apps/engine_ui_publish.cpp","line":264,"symbol":"writeMirrorParams","authority":"mirror default compact VST target","disposition":"migrate_to_execution_snapshot"},{"path":"apps/automation_clip.h","line":169,"symbol":"AutomationClip::targetPluginIndex","authority":"persisted compact target","disposition":"replace_with_stable_device_id"},{"path":"apps/engine_types.h","line":168,"symbol":"ParamMirrorEntry","authority":"long-lived compact mirror target","disposition":"replace_with_stable_device_id"},{"path":"apps/engine_automation_commands.cpp","line":13,"symbol":"handleSetAutomationTarget","authority":"automation target authoring","disposition":"replace_with_stable_device_id"},{"path":"apps/engine_automation_commands.cpp","line":144,"symbol":"handleWriteAutomationPoint","authority":"automation target update","disposition":"replace_with_stable_device_id"},{"path":"apps/engine_consumer.cpp","line":143,"symbol":"writeUiAutomationLanesTo","authority":"compact automation target publication","disposition":"replace_with_stable_device_id"},{"path":"apps/project_file.cpp","line":789,"symbol":"serializeProject","authority":"compact automation target persistence","disposition":"replace_with_stable_device_id"},{"path":"apps/daw_engine_main.cpp","line":1037,"symbol":"resolveDevicePluginPath","authority":"host path resolution","disposition":"plan_compiler_input"},{"path":"apps/daw_engine_main.cpp","line":1058,"symbol":"applyHostBypassStates","authority":"VST compaction and bypass staging","disposition":"migrate_to_execution_snapshot"},{"path":"apps/engine_chain_host.cpp","line":142,"symbol":"rebuildHostForChain","authority":"host plan compilation and installation","disposition":"migrate_to_execution_snapshot"},{"path":"apps/engine_chain_host.cpp","line":43,"symbol":"emitChainSnapshot","authority":"bus compact index and latency","disposition":"migrate_to_execution_snapshot"},{"path":"apps/engine_restart_worker.cpp","line":102,"symbol":"runRestartWorker","authority":"relaunch from mutable HostConfig","disposition":"migrate_to_execution_snapshot"},{"path":"apps/engine_track_setup.cpp","line":20,"symbol":"setupTrackRuntime","authority":"pre-chain host launch","disposition":"migrate_to_execution_snapshot"},{"path":"apps/engine_track_setup.cpp","line":149,"symbol":"reconcileChildTracks","authority":"aux compact host index","disposition":"migrate_to_execution_snapshot"},{"path":"apps/engine_track_setup.cpp","line":447,"symbol":"reconcileMasterHost","authority":"master VST presence alias","disposition":"migrate_to_execution_snapshot"},{"path":"apps/engine_master_render.cpp","line":19,"symbol":"runMasterRenderThread","authority":"masterFxActive execution alias","disposition":"migrate_to_execution_snapshot"},{"path":"apps/engine_audio_callback.h","line":694,"symbol":"EngineAudioCallback::process","authority":"masterFxActive output gate","disposition":"migrate_to_execution_snapshot"},{"path":"apps/engine_audio_start.cpp","line":84,"symbol":"startAudio","authority":"master FX wiring","disposition":"migrate_to_execution_snapshot"},{"path":"apps/engine_shutdown.cpp","line":136,"symbol":"shutdownEngine","authority":"master FX diagnostics","disposition":"migrate_to_execution_snapshot"},{"path":"apps/engine_consumer.cpp","line":512,"symbol":"runConsumerThread","authority":"aux mask execution alias","disposition":"migrate_to_execution_snapshot"},{"path":"apps/host_controller.cpp","line":118,"symbol":"HostController::launch","authority":"host compact plugin vector","disposition":"migrate_to_execution_snapshot"},{"path":"apps/host_controller.cpp","line":376,"symbol":"HostController host requests","authority":"unversioned compact indexes","disposition":"migrate_to_execution_snapshot"},{"path":"apps/juce_host_process_main.cpp","line":242,"symbol":"reconcileChain","authority":"host compact plugin vector","disposition":"migrate_to_execution_snapshot"},{"path":"apps/juce_host_process_main.cpp","line":607,"symbol":"handleProcessBlock","authority":"segment/parameter/meter compact indexes","disposition":"migrate_to_execution_snapshot"},{"path":"apps/juce_host_process_main.cpp","line":1105,"symbol":"handleOpenEditor/handleSetBypass","authority":"control compact indexes","disposition":"migrate_to_execution_snapshot"},{"path":"apps/engine_device_commands.cpp","line":42,"symbol":"handleOpenPluginEditor","authority":"live-chain compact index","disposition":"migrate_to_execution_snapshot"},{"path":"apps/engine_device_commands.cpp","line":126,"symbol":"handleSetDeviceParam","authority":"live-chain compact index and mirror target","disposition":"migrate_to_execution_snapshot"},{"path":"apps/engine_request_commands.cpp","line":46,"symbol":"handleRequestDeviceParams","authority":"live-chain compact index","disposition":"migrate_to_execution_snapshot"},{"path":"apps/engine_plugin_state_capture.cpp","line":41,"symbol":"hostedDevices","authority":"unfiltered VST compaction","disposition":"migrate_to_execution_snapshot"},{"path":"apps/engine_plugin_state_capture.cpp","line":62,"symbol":"capturePluginState/restorePluginSnapshot","authority":"state compact index","disposition":"migrate_to_execution_snapshot"},{"path":"apps/engine_save_project.cpp","line":243,"symbol":"captureDocument","authority":"derived plugin and patcher identity","disposition":"migrate_to_execution_snapshot"},{"path":"apps/engine_save_project.cpp","line":421,"symbol":"saveProjectToPath","authority":"state/parameter compact index","disposition":"migrate_to_execution_snapshot"},{"path":"apps/engine_load_track.cpp","line":13,"symbol":"loadTrackFromDocument","authority":"split chain/snapshot/host publication","disposition":"migrate_to_execution_snapshot"},{"path":"apps/engine_load_project.cpp","line":324,"symbol":"applyDocument","authority":"graph before chain publication","disposition":"migrate_to_execution_snapshot"},{"path":"apps/engine_load_project.cpp","line":460,"symbol":"applyDocument master","authority":"split master publication","disposition":"migrate_to_execution_snapshot"},{"path":"apps/engine_load_project.cpp","line":686,"symbol":"restorePluginStateFromDisk","authority":"reconstructed compact index","disposition":"migrate_to_execution_snapshot"},{"path":"apps/engine_consumer.cpp","line":312,"symbol":"writeUiPatcherTo","authority":"pooled owner identity","disposition":"migrate_to_execution_snapshot"},{"path":"apps/engine_consumer.cpp","line":1174,"symbol":"runConsumerThread meter path","authority":"meter compact index mapping","disposition":"migrate_to_execution_snapshot"},{"path":"apps/shared_memory.h","line":358,"symbol":"ShmHeader::hostDeviceMeters","authority":"compact meter carrier","disposition":"wire_carrier"},{"path":"apps/event_payloads.h","line":31,"symbol":"ParamPayload::targetPluginIndex","authority":"compact target carrier","disposition":"wire_carrier"}],"authored_document_exemptions":[{"path":"apps/device_chain.cpp","line":27,"symbol":"authored chain helpers","role":"document mutation and validation only"},{"path":"apps/engine_chain_commands.cpp","line":50,"symbol":"handleAddDevice","role":"document edit before transaction commit"},{"path":"apps/engine_patcher_commands.cpp","line":92,"symbol":"handleAddPatcherNode","role":"device-owned graph editing"},{"path":"apps/engine_modlink_commands.cpp","line":16,"symbol":"handleAddModLink","role":"stable device-id validation"},{"path":"apps/engine_sampler_commands.cpp","line":151,"symbol":"sampler edit handlers","role":"authored sampler editing"},{"path":"apps/engine_request_commands.cpp","line":176,"symbol":"handleRequestWaveform","role":"document/UI lookup"},{"path":"apps/project_file.cpp","line":540,"symbol":"serializeProject","role":"authored document persistence"},{"path":"apps/engine_save_project.cpp","line":44,"symbol":"captureAuthoredTrackFields","role":"authored chain capture only"},{"path":"apps/engine_pure.cpp","line":13,"symbol":"documentHasPerDeviceGraphs","role":"pure document inspection"},{"path":"apps/daw_lint_main.cpp","line":301,"symbol":"checkChains","role":"document validation"},{"path":"apps/engine_consumer.cpp","line":1063,"symbol":"rack UI publication","role":"labels and authored kinds only"},{"path":"apps/engine_chain_host.cpp","line":23,"symbol":"emitChainSnapshot presentation","role":"authored UI presentation only"},{"path":"apps/engine_startup.cpp","line":187,"symbol":"default plugin path","role":"plan compiler input"}],"process_block_senders":[{"path":"apps/engine_produce_block.cpp","line":1075,"symbol":"produceBlock/processTrack","branch":"track_debug"},{"path":"apps/engine_produce_block.cpp","line":1089,"symbol":"produceBlock/processTrack","branch":"track_normal"},{"path":"apps/engine_master_render.cpp","line":75,"symbol":"runMasterRenderThread","branch":"master"}],"process_block_receiver":[{"path":"apps/juce_host_process_main.cpp","line":1200,"symbol":"runControlLoop","role":"dispatch_edge"},{"path":"apps/juce_host_process_main.cpp","line":607,"symbol":"handleProcessBlock","role":"receiver"}],"replay_protocol_sites":[{"path":"apps/engine_ui_publish.cpp","line":292,"symbol":"writeMirrorParams","role":"parameter_sender"},{"path":"apps/engine_ui_publish.cpp","line":301,"symbol":"writeMirrorParams","role":"gate_sender"},{"path":"apps/engine_produce_block.cpp","line":513,"symbol":"produceBlock/processTrack","role":"prime_publication"},{"path":"apps/juce_host_process_main.cpp","line":777,"symbol":"handleProcessBlock","role":"gate_consumer"},{"path":"apps/juce_host_process_main.cpp","line":1097,"symbol":"handleProcessBlock","role":"ack_high_water_read"},{"path":"apps/juce_host_process_main.cpp","line":1099,"symbol":"handleProcessBlock","role":"ack_publisher"},{"path":"apps/engine_producer_thread.cpp","line":214,"symbol":"runProducerThread","role":"ack_reader"},{"path":"apps/shared_memory.h","line":618,"symbol":"BlockMailbox","role":"ack_word"}],"offline_coordinator_sites":[{"path":"apps/daw_engine_main.cpp","line":2115,"symbol":"main","role":"coordinator_invocation"},{"path":"apps/engine_offline_render.cpp","line":123,"symbol":"runOfflinePump","role":"mapping_preflight"},{"path":"apps/engine_offline_render.cpp","line":135,"symbol":"runOfflinePump","role":"timeline_reset"},{"path":"apps/engine_offline_render.cpp","line":137,"symbol":"runOfflinePump","role":"producer_arm"},{"path":"apps/engine_offline_render.cpp","line":149,"symbol":"runOfflinePump","role":"production_preflight"},{"path":"apps/engine_offline_render.cpp","line":164,"symbol":"runOfflinePump","role":"counted_block_wait"},{"path":"apps/engine_offline_render.cpp","line":179,"symbol":"runOfflinePump","role":"counted_mix"},{"path":"apps/engine_offline_render.cpp","line":186,"symbol":"runOfflinePump","role":"count_increment"},{"path":"apps/engine_offline_render.cpp","line":198,"symbol":"runOfflinePump","role":"output_writer"},{"path":"apps/engine_producer_thread.cpp","line":142,"symbol":"runProducerThread","role":"arm_observer"}],"mapping_and_output_gates":[{"path":"apps/engine_audio_callback.h","line":350,"symbol":"process startup cushion","state":"current_gate"},{"path":"apps/engine_audio_callback.h","line":376,"symbol":"process track gate","state":"current_gate"},{"path":"apps/engine_audio_callback.h","line":469,"symbol":"process channel gate","state":"current_gate"},{"path":"apps/engine_audio_callback.h","line":1040,"symbol":"awaitNextBlock","state":"current_gate"},{"path":"apps/engine_audio_callback.h","line":852,"symbol":"awaitAnyReadyTrack","state":"missing_gate"},{"path":"apps/engine_audio_callback.h","line":941,"symbol":"awaitAllReadyTracks","state":"missing_gate"},{"path":"apps/engine_audio_callback.h","line":694,"symbol":"master FX callback","state":"boolean_only"},{"path":"apps/engine_master_render.cpp","line":75,"symbol":"master sender","state":"boolean_only"},{"path":"apps/engine_consumer.cpp","line":656,"symbol":"TrackInfo publication","state":"generation_only"},{"path":"apps/engine_producer_thread.cpp","line":270,"symbol":"active publication","state":"pre_mirror_complete"}],"capacity_contract_sites":[{"path":"apps/host_controller.h","line":33,"symbol":"HostConfig::ringStdCapacity","role":"capacity_source"},{"path":"apps/host_controller.cpp","line":228,"symbol":"HostController::launch","role":"capacity_handshake"},{"path":"apps/juce_host_process_main.cpp","line":524,"symbol":"handleHello","role":"ring_header_capacity"},{"path":"apps/event_ring.cpp","line":72,"symbol":"ringWrite","role":"one_slot_empty_writer"},{"path":"apps/engine_types.h","line":383,"symbol":"TrackRuntime::paramMirror","role":"unbounded_mirror"}]}''')
EXPECTED_RECORDS = json.loads(r'''[{"id":"G-ITEM18","kind":"gate","owner":"backend","status":"READY_FOR_REVIEW","dependencies":["DEP-PREDECESSOR","DEP-ITEM15","DEP-FROZEN-BASE","P-READINESS-PUBLISHERS","P-SNAPSHOT-PUBLISHERS","P-HOST-PLAN-MUTATIONS","P-EXECUTION-AUTHORITY-CONSUMERS","P-DISPATCH-PROTOCOL-SURFACES","P-OFFLINE-OUTPUT-SURFACES","R-BYPASS-STAGED","R-HOST-PLAN-AUTHORITY","R-STABLE-DEVICE-TARGETS","R-DISPATCH-TICKET","R-MIRROR-EPOCH","R-CORRELATED-REPLAY-ACK","R-ATOMIC-PRIMER-CAPACITY","R-PASS4-REPLACEMENT","R-MASTER-CORRELATION","R-OFFLINE-PRIMER","R-PROTOCOL-VERSION","R-G4-WITNESS","D-PRODUCTION-FIXTURE","D-PRODUCTION-RECEIVER","CTRL-PACKET","CTRL-MUTATIONS"],"source_span":["predecessor:docs/architecture/tasks/AE-P1.2-shm-contract.md:965-984","predecessor:docs/architecture/tasks/AE-P1.2-shm-contract.md:1876-1890","predecessor:docs/architecture/tasks/AE-P1.2-shm-contract.md:2559-2569"],"control":"CTRL-PACKET","statement":"Item 18 is acceptance-decidable: one session ExecutionSnapshot is the sole execution authority; every sender, receiver, mapping, master, and offline surface is inventoried; and PASS 4 is a production-bound correlated primer/ack/re-arm oracle with deterministic capacity failure."},{"id":"DEP-PREDECESSOR","kind":"dependency","owner":"backend","status":"PINNED","dependencies":[],"source_span":"predecessor:docs/architecture/tasks/AE-P1.2-manifest.json:1-40","control":"CTRL-PACKET","statement":"The original AE-P1.2 packet and its withdrawn PASS 4 are immutable inputs."},{"id":"DEP-ITEM15","kind":"dependency","owner":"backend","status":"DUAL_PASS_PINNED","dependencies":[],"source_span":"item15:docs/architecture/tasks/AE-P1.2-g2b-item15-manifest.json:1-40","control":"CTRL-PACKET","statement":"The dual-PASS item-15 lock, plan-capture, bypass-failure, poisoned-transport, and stale-waiter rulings are mandatory implementation inputs."},{"id":"DEP-FROZEN-BASE","kind":"dependency","owner":"backend","status":"PINNED","dependencies":[],"source_span":"frozen:apps/engine_readiness_level.h:1-105","control":"CTRL-PACKET","statement":"The census and acceptance design describe exactly frozen product 92dfdfe2 and its governed blobs."},{"id":"P-READINESS-PUBLISHERS","kind":"population","owner":"backend","status":"EXACT_3","dependencies":["DEP-FROZEN-BASE"],"source_span":"manifest:/populations/host_ready_true_sites","control":"CTRL-PACKET","statement":"Exactly three production hostReady true stores exist; none proves mapping plus successful bypass staging for a published runtime."},{"id":"P-SNAPSHOT-PUBLISHERS","kind":"population","owner":"backend","status":"EXACT_24","dependencies":["DEP-FROZEN-BASE"],"source_span":"manifest:/populations/track_snapshot_publications","control":"CTRL-PACKET","statement":"Exactly twenty-four production TrackStateSnapshot publications exist: three prepublication assignments and twenty-one atomic stores; this packet does not silently treat them as a coherent host-plan authority."},{"id":"P-HOST-PLAN-MUTATIONS","kind":"population","owner":"backend","status":"EXACT_CLASSIFIED_ROOTS","dependencies":["DEP-FROZEN-BASE","P-SNAPSHOT-PUBLISHERS"],"source_span":["manifest:/populations/host_plan_mutation_roots","manifest:/populations/classified_non_host_chain_mutations","manifest:/populations/chain_mutation_scan"],"control":"CTRL-PACKET","statement":"The authored execution inputs have sixteen semantic mutation roots and four internal-state families cross-checked against the mechanically reproduced 44-line frozen-source mutation scan. Every mutation commits through the one session ExecutionSnapshot transaction or remains explicitly document-only."},{"id":"R-BYPASS-STAGED","kind":"ruling","owner":"backend","status":"DECIDED","dependencies":["DEP-ITEM15"],"source_span":["frozen:apps/host_controller.cpp:624-654","frozen:apps/ipc_io.cpp:89-155"],"control":"CTRL-MUTATIONS","statement":"The lower stage is MappedAndBypassStaged, not a claim that the plugin acknowledged application. It is reached only after every ordered SetBypass frame was transmitted completely; stream order places those frames before ProcessBlock. Any false result withdraws readiness and disconnects the possibly partial-frame-poisoned stream under controllerMutex."},{"id":"R-HOST-PLAN-AUTHORITY","kind":"ruling","owner":"backend","status":"DECIDED","dependencies":["P-HOST-PLAN-MUTATIONS","P-EXECUTION-AUTHORITY-CONSUMERS","DEP-ITEM15"],"source_span":["frozen:apps/daw_engine_main.cpp:319-329","frozen:apps/engine_produce_block.cpp:361-398","frozen:apps/engine_chain_host.cpp:142-272"],"control":"CTRL-MUTATIONS","statement":"One immutable session ExecutionSnapshot is the sole execution authority and contains one nonzero monotonic revision, the global patcher graph and owner map, and every track/master plan: ordered stable device ids, resolved VST path/name and compact index, bypass, host segments, aux/sidechain bindings, sampler identity, stable automation/mirror targets, and PatcherEvent/PatcherInstrument/PatcherAudio local-to-pooled node mappings. Under the command-thread writer lock, an authored mutation is applied to a candidate document, the whole affected snapshot is compiled and validated, and one atomic snapshot publication commits both; failure or revision exhaustion leaves the prior document and snapshot authoritative. TrackStateSnapshot.chainDevices and separately published graph/alias authorities are removed."},{"id":"R-DISPATCH-TICKET","kind":"ruling","owner":"backend","status":"DECIDED","dependencies":["R-HOST-PLAN-AUTHORITY","R-BYPASS-STAGED","P-DISPATCH-PROTOCOL-SURFACES"],"source_span":["frozen:apps/engine_audio_callback.h:34-56","frozen:apps/engine_consumer.cpp:636-665","frozen:apps/engine_restart_worker.cpp:102-165"],"control":"CTRL-MUTATIONS","statement":"One nonzero uint64 dispatch ticket binds the current mapping, host generation, exact ExecutionSnapshot revision and track/master plan identity, and successful bypass staging. Zero is withdrawn. Track and master senders acquire controllerMutex, reload the session snapshot and ticket, and refuse stale plans; TrackInfo and master handoffs snapshot the ticket with their mapping. Tickets never wrap: exhaustion stays withdrawn and reports terminal refusal."},{"id":"R-MIRROR-EPOCH","kind":"ruling","owner":"backend","status":"DECIDED","dependencies":["R-DISPATCH-TICKET","P-DISPATCH-PROTOCOL-SURFACES"],"source_span":["frozen:apps/engine_rt_helpers.cpp:33-56","frozen:apps/engine_produce_block.cpp:334-343","frozen:apps/engine_produce_block.cpp:508-516","frozen:apps/engine_producer_thread.cpp:199-229"],"control":"CTRL-MUTATIONS","statement":"Mirror readiness is one controllerMutex-guarded witness {dispatchTicket, mirrorEpoch, replayGate, causes, stage}, where stage is Pending, Primed, Complete, or FailedPermanent. Arm, atomic-batch prime, acknowledgement, re-arm, and failure transition that tuple under the owning lock; no loose atomic pair is a transaction. Epochs and gates are monotonic nonzero uint64 values and never wrap; exhaustion withdraws the ticket."},{"id":"R-R13-RECONCILIATION","kind":"ruling","owner":"backend","status":"DECIDED","dependencies":["R-MIRROR-EPOCH"],"source_span":"predecessor:docs/architecture/tasks/AE-P1.2-shm-contract.md:2158-2185","control":"CTRL-MUTATIONS","statement":"R13 remains valid: mirror parameters and the ReplayComplete carrying their gate token precede the primer ProcessBlock on the same ordered ring, and item 18 does not claim an intra-block parameter race. Completion waits for the production receiver's correlated acknowledgement. Gating is per host; the old global mirrorOnly scan is removed."},{"id":"R-PASS4-REPLACEMENT","kind":"ruling","owner":"backend","status":"DECIDED","dependencies":["R-R13-RECONCILIATION","R-CORRELATED-REPLAY-ACK","R-ATOMIC-PRIMER-CAPACITY"],"source_span":"manifest:/test_cases","control":"CTRL-MUTATIONS","statement":"PASS 4 is replaced: the shared production dispatch helper permits only a control-only primer while Pending/Primed and refuses ordinary MIDI, automation, render work, active publication, mapping/output mixing, and master processed-output publication until the exact witness is Complete. Primer and drain output are discarded; unrelated ready tracks continue. Atomic batch reservation publishes all mirror params plus ReplayComplete or none. Transient occupancy sends only a drain block and retries; a mirror larger than C-2 for capacity C enters FailedPermanent, withdraws the ticket, and makes offline rendering fail without output."},{"id":"R-OFFLINE-PRIMER","kind":"ruling","owner":"backend","status":"DECIDED","dependencies":["R-PASS4-REPLACEMENT","R-MASTER-CORRELATION","P-OFFLINE-OUTPUT-SURFACES"],"source_span":["frozen:apps/engine_audio_callback.h:900-980","frozen:apps/engine_master_render.cpp:1-150"],"control":"CTRL-MUTATIONS","statement":"The production offline coordinator has explicit MappingReady, ControlPreroll, ResetAcknowledged, Playing, and CountedOutput phases. It completes track and master primers while transport is stopped, rejects stale mappings in both preflight readers, then resets timeline and arms playback; no primer consumes block id, advances transport, reaches capture, or increments rendered. Each counted block waits for matching track and master ticket/generation/block output, and write failure or permanent mirror failure sets renderFailed with no partial-success claim."},{"id":"R-G4-WITNESS","kind":"ruling","owner":"backend","status":"DECIDED_FOR_SUCCESSOR","dependencies":["R-DISPATCH-TICKET","R-MIRROR-EPOCH"],"source_span":"predecessor:docs/architecture/tasks/AE-P1.2-shm-contract.md:1167-1174","control":"CTRL-MUTATIONS","statement":"Future G4 dispatch identity is {hostGeneration, dispatchTicket, ExecutionSnapshot revision, mirrorEpoch, mirrorStage}. TrackInfo and master handoffs compare all fields against live state before readiness, waits, mixing, metering, or output publication; aux children inherit the parent's exact identity. Any differing field compares unequal."},{"id":"D-PRODUCTION-FIXTURE","kind":"test_decision","owner":"backend","status":"PLANNED","dependencies":["R-PASS4-REPLACEMENT","R-MASTER-CORRELATION","R-OFFLINE-PRIMER"],"source_span":"manifest:/test_cases","control":"CTRL-MUTATIONS","statement":"One clock-free fixture drives the exact shared production dispatch helper used by both track call branches and the master branch. Fake controller/mailbox barriers expose plan, ticket, slot, batch capacity, primer, active, disconnect, generation, block, and output witnesses; stale track mappings and stale master handoffs are rejected. A separate coordinator fixture drives the real offline state-machine seam through timeline-zero and counted output."},{"id":"R-REVIEW-GATED-AUTH","kind":"authorization","owner":"backend","status":"CONDITIONAL","dependencies":["G-ITEM18"],"source_span":"manifest:/implementation_authorization","control":"CTRL-PACKET","statement":"Product implementation is unauthorized before independent semantic and evidence PASS results name this same immutable packet SHA and frozen product. Those two PASS results authorize only the declared item-15 plus item-18 scope."},{"id":"CTRL-PACKET","kind":"control","owner":"backend","status":"EXECUTABLE","dependencies":[],"source_span":"packet:tools/architecture/ae_p1_2_g2b_item18_check.py","control":"CTRL-PACKET","statement":"The checker binds external identities, all governed blobs, exact populations, source locators, dependency closure, authorization semantics, and generated prose."},{"id":"CTRL-MUTATIONS","kind":"control","owner":"backend","status":"EXECUTABLE","dependencies":[],"source_span":"packet:tools/architecture/ae_p1_2_g2b_item18_check.py","control":"CTRL-MUTATIONS","statement":"The checker self-tests semantic and lexical population deletion/substitution, stale-plan omission, loose mirror publication, counter wrap, premature readiness, bypass overclaim, primed-is-complete, partial primer publication, stale ack, lost regression, global gating, offline leakage, authorization drift, locator substitution, and governed-byte drift."},{"id":"P-EXECUTION-AUTHORITY-CONSUMERS","kind":"population","owner":"backend","status":"EXACT_CLASSIFIED","dependencies":["DEP-FROZEN-BASE","P-HOST-PLAN-MUTATIONS"],"source_span":["manifest:/populations/execution_authority_lexical_scan","manifest:/populations/execution_authority_consumers","manifest:/populations/authored_document_exemptions"],"control":"CTRL-MUTATIONS","statement":"Fifty-seven direct chainDevices/resolveDevicePluginPath lexical hits, fifty-five semantic execution/carrier consumers, and thirteen document-only exemptions form the bounded authority population. The implementation removes chainDevices from TrackStateSnapshot, prevents execution targets from including authored chain APIs, and uses stable device ids at durable boundaries."},{"id":"P-DISPATCH-PROTOCOL-SURFACES","kind":"population","owner":"backend","status":"EXACT_CLASSIFIED","dependencies":["DEP-FROZEN-BASE"],"source_span":["manifest:/populations/process_block_senders","manifest:/populations/process_block_receiver","manifest:/populations/replay_protocol_sites","manifest:/populations/mapping_and_output_gates","manifest:/populations/capacity_contract_sites"],"control":"CTRL-MUTATIONS","statement":"The production surface is exactly three sendProcessBlock expressions in two semantic families, one receiver reached by one dispatch edge, eight replay protocol sites, ten mapping/output gates, and five capacity roots. Additions, deletions, substitutions, or unclassified missing gates fail the checker."},{"id":"P-OFFLINE-OUTPUT-SURFACES","kind":"population","owner":"backend","status":"EXACT_CLASSIFIED","dependencies":["DEP-FROZEN-BASE"],"source_span":"manifest:/populations/offline_coordinator_sites","control":"CTRL-MUTATIONS","statement":"Ten exact sites bind the real runOfflinePump invocation, mapping preflight, reset/arm ordering, counted wait/mix/increment, output writer, and producer arm observer."},{"id":"R-STABLE-DEVICE-TARGETS","kind":"ruling","owner":"backend","status":"DECIDED","dependencies":["R-HOST-PLAN-AUTHORITY","P-EXECUTION-AUTHORITY-CONSUMERS"],"source_span":["frozen:apps/automation_clip.h:169-192","frozen:apps/engine_types.h:168-171","frozen:apps/engine_device_commands.cpp:126-228"],"control":"CTRL-MUTATIONS","statement":"Automation, mirrors, editor/parameter requests, state capture, save/load, meters, modulation, sampler and patcher ownership cross durable boundaries by stable device id. Compact host indexes and pooled node ids are resolved only from the current ExecutionSnapshot after controllerMutex and ticket revalidation; they are never persisted or cached as authored identity."},{"id":"R-CORRELATED-REPLAY-ACK","kind":"ruling","owner":"backend","status":"DECIDED","dependencies":["R-MIRROR-EPOCH"],"source_span":["frozen:apps/engine_ui_publish.cpp:278-302","frozen:apps/juce_host_process_main.cpp:772-780","frozen:apps/juce_host_process_main.cpp:1095-1099"],"control":"CTRL-MUTATIONS","statement":"ReplayComplete keeps scheduling sampleTime in the block window and carries a ReplayCompletePayload containing a monotonic replayGate token. Under controllerMutex the engine allocates gate = max(lastIssuedGate, liveAck)+1, binds it to the current ticket/epoch, and fails closed at UINT64_MAX. The host publishes the maximum consumed payload gate. Therefore a retained prior acknowledgement greater than or equal to an old gate is still strictly below every newly issued gate and cannot promote a re-armed epoch."},{"id":"R-ATOMIC-PRIMER-CAPACITY","kind":"ruling","owner":"backend","status":"DECIDED","dependencies":["R-CORRELATED-REPLAY-ACK"],"source_span":["frozen:apps/event_ring.cpp:72-91","frozen:apps/host_controller.h:23-35","frozen:apps/engine_types.h:383-384"],"control":"CTRL-MUTATIONS","statement":"A production ringWriteBatch reserves N mirror entries plus one gate with one CAS and publishes all reserved ready flags or none. For power-of-two capacity C, usable=C-1 and the permanent mirror maximum is C-2 parameters: N=C-2 succeeds on an empty ring, N>=C-1 enters FailedPermanent without a ProcessBlock retry, while lesser N with transient occupancy drains and retries. Every batch result is consumed."},{"id":"R-MASTER-CORRELATION","kind":"ruling","owner":"backend","status":"DECIDED","dependencies":["R-DISPATCH-TICKET","R-MIRROR-EPOCH","P-DISPATCH-PROTOCOL-SURFACES"],"source_span":["frozen:apps/engine_master_render.cpp:19-143","frozen:apps/engine_audio_callback.h:688-738"],"control":"CTRL-MUTATIONS","statement":"Master FX uses the same dispatch/primer helper and witness as tracks. Its handoff is {hostGeneration, dispatchTicket, ExecutionSnapshot revision, blockId, sampleStart, audio}; restart or re-arm invalidates pending and cached output, and the callback publishes processed master audio only when the entire identity matches live state. Boolean hostReady/masterFxActive cannot authorize master output."},{"id":"R-PROTOCOL-VERSION","kind":"ruling","owner":"backend","status":"DECIDED","dependencies":["R-CORRELATED-REPLAY-ACK"],"source_span":["frozen:apps/ipc_protocol.h:12-53","frozen:apps/shared_memory.h:135-187","frozen:ui/daw-bridge/src/layout.rs:1-8","frozen:SHM_LAYOUT.md:217-233"],"control":"CTRL-PACKET","statement":"ReplayComplete gains a payload gate and BlockMailbox.replayAckSampleTime becomes replayAckGate. Bytes and offsets do not move, but meaning changes: kControlVersion is 15 and kShmVersion is 42 in C++, Rust, and SHM_LAYOUT.md, with existing size/offset assertions retained."},{"id":"D-PRODUCTION-RECEIVER","kind":"design","owner":"backend","status":"REQUIRED","dependencies":["R-CORRELATED-REPLAY-ACK","R-ATOMIC-PRIMER-CAPACITY","P-DISPATCH-PROTOCOL-SURFACES"],"source_span":["frozen:apps/juce_host_process_main.cpp:735-780","frozen:apps/juce_host_process_main.cpp:1073-1099","frozen:apps/juce_host_process_main.cpp:1200-1205"],"control":"CTRL-MUTATIONS","statement":"The host receiver extracts a no-JUCE production replay-consumer seam used by handleProcessBlock. An in-memory ring/mailbox test drives the real sender batch through that exact receiver, proves Param precedes ReplayComplete, ack changes only after actual consumption, retained high ack cannot answer a new gate, and runControlLoop still dispatches ProcessBlock to the seam."}]''')
EXPECTED_TESTS = json.loads(r'''[{"id":"T-COLD-REFUSES","statement":"A zero dispatch ticket refuses every ProcessBlock and every audio mapping read."},{"id":"T-ALL-PUBLISHERS","statement":"Each of the three old hostReady true paths reaches the one staged publication helper; deleting any path from the fixture population fails."},{"id":"T-PLAN-RACE","statement":"An offline or realtime dispatcher that loaded ExecutionSnapshot N and parks on controllerMutex refuses after snapshot N+1 commits; authored mutation without publication leaves every execution consumer on N, and one publication switches host map, bypass, segments, patcher graph/ownership, meters, sampler, modulation, and master together."},{"id":"T-EXACT-SLOTS","statement":"Every included multi-slot execution plan produces exactly one ordered SetBypass attempt per resolvable VST slot before the ticket publishes; non-VST topology affects segmentation but produces no SetBypass, and hostless aux produces neither host dispatch nor bypass."},{"id":"T-BYPASS-FAILURE","statement":"A forced partial-frame SetBypass failure leaves the ticket zero, requests recovery, disconnects under controllerMutex, and a stale waiter sends no ProcessBlock after acquiring."},{"id":"T-LOWER-PRIMER","statement":"With a non-empty mirror and ack parked below the payload gate, the actual shared production sender emits one atomic control-only primer batch plus ProcessBlock while ordinary work, active, track output, and master processed output remain absent; primer output is discarded."},{"id":"T-PRIMED-NOT-COMPLETE","statement":"A second producer step with mirror primed and ack still below gate remains Pending and emits no ordinary sentinel."},{"id":"T-PRIMER-CAPACITY","statement":"For capacity C, ringWriteBatch accepts exactly C-2 mirror parameters plus ReplayComplete on an empty ring; with transient occupancy it reserves none, sends only a drain ProcessBlock and retries; every result is consumed."},{"id":"T-ACK-BOUNDARY","statement":"A retained prior ack at or above the old gate remains below the newly allocated payload gate; gate-minus-one stays Pending, the exact gate transitions the same ticket/epoch under controllerMutex, and only the next dispatch may carry the ordinary sentinel."},{"id":"T-REARM-REGRESSES","statement":"Overflow re-arm advances the epoch, returns readiness to Pending, refuses the sentinel again, and cannot be completed by the prior epoch's ack."},{"id":"T-TRACK-LOCAL","statement":"A parked Pending primer on track A never suppresses ordinary dispatch and output for ready track B."},{"id":"T-OFFLINE-NO-LEAK","statement":"The real offline coordinator completes track and master control pre-roll while stopped, rejects stale preflight mappings, acknowledges reset, then produces tick-zero as counted block one; primers change no transport, block count, captured output, WAV frames, or master handoff."},{"id":"T-G4-WITNESS","statement":"TrackInfo, aux-child copies, and master handoffs compare {hostGeneration, dispatchTicket, ExecutionSnapshot revision, mirrorEpoch, mirrorStage}; any differing field refuses readiness, waits, meters, mixing, or output."},{"id":"T-STALE-SNAPSHOT-AUTHORITY","statement":"Execution modules cannot name TrackStateSnapshot.chainDevices, authored Device/TrackChain, runtime.config plugin vectors, compact persisted targets, pooled authored node ids, or masterFxActive as authority; direct and alias-laundered negative fixtures fail the scanner."},{"id":"T-STABLE-DEVICE-TARGETS","statement":"Reorder, unresolved VST insertion, and patcher reassembly preserve automation, mirror, editor, state, meter, modulation, sampler and patcher targets by stable device id while plan-local compact indexes change."},{"id":"T-CAPACITY-PERMANENT","statement":"A mirror with C-1 parameters enters FailedPermanent immediately, withdraws the ticket, emits one diagnostic, sends no primer or ordinary ProcessBlock, and makes offline render terminate with renderFailed and no output; C-2 is the exact success boundary."},{"id":"T-RECEIVER-BOUND","statement":"The production ringWriteBatch sender feeds the no-JUCE helper called by handleProcessBlock; params are applied before ReplayComplete, ack publishes only the consumed payload gate, and deleting either production call edge fails."},{"id":"T-MASTER-CORRELATION","statement":"Restart between master processing and publication changes generation/ticket and rejects the old handoff; matching current identity publishes exactly one processed block, and Pending/Failed mirror stages pass no processed master output."},{"id":"T-OFFLINE-PHASES","statement":"The production offline state machine refuses Playing before all contributing track and master witnesses are Complete and reset is acknowledged; every counted callback has one matching master completion and the WAV frame count is exactly countedBlocks*blockSize."},{"id":"T-MAPPING-PREFLIGHT","statement":"awaitAnyReadyTrack and awaitAllReadyTracks reject a stale TrackInfo mapping exactly as process and awaitNextBlock do; a current mapping with the same readiness values succeeds."},{"id":"T-PROTOCOL-VERSIONS","statement":"C++ engine/host handshake uses kControlVersion 15; C++ and Rust SHM versions are 42; ReplayCompletePayload and BlockMailbox sizes/offsets remain asserted and old semantic versions are rejected."}]''')
EXPECTED_GOVERNED.extend([
    {"path": "apps/engine_chain_host.h", "sha256": "f91cbc3e941e57939877d3a1a554d92d7a37a81050a8f71bbe942ff662b83b1a"},
    {"path": "apps/engine_producer_thread.h", "sha256": "c43ac198d38ac50dfc36c216a1108f50dc60c0e1ab0a12e0620af8108ffe138a"},
])
EXPECTED_GOVERNED.sort(key=lambda entry: (
    0 if entry["path"].startswith("apps/") else
    1 if entry["path"] == "CMakeLists.txt" else
    2 if entry["path"] == "SHM_LAYOUT.md" else 3,
    entry["path"],
))
EXPECTED_GOVERNED.append({
    "path": "apps/project_file.h",
    "sha256": "3f05c4bb710c35621020aae95d4d0ed7d88724a338e24e9b4d0266dbafae25e6",
})
EXPECTED_GOVERNED.sort(key=lambda entry: (
    0 if entry["path"].startswith("apps/") else
    1 if entry["path"] == "CMakeLists.txt" else
    2 if entry["path"] == "SHM_LAYOUT.md" else 3,
    entry["path"],
))
_expected_gate = next(record for record in EXPECTED_RECORDS if record["id"] == "G-ITEM18")
_expected_gate["dependencies"].insert(
    _expected_gate["dependencies"].index("R-PROTOCOL-VERSION"),
    "R-PROJECT-TARGET-MIGRATION",
)
next(record for record in EXPECTED_RECORDS if record["id"] == "R-ATOMIC-PRIMER-CAPACITY")["statement"] = (
    "A production ringWriteBatch reserves N mirror entries plus one gate with one CAS, writes every reserved slot with ready=0, publishes later slots first, and publishes the first reserved ready flag last; the consumer therefore observes the whole batch or none. For power-of-two capacity C, usable=C-1 and the permanent mirror maximum is C-2 parameters: N=C-2 succeeds on an empty ring, N>=C-1 enters FailedPermanent without a ProcessBlock retry, while lesser N with transient occupancy drains and retries. Every batch result is consumed."
)
EXPECTED_RECORDS.append({
    "id": "R-PROJECT-TARGET-MIGRATION", "kind": "ruling", "owner": "backend",
    "status": "DECIDED", "dependencies": ["R-STABLE-DEVICE-TARGETS"],
    "source_span": [
        "frozen:apps/project_file.cpp:24-24", "frozen:apps/project_file.cpp:775-798",
        "frozen:apps/project_file.cpp:893-895", "frozen:apps/project_file.cpp:1195-1212",
        "frozen:apps/project_file.h:298-305",
    ],
    "control": "CTRL-MUTATIONS",
    "statement": "Project schema advances 4 to 5 and writes automation target_device_id, never compact target_plugin_index. Loading schema 1-4 resolves each legacy compact index once against that track's candidate ExecutionSnapshot compaction before publication; kParamTargetAll remains all, and an unresolved or out-of-range legacy target emits a load diagnostic and disables the lane rather than silently retargeting it. Schema 5 rejects target_plugin_index.",
})
next(case for case in EXPECTED_TESTS if case["id"] == "T-PRIMER-CAPACITY")["statement"] = (
    "For capacity C, ringWriteBatch accepts exactly C-2 mirror parameters plus ReplayComplete on an empty ring; with transient occupancy it reserves none, sends only a drain ProcessBlock and retries; every result is consumed and no prefix is visible before the first reserved ready flag publishes last."
)
EXPECTED_TESTS.extend([
    {"id": "T-BATCH-VISIBILITY", "statement": "After a successful wrapped or contiguous reservation, a consumer racing every producer publication observes no batch entry until all later slots are ready and the first reserved ready flag publishes last; it then consumes the complete ordered parameter-plus-gate batch."},
    {"id": "T-PROJECT-TARGET-MIGRATION", "statement": "Schema 5 round-trips target_device_id; schema 4 compact indexes migrate through the candidate plan under reorder/unresolved-device fixtures, all-target remains all, invalid legacy targets disable with a diagnostic, and schema 5 target_plugin_index is refused."},
])
EXPECTED_META["changed_records"] = sorted(
    set(EXPECTED_META["changed_records"]) | {"R-PROJECT-TARGET-MIGRATION"}
)
EXPECTED_RECORD_IDS = {item["id"] for item in EXPECTED_RECORDS}
EXPECTED_TEST_IDS = {item["id"] for item in EXPECTED_TESTS}
EXPECTED_FROZEN = EXPECTED_META["frozen_product"]
EXPECTED_PREDECESSOR = EXPECTED_META["predecessor"]
EXPECTED_ITEM15 = EXPECTED_META["item15"]
EXPECTED_PROGRAM = EXPECTED_META["program_source"]
EXPECTED_REVISION_PREDECESSOR = EXPECTED_META["revision_predecessor"]

MUTATION_SCAN_RE = re.compile(
    r"(?:addDevice|removeDeviceById|moveDeviceById|setDevice[A-Za-z]+)\([^;]*track\.chain"
    r"|track\.chain\s*="
    r"|resetTrackContent\("
    r"|for \(auto& [^:]+:\s*[^)]*track\.chain\.devices"
    r"|auto& devices\s*=\s*[^;]*track\.chain\.devices"
)
AUTHORITY_SCAN_RE = re.compile(r"chainDevices|resolveDevicePluginPath")
PROCESS_BLOCK_SEND_RE = re.compile(r"(?:->|\.)controller\.sendProcessBlock\(")

class Refused(RuntimeError):
    pass

def refuse(condition: bool, message: str) -> None:
    if condition:
        raise Refused(message)

def git(*args: str) -> bytes:
    return subprocess.check_output(["git", *args], cwd=ROOT)

def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()

def object_tree(commit: str) -> str:
    return git("show", "-s", "--format=%T", commit).decode().strip()

def safe_repo_path(path_text: str, authority: str) -> str:
    refuse(not path_text or "\\" in path_text, f"unsafe {authority} path: {path_text}")
    relative = Path(path_text)
    refuse(relative.is_absolute() or ".." in relative.parts or "." in relative.parts,
           f"unsafe {authority} path: {path_text}")
    refuse(relative.as_posix() != path_text, f"non-canonical {authority} path: {path_text}")
    return path_text

def frozen_text(path: str) -> str:
    return git("show", f"{EXPECTED_FROZEN['commit']}:{path}").decode()

def production_sources(commit: str) -> list[str]:
    paths = git("ls-tree", "-r", "--name-only", commit, "--", "apps").decode().splitlines()
    return sorted(
        p for p in paths
        if p.endswith((".cpp", ".h")) and "_tests_main.cpp" not in p
    )

def derive_line_scan(commit: str, pattern: re.Pattern[str]) -> tuple[tuple[str, int], ...]:
    found: list[tuple[str, int]] = []
    for path in production_sources(commit):
        for line_no, line in enumerate(git("show", f"{commit}:{path}").decode().splitlines(), 1):
            if pattern.search(line):
                found.append((path, line_no))
    return tuple(sorted(found))

def derive_mutation_scan(commit: str) -> tuple[tuple[str, int], ...]:
    found: list[tuple[str, int]] = []
    for path in production_sources(commit):
        if not path.endswith(".cpp"):
            continue
        for line_no, line in enumerate(
            git("show", f"{commit}:{path}").decode().splitlines(), 1
        ):
            if MUTATION_SCAN_RE.search(line):
                found.append((path, line_no))
    return tuple(sorted(found))

def derive_authority_scan(commit: str) -> tuple[tuple[str, int], ...]:
    return derive_line_scan(commit, AUTHORITY_SCAN_RE)

def derive_process_block_senders(commit: str) -> tuple[tuple[str, int], ...]:
    return derive_line_scan(commit, PROCESS_BLOCK_SEND_RE)

def resolve_manifest_pointer(manifest: dict, locator: str) -> object:
    refuse(not locator.startswith("manifest:/"), f"unparseable manifest locator: {locator}")
    value: object = manifest
    for raw in locator[len("manifest:/"):].split("/"):
        token = raw.replace("~1", "/").replace("~0", "~")
        if isinstance(value, dict):
            refuse(token not in value, f"missing manifest pointer target: {locator}")
            value = value[token]
        elif isinstance(value, list):
            refuse(not token.isdigit(), f"non-numeric manifest index: {locator}")
            index = int(token)
            refuse(index >= len(value), f"manifest index out of range: {locator}")
            value = value[index]
        else:
            raise Refused(f"manifest pointer traverses scalar: {locator}")
    return value

def resolve_packet_path(locator: str) -> Path:
    refuse(not locator.startswith("packet:"), f"unparseable packet locator: {locator}")
    relative = Path(safe_repo_path(locator[len("packet:"):], "packet"))
    resolved = (ROOT / relative).resolve()
    refuse(ROOT.resolve() not in resolved.parents, f"packet locator escapes root: {locator}")
    refuse(not resolved.is_file(), f"missing packet locator target: {locator}")
    return resolved

def validate_locator(manifest: dict, locator: str, governed_paths: set[str]) -> None:
    if locator.startswith("manifest:/"):
        resolve_manifest_pointer(manifest, locator)
        return
    if locator.startswith("packet:"):
        resolve_packet_path(locator)
        return
    match = re.fullmatch(r"(frozen|predecessor|item15):([^:]+):(\d+)-(\d+)", locator)
    refuse(match is None, f"unparseable source locator: {locator}")
    authority, path_text, first_text, last_text = match.groups()
    path = safe_repo_path(path_text, authority)
    first, last = int(first_text), int(last_text)
    refuse(first < 1 or last < first, f"invalid source range: {locator}")
    commits = {
        "frozen": EXPECTED_FROZEN["commit"],
        "predecessor": EXPECTED_PREDECESSOR["packet_commit"],
        "item15": EXPECTED_ITEM15["packet_commit"],
    }
    lines = git("show", f"{commits[authority]}:{path}").decode().splitlines()
    refuse(last > len(lines), f"source range exceeds file: {locator}")
    if authority == "frozen":
        refuse(path not in governed_paths, f"ungoverned frozen locator: {locator}")

def render(manifest: dict) -> str:
    p = manifest["populations"]
    auth = manifest["implementation_authorization"]
    lines = [
        "# AE-P1.2 G2-B — item 18 readiness successor",
        "",
        "> Generated from AE-P1.2-g2b-item18-manifest.json; do not edit by hand.",
        "",
        f"Status: {manifest['status']}. Owner: {manifest['owner']}.",
        f"Frozen product: {manifest['frozen_product']['commit']} (tree {manifest['frozen_product']['tree']}).",
        f"Item-15 input: {manifest['item15']['packet_commit']} / manifest {manifest['item15']['manifest_sha256']}.",
        f"Revision successor to: {manifest['revision_predecessor']['packet_commit']} / manifest {manifest['revision_predecessor']['manifest_sha256']}.",
        "",
        "## Scope",
        "",
        manifest["scope"],
        "",
        "Implementation authorized before dual PASS: "
        + str(auth["before_dual_pass"]).lower() + ".",
        "Implementation authorized after same-SHA semantic and evidence PASS: "
        + str(auth["after_same_sha_semantic_and_evidence_pass"]).lower() + ".",
        "Authorized scope: " + auth["scope"],
        "",
        "## Frozen populations",
        "",
        f"- Governed files: {len(manifest['governed_files'])}.",
        f"- Readiness publishers: {len(p['host_ready_true_sites'])}.",
        f"- TrackStateSnapshot publications: {len(p['track_snapshot_publications'])}.",
        f"- Mutation lexical candidates: {len(p['chain_mutation_scan'])}.",
        f"- Authority lexical candidates: {len(p['execution_authority_lexical_scan'])}.",
        f"- Semantic execution consumers: {len(p['execution_authority_consumers'])}.",
        f"- Document-only exemptions: {len(p['authored_document_exemptions'])}.",
        f"- ProcessBlock send expressions: {len(p['process_block_senders'])}.",
        f"- Replay protocol sites: {len(p['replay_protocol_sites'])}.",
        f"- Offline coordinator sites: {len(p['offline_coordinator_sites'])}.",
        "",
        "## Review history",
        "",
    ]
    for review in manifest["review_history"]:
        lines.append(
            f"- {review['packet_commit']}: semantic {review['semantic']}, "
            f"evidence {review['evidence']}. {review['resolution']}"
        )
    lines.extend(["", "## Records", ""])
    for record in manifest["records"]:
        lines.append(
            f"- {record['id']} [{record['kind']} / {record['status']}]: "
            + record["statement"]
        )
    lines.extend(["", "## Required implementation tests", ""])
    for case in manifest["test_cases"]:
        lines.append(f"- {case['id']}: {case['statement']}")
    lines.extend(["", "## Non-goals", ""])
    lines.extend(f"- {item}" for item in manifest["non_goals"])
    return "\n".join(lines) + "\n"

def exact_meta(manifest: dict) -> dict:
    return {
        key: manifest[key]
        for key in (
            "schema", "ticket", "status", "owner", "revision_predecessor",
            "review_history", "predecessor", "item15", "program_source",
            "frozen_product", "scope", "implementation_authorization",
            "non_goals", "changed_records",
        )
    }

def validate(manifest: dict, *, verify_files: bool = True, verify_prose: bool = True) -> None:
    refuse(set(manifest) != TOP_LEVEL_KEYS, "top-level manifest shape changed")
    refuse(exact_meta(manifest) != EXPECTED_META, "packet metadata or authorization changed")
    refuse(manifest["governed_files"] != EXPECTED_GOVERNED, "governed population or digest changed")
    refuse(manifest["populations"] != EXPECTED_POPULATIONS, "machine population changed")
    refuse(manifest["records"] != EXPECTED_RECORDS, "record graph or ruling changed")
    refuse(manifest["test_cases"] != EXPECTED_TESTS, "required test set changed")

    records = manifest["records"]
    ids = [record.get("id") for record in records]
    refuse(len(ids) != len(set(ids)), "duplicate record id")
    refuse(set(ids) != EXPECTED_RECORD_IDS, "record id population changed")
    for record in records:
        deps = record.get("dependencies")
        refuse(not isinstance(deps, list), f"dependencies are not a list: {record['id']}")
        for dep in deps:
            refuse(dep not in EXPECTED_RECORD_IDS, f"dangling dependency {dep} from {record['id']}")
    tests = manifest["test_cases"]
    test_ids = [case.get("id") for case in tests]
    refuse(len(test_ids) != len(set(test_ids)), "duplicate test id")
    refuse(set(test_ids) != EXPECTED_TEST_IDS, "test id population changed")
    refuse(manifest["implementation_authorization"]["before_dual_pass"] is not False,
           "implementation authorized before dual PASS")
    refuse(manifest["implementation_authorization"]["after_same_sha_semantic_and_evidence_pass"] is not True,
           "same-SHA dual PASS no longer authorizes declared scope")

    by_id = {record["id"]: record for record in records}
    closure: set[str] = set()
    stack = ["R-REVIEW-GATED-AUTH"]
    while stack:
        current = stack.pop()
        if current in closure:
            continue
        closure.add(current)
        stack.extend(by_id[current]["dependencies"])
    refuse("G-ITEM18" not in closure or "R-PROTOCOL-VERSION" not in closure
           or "D-PRODUCTION-RECEIVER" not in closure,
           "authorization dependency closure is incomplete")

    authority = by_id["R-HOST-PLAN-AUTHORITY"]["statement"]
    refuse("session ExecutionSnapshot" not in authority or
           "one atomic snapshot publication" not in authority or
           "chainDevices" not in authority or "revision exhaustion" not in authority,
           "session authority or linearization weakened")
    stable = by_id["R-STABLE-DEVICE-TARGETS"]["statement"]
    refuse("stable device id" not in stable or "never persisted" not in stable,
           "stable target rule weakened")
    ack = by_id["R-CORRELATED-REPLAY-ACK"]["statement"]
    refuse("ReplayCompletePayload" not in ack or
           "max(lastIssuedGate, liveAck)+1" not in ack or
           "UINT64_MAX" not in ack or "strictly below" not in ack,
           "correlated acknowledgement weakened")
    capacity = by_id["R-ATOMIC-PRIMER-CAPACITY"]["statement"]
    refuse("ringWriteBatch" not in capacity or "one CAS" not in capacity or
           "ready=0" not in capacity or "first reserved ready flag last" not in capacity or
           "C-2" not in capacity or "N>=C-1" not in capacity or
           "Every batch result is consumed" not in capacity,
           "atomic or permanent capacity boundary weakened")
    master = by_id["R-MASTER-CORRELATION"]["statement"]
    refuse("hostGeneration" not in master or "dispatchTicket" not in master or
           "blockId" not in master or "Boolean hostReady/masterFxActive cannot authorize" not in master,
           "master output correlation weakened")
    offline = by_id["R-OFFLINE-PRIMER"]["statement"]
    refuse("ControlPreroll" not in offline or "transport is stopped" not in offline or
           "matching track and master" not in offline or "renderFailed" not in offline,
           "offline phase or output closure weakened")
    protocol = by_id["R-PROTOCOL-VERSION"]["statement"]
    refuse("kControlVersion is 15" not in protocol or "kShmVersion is 42" not in protocol,
           "protocol semantic version bump removed")
    project = by_id["R-PROJECT-TARGET-MIGRATION"]["statement"]
    refuse("schema advances 4 to 5" not in project or
           "target_device_id" not in project or "schema 1-4" not in project or
           "disables the lane" not in project or "Schema 5 rejects" not in project,
           "legacy project target migration weakened")
    receiver = by_id["D-PRODUCTION-RECEIVER"]["statement"]
    refuse("used by handleProcessBlock" not in receiver or
           "actual consumption" not in receiver or "runControlLoop" not in receiver,
           "production receiver binding weakened")

    governed_paths = {entry["path"] for entry in manifest["governed_files"]}
    for record in records:
        spans = record.get("source_span")
        spans = spans if isinstance(spans, list) else [spans]
        refuse(any(not isinstance(span, str) for span in spans),
               f"invalid source span shape: {record['id']}")
        for locator in spans:
            validate_locator(manifest, locator, governed_paths)

    for name, entries in manifest["populations"].items():
        if not isinstance(entries, list):
            raise Refused(f"population is not a list: {name}")
        for entry in entries:
            path = entry.get("path")
            line = entry.get("line")
            if name == "classified_non_host_chain_mutations":
                refuse(not isinstance(path, str),
                       "classified non-host population path is malformed")
                refuse(path not in governed_paths,
                       f"population path is ungoverned: {path}")
                continue
            refuse(not isinstance(path, str) or not isinstance(line, int),
                   f"population locator malformed: {name}")
            refuse(path not in governed_paths, f"population path is ungoverned: {path}")
            lines = frozen_text(path).splitlines()
            refuse(line < 1 or line > len(lines), f"population line is invalid: {path}:{line}")

    if verify_files:
        refuse(object_tree(EXPECTED_FROZEN["commit"]) != EXPECTED_FROZEN["tree"],
               "frozen product tree mismatch")
        refuse(object_tree(EXPECTED_PREDECESSOR["packet_commit"]) != EXPECTED_PREDECESSOR["packet_tree"],
               "predecessor tree mismatch")
        refuse(object_tree(EXPECTED_ITEM15["packet_commit"]) != EXPECTED_ITEM15["packet_tree"],
               "item-15 tree mismatch")
        refuse(object_tree(EXPECTED_PROGRAM["commit"]) != EXPECTED_PROGRAM["tree"],
               "program source tree mismatch")
        refuse(object_tree(EXPECTED_REVISION_PREDECESSOR["packet_commit"]) !=
               EXPECTED_REVISION_PREDECESSOR["packet_tree"],
               "revision predecessor tree mismatch")
        predecessor_bytes = git("show", f"{EXPECTED_PREDECESSOR['packet_commit']}:{PREDECESSOR_MANIFEST}")
        refuse(sha256(predecessor_bytes) != EXPECTED_PREDECESSOR["manifest_sha256"],
               "predecessor manifest digest mismatch")
        item15_bytes = git("show", f"{EXPECTED_ITEM15['packet_commit']}:{ITEM15_MANIFEST}")
        refuse(sha256(item15_bytes) != EXPECTED_ITEM15["manifest_sha256"],
               "item-15 manifest digest mismatch")
        revision_bytes = git(
            "show", f"{EXPECTED_REVISION_PREDECESSOR['packet_commit']}:{MANIFEST_PATH.relative_to(ROOT)}"
        )
        refuse(sha256(revision_bytes) != EXPECTED_REVISION_PREDECESSOR["manifest_sha256"],
               "revision predecessor manifest digest mismatch")
        for entry in EXPECTED_GOVERNED:
            path = safe_repo_path(entry["path"], "governed")
            blob = git("show", f"{EXPECTED_FROZEN['commit']}:{path}")
            refuse(sha256(blob) != entry["sha256"], f"frozen blob mismatch: {path}")
            checkout = ROOT / path
            refuse(not checkout.is_file(), f"missing governed checkout file: {path}")
            refuse(sha256(checkout.read_bytes()) != entry["sha256"], f"checkout drift: {path}")

        raw_ready = git(
            "grep", "-n", "-F", "hostReady.store(true", EXPECTED_FROZEN["commit"], "--", "apps"
        ).decode().splitlines()
        observed_ready: list[tuple[str, int]] = []
        for raw in raw_ready:
            if "_tests_main" in raw:
                continue
            match = re.fullmatch(r"[^:]+:([^:]+):(\d+):.*", raw)
            refuse(match is None, f"unparseable readiness grep line: {raw}")
            observed_ready.append((match.group(1), int(match.group(2))))
        expected_ready = sorted(
            (entry["path"], entry["line"])
            for entry in EXPECTED_POPULATIONS["host_ready_true_sites"]
        )
        refuse(sorted(observed_ready) != expected_ready, "readiness publisher census drifted")

        observed_snapshots: list[tuple[str, int, str]] = []
        for path in production_sources(EXPECTED_FROZEN["commit"]):
            source = frozen_text(path)
            for match in re.finditer(
                r"(?:std::)?atomic_store_explicit\s*\(\s*&[A-Za-z_][A-Za-z0-9_]*->trackSnapshot",
                source,
            ):
                observed_snapshots.append(
                    (path, source.count("\n", 0, match.start()) + 1, "atomic")
                )
            for match in re.finditer(
                r"[A-Za-z_][A-Za-z0-9_]*->trackSnapshot\s*=\s*buildTrackSnapshot",
                source,
            ):
                observed_snapshots.append(
                    (path, source.count("\n", 0, match.start()) + 1, "plain_prepublication")
                )
        expected_snapshots = sorted(
            (
                entry["path"], entry["line"],
                "atomic" if entry["kind"] == "atomic_unlocked_build" else entry["kind"],
            )
            for entry in EXPECTED_POPULATIONS["track_snapshot_publications"]
        )
        refuse(sorted(observed_snapshots) != expected_snapshots,
               "TrackStateSnapshot publication census drifted")

        expected_mutations = sorted(
            (entry["path"], entry["line"])
            for entry in EXPECTED_POPULATIONS["chain_mutation_scan"]
        )
        refuse(list(derive_mutation_scan(EXPECTED_FROZEN["commit"])) != expected_mutations,
               "chain mutation lexical scan drifted")
        expected_authority = sorted(
            (entry["path"], entry["line"])
            for entry in EXPECTED_POPULATIONS["execution_authority_lexical_scan"]
        )
        refuse(list(derive_authority_scan(EXPECTED_FROZEN["commit"])) != expected_authority,
               "execution authority lexical scan drifted")
        expected_senders = sorted(
            (entry["path"], entry["line"])
            for entry in EXPECTED_POPULATIONS["process_block_senders"]
        )
        refuse(list(derive_process_block_senders(EXPECTED_FROZEN["commit"])) != expected_senders,
               "ProcessBlock sender population drifted")

    if verify_prose:
        refuse(not PROSE_PATH.is_file(), "generated prose is missing")
        refuse(PROSE_PATH.read_text(encoding="utf-8") != render(manifest),
               "generated prose differs from manifest")

def self_test(manifest: dict) -> int:
    cases: list[tuple[str, dict]] = []
    def add(name: str, candidate: dict) -> None:
        cases.append((name, candidate))

    c = copy.deepcopy(manifest); c.pop("scope"); add("top-level deletion", c)
    c = copy.deepcopy(manifest); c["schema"] = "wrong"; add("schema substitution", c)
    c = copy.deepcopy(manifest); c["revision_predecessor"]["packet_commit"] = "0" * 40; add("revision identity drift", c)
    c = copy.deepcopy(manifest); c["review_history"].pop(); add("review history deletion", c)
    c = copy.deepcopy(manifest); c["frozen_product"]["tree"] = "0" * 40; add("frozen tree drift", c)
    c = copy.deepcopy(manifest); c["governed_files"].pop(); add("governed path deletion", c)
    c = copy.deepcopy(manifest); c["governed_files"][0]["sha256"] = "0" * 64; add("governed digest substitution", c)

    for population in (
        "host_ready_true_sites", "track_snapshot_publications", "host_plan_mutation_roots",
        "classified_non_host_chain_mutations", "chain_mutation_scan",
        "execution_authority_lexical_scan", "execution_authority_consumers",
        "authored_document_exemptions", "process_block_senders", "process_block_receiver",
        "replay_protocol_sites", "offline_coordinator_sites", "mapping_and_output_gates",
        "capacity_contract_sites",
    ):
        c = copy.deepcopy(manifest)
        c["populations"][population].pop()
        add(f"{population} deletion", c)

    c = copy.deepcopy(manifest); c["populations"]["execution_authority_lexical_scan"][0]["line"] += 1; add("authority scan substitution", c)
    c = copy.deepcopy(manifest); c["populations"]["process_block_senders"][0]["branch"] = "unknown"; add("sender classification substitution", c)
    c = copy.deepcopy(manifest); c["records"].pop(); add("record deletion", c)
    c = copy.deepcopy(manifest); c["records"][0]["dependencies"].append("MISSING"); add("dangling dependency", c)
    c = copy.deepcopy(manifest); c["test_cases"].pop(); add("test deletion", c)
    c = copy.deepcopy(manifest); c["implementation_authorization"]["before_dual_pass"] = True; add("premature authorization", c)
    c = copy.deepcopy(manifest); c["implementation_authorization"]["after_same_sha_semantic_and_evidence_pass"] = False; add("dual-pass authorization deletion", c)
    c = copy.deepcopy(manifest); c["non_goals"] = [x for x in c["non_goals"] if "global mirrorOnly" not in x]; add("global gate allowed", c)

    for record_id in (
        "R-HOST-PLAN-AUTHORITY", "R-STABLE-DEVICE-TARGETS", "R-DISPATCH-TICKET",
        "R-MIRROR-EPOCH", "R-CORRELATED-REPLAY-ACK", "R-ATOMIC-PRIMER-CAPACITY",
        "R-PASS4-REPLACEMENT", "R-MASTER-CORRELATION", "R-OFFLINE-PRIMER",
        "R-PROTOCOL-VERSION", "R-PROJECT-TARGET-MIGRATION", "D-PRODUCTION-RECEIVER",
    ):
        c = copy.deepcopy(manifest)
        next(r for r in c["records"] if r["id"] == record_id)["statement"] = "weakened"
        add(f"{record_id} weakening", c)

    for test_id in (
        "T-PLAN-RACE", "T-ACK-BOUNDARY", "T-CAPACITY-PERMANENT",
        "T-RECEIVER-BOUND", "T-MASTER-CORRELATION", "T-OFFLINE-PHASES",
        "T-MAPPING-PREFLIGHT", "T-PROTOCOL-VERSIONS", "T-BATCH-VISIBILITY",
        "T-PROJECT-TARGET-MIGRATION",
    ):
        c = copy.deepcopy(manifest)
        next(t for t in c["test_cases"] if t["id"] == test_id)["statement"] = "eventually succeeds"
        add(f"{test_id} weakening", c)

    refused = 0
    for name, candidate in cases:
        try:
            validate(candidate, verify_files=False, verify_prose=False)
        except Refused:
            refused += 1
        else:
            raise Refused(f"mutation was accepted: {name}")
    refuse(refused < 48, f"too few mutation controls: {refused}")
    return refused

def main() -> int:
    manifest = json.loads(MANIFEST_PATH.read_text(encoding="utf-8"))
    if "--render" in sys.argv:
        print(render(manifest), end="")
        return 0
    validate(manifest)
    controls = self_test(manifest)
    p = manifest["populations"]
    print("AE-P1.2 G2-B item 18 schema-v3 packet: PASS")
    print(f"  records: {len(manifest['records'])}")
    print(f"  tests: {len(manifest['test_cases'])}")
    print(f"  governed files: {len(manifest['governed_files'])}")
    print(f"  readiness publishers: {len(p['host_ready_true_sites'])}")
    print(f"  snapshot publications: {len(p['track_snapshot_publications'])}")
    print(f"  mutation candidates: {len(p['chain_mutation_scan'])}")
    print(f"  authority candidates: {len(p['execution_authority_lexical_scan'])}")
    print(f"  semantic consumers: {len(p['execution_authority_consumers'])}")
    print(f"  ProcessBlock senders: {len(p['process_block_senders'])}")
    print(f"  mutation controls: {controls}/{controls} refused")
    print("  implementation before dual PASS: false")
    print("  implementation after same-SHA dual PASS: true")
    return 0

if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (Refused, subprocess.CalledProcessError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        print(f"AE-P1.2 G2-B item 18 packet: REFUSED: {exc}", file=sys.stderr)
        raise SystemExit(1)
