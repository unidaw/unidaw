# AE-P0.2 ADR — Attributable, isolated build and test execution

Status: `DRAFT — CURRENT BASELINE SELECTED; REVIEW PENDING`

Date: 2026-08-10 (Europe/Helsinki)

Decision owner: `backend`

Independent reviewer: `codex-worker-2`

Historical discovery baseline: `5bef283798b59c2c4f5720292554c7ab8c265be6`

Selected execution baseline: `62bafdc6cf1cd53168ce73d098cd6acc78659be8`

Decision-record base: `158c3a81b1e72d912bfdbff881b34aa8c791535a`

This draft resolves the design questions that survived more than two independent
AE-P0.2 review cycles. It does not itself authorize ratification. During
preflight, `main` and `origin/main` advanced 223 commits beyond the frozen
discovery baseline; the current main SHA is now the selected execution baseline.
Ratification remains blocked until the inventory and evidence are refreshed at
that exact SHA, this ADR is accepted, AE-P0.1 is independently approved and
integrated on the selected baseline, and an exact non-overlapping ownership
manifest is committed.

## Context

The current verification system cannot attribute a result to the source and
artifacts that supposedly produced it:

- 103 of 211 registered CTest tests call scripts that hard-code
  `<source>/build`; another 15 honor `DAW_BUILD_DIR`; the remaining 93 use a
  compiled target, Cargo, or another entry point. `ctest --test-dir X` therefore
  does not mean that the suite executes artifacts from `X`.
- Many scripts prefer a stale `ui/target/debug/daw-cli` over any selected build.
  The engine in turn selects `./juce_host_process` and a CWD-relative plugin
  cache.
- Fixed paths, PID-derived SHM/socket names, truncated socket paths, fixed ports,
  shared project/preset paths, ambient caches, and user-global dependencies make
  simultaneous worktrees interfere.
- Configure-time metadata, a sidecar `provenance.json`, or rehashing an artifact
  at launch cannot prove which bytes and build recipe produced that artifact.
  CMake currently does not even make `patcher_rust` depend on `Cargo.lock`.
- `DAW_ENGINE_TEST_MODE=1` still opens the physical audio device. Offline render
  also probes and opens it unless `--no-audio` is explicit. The Rust e2e suite,
  nested engine tests, normal capture, `daw_audio_probe`, and `juce_host` can all
  contend for the same device while appearing headless or unrelated.
- Endpoint allocation currently binds port zero, closes it, and asks a child to
  bind later. The selected port is not reserved during that gap.

Four discovery designs were rejected independently. Repeating that loop would
violate the program's escalation rule, so the unresolved choices are decided
here before implementation.

The compiled-test inventory is closed for the inspected baseline: all 48
`apps/*_tests_main.cpp` targets and all 66 source files behind the 48 compiled
entries in the 93-test "other" partition were inspected. Fifteen files inside
that 66-file population exercise real resource authority; the remainder contain
no filesystem, IPC, process, or device authority relevant to this ADR. Direct
device entry points outside that population were inventoried separately. The
authority files are recorded under "Migration inventory" below.

## Decision drivers

1. A result must identify the exact source snapshot, build recipe, artifacts,
   runtime peers, dependencies, test selection, and evidence that produced it.
2. Verification must fail before product code runs when any identity is absent,
   malformed, stale, swapped, or inconsistent.
3. Two worktrees must be able to build and run non-device tests concurrently
   without a shared writable path, resource name, endpoint, or log.
4. Physical-device exclusion must survive crashes and cover every opener,
   including manual binaries outside CTest.
5. One schema and generator must own all cross-language contracts.
6. Incremental CMake and Cargo builds remain usable; correctness cannot depend on
   a whole-build timestamp or a single generation number.
7. Audio callback behavior remains allocation- and lock-free. This control-plane
   work may not add work to the realtime path.
8. Failure evidence must remain attributable without trusting PIDs, mtimes, or
   directory names as identity.

## Scope and threat model

AE-P0.2 protects against accidental or stale substitution, concurrent build and
runtime races, malformed documents, unintended external paths, path traversal,
escaping symlinks, partial publication, PID reuse, endpoint reuse, and process
crashes.

A malicious process with the same user identity and write access to the source,
launcher, build records, staged artifacts, or target process memory is outside
AE-P0.2. The hashes in this ADR provide integrity correlation, not
authentication against that actor. Signed CI attestations, protected signing
keys, sandboxed builders, and an in-toto/SLSA-style supply-chain trust root are a
later security decision. No release claim may imply that AE-P0.2 provides those
properties.

This boundary does not permit unsafe path handling. Documents, paths, symlinks,
permissions, schemas, and resource ownership still fail closed.

## The three-lifetime model

Build configuration, artifact creation, and test execution have different
lifetimes and must never be collapsed into one manifest.

```text
SourceSnapshotIdentity
          |
          v
     BuildContext --------> ArtifactProvenance (one per artifact generation)
                                  |
                                  v
                         AssemblyContext
                                  |
                                  v
InvocationContext -------> ExecutionGraph -------> staged immutable closure
          |                       |
          v                       v
  child RunContexts ----------> RunResult
```

### `BuildContext`: configure-time authority

One source-resident launcher creates a content-addressed `BuildContext` for an
explicit source snapshot and explicit build directory. It records at least:

- schema and document IDs;
- canonical source/build path references;
- source snapshot ID, Git HEAD, dirty classification, and submodule identities;
- generator, configuration set, target platform, compiler, linker, SDK/sysroot,
  and closed build-affecting environment;
- dependency identities available at configure time;
- protocol/layout fingerprints and selected feature configuration;
- creation tool identity and creation time for diagnostics only.

Reconfiguration publishes another immutable context. It never overwrites the
context referenced by an older artifact or result.

### `ArtifactProvenance`: build-time authority

Every object, archive, executable, library, bundle, generated file, Rust target,
script, and runtime helper selected for an attributable run has an immutable
record for the generation that created or selected it. Per-artifact generations
allow a valid incremental build; one build-wide generation equality is rejected.

Compiled-artifact records include:

- the producing `BuildContext` and `SourceSnapshotIdentity`;
- normalized compiler/linker commands, response files, flags, definitions,
  target, SDK/sysroot, and a closed environment allowlist;
- exact source, generated, build-script, proc-macro, native, object, archive, and
  linker inputs with digests captured when consumed;
- the immutable `ActionInputManifest` that bounded every producing action's
  readable source namespace;
- a compile-unit closure and true linker-input closure;
- dependency and build-recipe digests;
- compiled protocol/ABI/capability contract digest;
- target/configuration/profile and a unique build-action generation ID;
- final output kind, exact byte length, and SHA-256 for a regular file, or the
  canonical complete tree identity for a directory/bundle.

The distinction below is normative:

- `link_inputs` are bytes consumed by the linker.
- `runtime_requires` are protocol, ABI, feature, and capability predicates
  compiled into a consumer.
- `provides_contract` describes a runtime provider.
- `assembly` selects concrete providers whose contracts satisfy those
  predicates.

`juce_host_process` is therefore a runtime provider for `daw_engine`, not a fake
link input. A compatible host relink can be selected without rebuilding the
engine; an incompatible provider is rejected.

Each final compiled artifact embeds a digest recomputable from its closed record:

```text
SHA-256(JCS({
  build_context_id,
  source_snapshot_id,
  target_id,
  configuration,
  profile,
  compile_unit_closure_digest,
  true_link_input_digest,
  build_recipe_digest,
  dependency_identity_digest,
  compiled_contract_digest,
  build_action_generation_id
}))
```

Preflight extracts that digest from the selected binary and independently
recomputes it. Comparing two attacker-editable strings is not validation.

Artifact publication is deliberately non-circular: the build embeds the
provenance digest above, finishes and closes the artifact, hashes the complete
output, then publishes `ArtifactProvenance` by no-replace atomic publication.
The complete-output digest is not embedded in the output whose bytes it hashes.
A later bit flip outside the embedded section or a changed bundle member is
therefore detected.

### `AssemblyContext`: compatible incremental selection

One immutable `AssemblyContext` selects the concrete artifacts, scripts,
bundles, fixtures, and system-tool boundary for a run. It names every artifact's
own BuildContext and source snapshot; those IDs need not be globally equal for a
daily incremental assembly. It also records the complete current checkout
identity against which the assembly was selected.

For each selected artifact, preflight recomputes the current checkout bytes for
that artifact's exact source/configuration/build-recipe input closure and
requires equality with the closure captured when it was built. It separately
validates true link inputs and `runtime_requires`/`provides_contract` edges.
Thus an unrelated source edit does not invalidate an unchanged artifact, while
editing any byte that could have affected it does. An input absent from the
declared closure is a hermetic-build failure, not permission to guess.

"Could have affected" means every source entry made readable to a producing
action, not only paths later reported by a depfile or successful open trace.
Each action consumes a generated, content-addressed `ActionInputManifest` whose
entries are descriptor-relative paths into the sealed snapshot. The sandbox
materializes only those entries at the action's logical source root and denies
all other source paths. Directory enumeration requires the complete enumerated
directory tree in the manifest. A tool whose source reads cannot be restricted
must conservatively name the whole readable subtree or snapshot, which is then
the artifact closure and intentionally sacrifices unrelated-edit reuse.

Baseline and release assemblies are stricter: all repository-built artifacts
must name one clean SourceSnapshotIdentity and one compatible configuration
family. Mixed-snapshot assemblies are labeled `development` and cannot support
a clean-baseline or release claim.

### `InvocationContext` and child `RunContext`: run-time authority

The source-resident verification launcher creates one immutable
`InvocationContext` for each top-level command. It contains a fresh 128-bit
CSPRNG invocation ID, the exact ordered selection, the selected AssemblyContext
and its BuildContexts, the permitted execution graph, supervisor policy, and
evidence root.

Each selected top-level test receives its own child `RunContext` with a fresh
128-bit CSPRNG run ID, parent invocation ID, resource namespace, run-owned
scratch/staging roots, explicit inputs, explicit environment, timeouts, and
expected result cardinality. Compound runners such as the web suite declare
their ordered internal selection rather than hiding it in prose.

`RunResult` binds the invocation, every child context, assembly/build/source/
artifact and dependency record, exact execution graph, outcome, and evidence
digests. It has
exactly one top-level outcome for every selected test and no outcome for an
unselected test. The closed outcome enum is `pass`, `fail`, `skip`, `timeout`, or
`error`; skip/error reasons are closed enums, not arbitrary success-shaped text.

The broker owns a hash-chained, fsynced outcome journal and normally holds the
result-writer lease. An independent per-run lifecycle sentinel holds the active-
run lease and containment authority. Supervisor events feed the journal; the
later lifecycle section defines failover under kernel leases so a surviving
sentinel or recovery claimant can finish teardown and publish exactly one result
without treating an unlocked file as proof that descendants are gone.

## Invocation authority and direct commands

The checkout-local verification launcher is the only authority that may mint an
attributable InvocationContext. AE-P0.2 extends the AE-P0.1 launcher/root helper;
it does not create a competing root mechanism.

The following behavior is intentional:

- The launcher selects the explicit build and mints the top-level context.
- CTest entries are guarded wrappers that validate that context and mint their
  child RunContext.
- A bare `ctest`, including `ctest -R`, has no independently selected build
  anchor and therefore refuses before executing any test. Its diagnostic prints
  the exact checkout-local wrapper command.
- Direct Cargo, Node, shell, and compiled test entry points used for canonical
  verification likewise require a validated child context. Embedded
  `CARGO_MANIFEST_DIR`, CWD, PID, and ambient environment are not authority.
- Developers may build exploratory artifacts directly, but those artifacts are
  not selectable by an attributable run until their provenance is generated and
  validated through the launcher.

This chooses honest refusal over a CTest fixture or PID-derived context. A CTest
fixture cannot export a unique immutable context to sibling tests, and a static
test property cannot mint one per invocation.

## Source snapshots and dirty-tree policy

The launcher resolves its own real path, finds the checkout marker, and creates
the source anchor. It validates all of the following before a build or run:

1. anchored source equals every selected BuildContext authority source;
2. selected build realpath equals every selected BuildContext build root;
3. CMake/Cargo configuration and target recipes match their recorded contexts;
4. every document ID recomputes;
5. the complete current checkout identity equals the AssemblyContext identity
   immediately before staging and again immediately before child launch;
6. each artifact's current source/configuration input closure equals its recorded
   closure at both barriers;
7. the two complete reads are equal, so a concurrent edit cannot slip through;
8. release/baseline mode additionally has one clean whole-source snapshot shared
   by every repository-built artifact.

An attributable build compiles from a sealed, read-only materialization of the
build-relevant source bytes, not the concurrently editable checkout. It is not a
second Git worktree or object database. The snapshot identity includes native
path bytes/code units, entry type, mode, content or link target, submodules,
generated external inputs, and dirty classification.

Daily attributable builds may include tracked modifications and explicitly
declared untracked build inputs. Their bytes are part of the producing snapshot
and the consuming artifact closure. Unknown untracked or ignored build inputs
are refused. Baseline and release claims additionally require a clean tracked
tree, no undeclared untracked build inputs, one coherent snapshot, pinned
configurations, and all dependency release gates.

A Git SHA is useful metadata but never substitutes for the byte identity.

## CMake and Cargo provenance

CMake compile and link launchers create per-compile-unit records at the time
inputs are consumed, then aggregate them at archive/link time. Depfiles are an
input-discovery mechanism, not temporal proof by themselves; the immutable
source snapshot and compile-time records close the edit-between-compile-and-link
window.

Configure/generation is a separately attributable action. Its input manifest is
conservative and includes every source directory it may enumerate plus all
toolchain/configuration inputs; its generated outputs are immutable inputs to
later manifests. A discovery run may propose a narrower manifest, but that
proposal becomes authority only after a second build action runs with exactly
that restricted view. Evidence from the broad discovery run cannot by itself
justify a narrow artifact closure.

Cargo runs with an explicit target directory, target/profile, `--locked`, and
`--offline`. A Rust compiler wrapper records rustc/link commands, dependency
graph, package sources/checksums, features, build-script inputs/environment/
directives/output tree, proc macros, native libraries, and final link inputs.
`Cargo.lock`, manifests, `.cargo` configuration, and every tracked build input
are explicit dependencies of the CMake integration.

Wrappers and depfiles are evidence collectors, not the hermeticity boundary.
Every attributable build action runs through a platform sandbox adapter with:

- read access only to the `ActionInputManifest` projection of the sealed source
  snapshot and content-addressed, read-only dependency/toolchain/SDK roots;
- write access only to one action-owned scratch/output tree;
- an empty/remapped home and explicit writable cache directories beneath that
  tree;
- a closed environment and executable search path;
- network denied, including from Cargo build scripts and proc macros;
- child execution restricted to the declared tool closure.

An entire exposed source subtree, SDK, toolchain, or dependency root is recorded
conservatively when exact per-file restriction is unavailable. This is larger
than a depfile closure but still complete and immutable. Read tracing may prove
that the sandbox denied an undeclared access; it may not retroactively omit a
readable input. A platform without a tested sandbox adapter may run an explicitly
labeled developer build, but cannot publish attributable ArtifactProvenance or a
baseline/release result. The initial adapters must cover supported macOS and
Windows hosts; CI negative controls prove denied reads, writes, network, and
child execution instead of trusting the adapter name.

## Immutable execution closure

Mutable build outputs are never executed after verification. Every selected
runtime artifact is independently copied or proven copy-on-write cloned into an
exclusively created run-owned staging tree. Hardlinks are forbidden because a
linker can mutate the shared inode after validation.

Materialization uses descriptor-relative, no-follow operations; temporary
destinations; atomic rename; final tree hashing; and read-only permissions.
Bundle trees are staged in full. Symlinks are recorded and must resolve within
the staged tree. Escaping links, special files, duplicate destinations, and path
normalization collisions are refused.

The materializer opens the selected source artifact without following links,
compares its complete file/tree identity and length to ArtifactProvenance, copies
from that stable descriptor, and validates both source and destination after the
copy. Replacement before or during the copy therefore either stages the
already-open validated bytes or refuses; replacement after publication cannot
affect the independent staged inode/tree.

The closed `ExecutionGraph` has typed nodes:

- `executable`, `interpreter`, `script`, `runtime_helper`, `loaded_bundle`,
  `static_link_input`, `dynamic_link_input`, and `system_tool`;

and typed edges:

- `executes`, `interprets`, `spawns`, `loads`, `linked_against`,
  `runtime_requires`, and `provides_contract`.

Every selected runtime path maps to exactly one staged node; every node is
reachable from one selected root; every required edge is present; and no
verified-but-unused node may substitute for an executed node. Bash, Node,
Python, CMake, CTest, compilers, linkers, OS loaders, and SDKs are explicit
system-tool/platform-boundary nodes with realpath, digest, version, dynamic
dependency, and OS/SDK identity.

Execution uses a second platform sandbox adapter. It remaps HOME and every known
cache/temp/config variable to run-owned paths; removes loader/import steering
such as `PATH` entries outside the graph, `NODE_PATH`, Python path variables,
dynamic-loader injection, and ambient plugin/cache variables; exposes only the
staged closure plus declared read-only fixtures/resources; permits writes only
to RunContext-owned roots; restricts process execution and dynamic loads to
declared graph nodes or the recorded system boundary; and denies network except
for explicit authenticated loopback endpoint edges. A declared graph without
this enforcement is invalid. Plugin resource trees are part of the staged
bundle node, and the host never receives the original machine bundle path.

## Canonical schemas and document publication

JSON Schema 2020-12 is the normative schema. One generator emits C++, Rust, and
TypeScript types, validators, canonical writers, namespace-vector code, and test
fixtures. Shell code calls one generated validator/reader CLI and never parses
these documents itself.

The normative source is a content-addressed `SchemaBundleIdentity`. Each schema
ID is:

```text
SHA-256("daw-schema-v1\0" || JCS(schema_without_self_id))
```

The bundle deterministically lists every schema ID/canonical-byte digest,
namespace/domain table, integer grammar, array sort rule, generator source/tool
identity, and expected generated-output digest. Its own ID uses the same
body-minus-self domain-separated rule. Every generated validator and writer
embeds the accepted bundle ID; every document names its schema and bundle IDs.
Unknown bundles fail closed. Supporting an older compatible bundle requires an
explicit generated allowlist and downgrade test; replacing both a stale schema
and stale generated validator cannot silently redefine current acceptance.

The accepted bundle is not self-authenticating. A separately owned,
hand-reviewed `SchemaTrustAnchor` is part of the checkout-local root launcher
bootstrap and contains the ratified current bundle ID, the expected generated
validator ID/output digest, and an explicit compatibility allowlist. The
bootstrap parser for this tiny anchor is independent of the generated schema
validator. Validation order is fixed: the root launcher validates the anchor's
canonical bytes, computes the candidate bundle ID with the fixed bootstrap
algorithm, compares it to the anchor, verifies the generated validator's
embedded bundle ID and output digest against the anchor, and only then invokes
that validator for ordinary documents. A stale schema plus matching stale
validator is therefore rejected by a current launcher even when its old
content-addressed bundle remains stored. A compatibility or rollback entry is
accepted only when explicitly present in the anchor and covered by a downgrade
test. The anchor itself is changed only by an independently reviewed bootstrap
commit and is included in ownership and baseline manifests.

The schema rules are:

- `additionalProperties: false` at every object boundary;
- closed enums and tagged unions;
- duplicate JSON keys rejected before parsing/canonicalization;
- RFC 8785 JCS canonicalization and SHA-256;
- deterministic array sort keys and duplicate rejection;
- every integer-valued field represented as a canonical decimal string with a
  generated signedness/bit-width bound; there is no implementation-local
  decision about whether a value is "wide";
- native paths represented by the exact `PathRef` encoding below, never a lossy
  assumed-Unicode string;
- planned versus existing paths represented as distinct states.

`PathRef` is a closed tagged union:

```text
PosixPathRef {
  platform: "posix",
  state: "existing-absolute" | "anchor-relative-planned",
  anchor_id: <document id or empty for existing-absolute>,
  bytes_base64url: <unpadded RFC 4648 base64url of raw path bytes>
}

WindowsPathRef {
  platform: "windows",
  state: "existing-final" | "anchor-relative-planned",
  anchor_id: <document id or empty for existing-final>,
  utf16le_base64url: <unpadded base64url of little-endian UTF-16 code units>
}
```

POSIX values reject NUL, empty components, repeated separators, `.`/`..`, and a
trailing separator except for root. Existing paths are resolved component by
component with no-follow handles and record the final absolute native bytes;
planned paths are relative to an already-open typed anchor and are validated
again after exclusive creation.

Windows existing paths are opened without following reparse points and record a
volume-GUID final path obtained from the handle. Planned values reject device
names, alternate data streams, dot components, trailing-dot/space aliases, and
unvalidated reparse points; equality and containment use final handle identity,
not case-sensitive lexical prefix tests or 8.3 aliases. Lone UTF-16 surrogates
remain representable because the wire value is code units, not JSON Unicode.

Each array has a schema-declared stable sort key and duplicate rule. Stored
canonical documents must be byte-for-byte equal to their JCS serialization, so
a source file with reordered object keys is refused as noncanonical even though
JCS would give the same semantic digest. Generated golden vectors cover
non-UTF-8 POSIX bytes, NFC/NFD-distinct POSIX names, lone Windows surrogates,
case/alias/reparse cases, and integer boundaries around `2^53` and every field's
minimum/maximum.

The initially generated document set is:

- `SchemaBundleIdentity`
- `SchemaTrustAnchor`
- `SourceSnapshotIdentity`
- `BuildContext`
- `ActionInputManifest`
- `CompileUnitProvenance`
- `ArtifactProvenance`
- `DependencyIdentity`
- `AssemblyContext`
- `ExecutionGraph`
- `InvocationContext`
- `RunContext`
- `RunResult`
- `ActiveRunLocator`
- `AllocationIntent`
- `ContainmentJournalFrame`
- `ContainmentClosure`
- `PinDocument`
- `PayloadIndex`
- `PluginCacheSnapshot`
- `OwnershipManifest`
- `OwnershipTransfer`
- `GcTombstone`

Ordinary document IDs are:

```text
SHA-256("daw-doc-v1\0" || schema_id || "\0" || JCS(document_without_id))
```

Documents are published beneath their content-addressed name using an opened,
owner/mode-validated, no-follow store root; exclusive temporary creation; file
fsync/`FlushFileBuffers`; a platform no-replace move (`renameatx_np`/
`renameat2(RENAME_NOREPLACE)` or Windows no-replace move); and parent-directory
durability. If the destination already exists, the publisher validates its
canonical bytes, schema bundle, and recomputed ID and treats only an exact match
as idempotent success. A mutable "current" pointer may select a document but can
never replace its identity. Concurrent-publisher and crash-at-every-publication-
boundary tests are mandatory.

## Resource namespace

A run ID is exactly 16 CSPRNG bytes interpreted as one 128-bit value; only the
all-zero value is rejected. A stable subject ID is the full unsigned 32-bit
value. The generated track-ID contract reserves `kMasterTrackId = 0xffff0000`
exclusively for master and prevents the ordinary track allocator from issuing
it. A generated one-byte subject-kind enum (`global`, `track`, `master`,
`device`, and future closed values) provides defense in depth so equal raw IDs
of different kinds cannot alias. A resource instance ID is another unsigned
32-bit value; instance zero is valid only for domains whose generated
cardinality is exactly one per subject.

For every resource:

```text
digest = SHA-256(
  "daw-namespace-v1\0" ||
  u16be(domain_length) || domain_ascii ||
  run_id_be128 || subject_kind_u8 || stable_subject_id_be32 ||
  resource_instance_id_be32
)
token = lowercase_base32_no_padding(digest[0..16])
```

The token is 26 characters and retains 128 bits. Domains are closed generated
constants such as `ui-shm`, `track-shm`, `host-socket`, `command-socket`,
`capture`, `project`, and `event-log`. Domain separation makes UI-global subject
zero distinct from track zero; the instance field prevents same-domain resources
for one subject from aliasing.

All numeric inputs above are unsigned big-endian. `domain_length` counts ASCII
bytes. `digest[0..16]` means the first 16 bytes with an exclusive upper bound.
Base32 is RFC 4648 big-endian bit packing with alphabet
`abcdefghijklmnopqrstuvwxyz234567` and no padding. CSPRNG failure is fatal and has
no PID/time fallback. Committed literal vectors, not expectations computed by
the function under test, pin these bytes.

SHM names use a one-character generated type prefix plus the token, for example
`/u<token>` and `/t<token>`, and remain below the 31-character macOS limit. They
are created mode 0600 with exclusive creation. There is no unlink-before-create
or timer-based reuse; any collision discards the entire candidate RunContext and
mints a new run ID.

Sockets live under a short trusted per-user root:

```text
/tmp/daw-u<uid>/<base32(run_id_be128)>/s<token>.sock
```

The per-user and per-run directories are verified for owner, mode, type, and
absence of links/reparse points; the run directory is created exclusively mode
0700 and socket entries mode 0600. Full path length is checked before every
unlink, bind, and connect. No entropy is truncated and no machine-wide socket
sweep is permitted.

On Windows, shared memory uses `Local\\DAW-<token>`, named pipes use
`\\\\.\\pipe\\daw-<token>`, and run files live under a final-handle-validated
per-user LocalAppData root. Objects use a DACL limited to the user SID and
SYSTEM; named pipes reject remote clients and require first-instance creation.

Before the first resource is created, the broker takes the per-user registry
mutation lease and writes an immutable, fsynced `AllocationIntent` containing
the candidate run ID, the complete sorted resource list, exact owner tokens,
the rollback policy, and state `planned`. The intent is published beneath the
trusted registry root before any candidate run directory, SHM object, pipe, or
file exists. The broker starts the authenticated sentinel next; the sentinel
must acquire the active-run lease and initialize its durable containment
journal before allocation is allowed to proceed. There is no reserve-before-
authority interval.

Each reservation is exclusive and is followed by an fsynced intent transition
record naming the exact resource and owner token. After all resources are
validated, the intent transitions atomically to `committed` and only then may
the immutable RunContext be published. A broker, sentinel, or recovery claimant
that dies at any boundary reopens the intent, verifies each surviving resource
by owner token and no-follow handle, and either completes the remaining plan or
rolls back only resources recorded for that candidate. Ambiguous ownership
quarantines the candidate; it is never guessed or broadly swept. A collision
transitions the intent to `aborted`, releases only those exact candidate
reservations, and mints a new run ID. TCP endpoints are intentionally not values
in RunContext:
the service binds port zero while retaining the listener, and the later
generation-specific readiness record publishes the endpoint. Cross-language
golden vectors cover every domain/instance, track zero, max ordinary ID, master,
all-zero rejection, and POSIX/Windows object spelling.

AE-P0.2 avoids a shared-memory layout change. Unique exclusive creation and a
supervised no-reuse lifetime provide the P0 boundary. In-header run identity,
generation, and protocol negotiation belong to the generated wire protocol in
Phase 1.

## Endpoint supervision and discovery

All ephemeral tests and persistent web stacks use one per-user run broker, one
minimal per-run lifecycle sentinel, and one per-run service supervisor.
Consecutive ports and reserve-close-rebind are deleted.

The broker is a minimal coordination process, not a product-service host. It
authenticates callers by OS peer credentials, creates Invocation/RunContexts,
normally holds the result-writer lease, owns the outcome journal, and starts the
per-run sentinel. The sentinel is a separately built and staged lifecycle
authority: it holds the active-run lease, owns the containment handles and
durable membership registry, and starts the service supervisor. The supervisor
owns product services and endpoint readiness. Neither product code nor the
supervisor can release or impersonate the lifecycle authority.

1. The broker creates per-run control channels and a 256-bit authentication
   secret. The secret is held in memory or a run-owned 0600 secret object;
   immutable contexts contain only its ID/digest. It is never logged or retained
   in a result.
2. Each service binds loopback port zero itself and continuously owns that
   listener for the service lifetime.
3. The service reports `{run_id, service_id, generation, endpoint,
   protocol_fingerprint}` over an inherited authenticated control channel.
4. The supervisor challenges the live endpoint, validates the run/service/
   generation proof, and atomically publishes a JCS readiness record protected
   by HMAC-SHA-256 with the ephemeral run secret.
5. The page service exposes same-origin discovery. Browser and sidecar
   transports authenticate the selected run, service, and generation; a stale
   readiness file or rebound foreign port is rejected.

Run and service state are separate closed machines:

```text
run:     allocating -> starting -> ready -> stopping -> completed
                    \-> failing -> failed
          active state -> recovering -> failed

service: absent -> binding -> starting -> ready -> withdrawing -> stopped
                                      \-> restarting -> binding
          active state -------------------------------> failed
```

Every transition is journaled before its externally visible side effect.
Readiness has a bounded monotonic deadline. Before a listener is closed or a
generation is replaced, the supervisor atomically changes the active readiness
pointer to `withdrawing`; new handshakes refuse, dependencies stop in reverse
order, the process is reaped, and an immutable `withdrawn` or `failed` record is
published. Old readiness records remain evidence but are never active locators.

Ephemeral runs have restart budget zero: a child death after readiness fails the
run. Persistent development mode has a closed restart policy recorded in the
InvocationContext. The default permits at most three attempts with monotonic
delays of 250 ms, 1 s, and 4 s; a failed attempt withdraws the old generation,
and budget exhaustion fails the run. Dependent services withdraw before their
provider restarts and start only after the new provider is authenticated.
Supervisor restart always creates a new run ID.

The broker itself has a generation-specific socket/named-pipe endpoint beneath
the trusted per-user root and an OS-user-scoped singleton startup lease. A
launcher first authenticates an existing generation. If none answers, the
startup-lease winner creates a new generation and atomically publishes the
per-user broker pointer; it never reuses or unlinks the stale endpoint. Old
generation directories are later handled by the same registry/GC transaction.

An `ActiveRunLocator` lives under the trusted per-user broker root and contains
only broker endpoint, run ID, current state/generation, and the PathRef of a
0600 control-capability object. The starter prints the run ID and status/stop
command after readiness. Status and stop are serialized by the broker; stop is
idempotent, and a stale locator returns the final state rather than attaching to
a successor. The locator is atomically tombstoned after result publication and
never substitutes for a content-addressed context/result.

Ephemeral mode gives the broker a caller-liveness channel; caller EOF requests
teardown. Persistent mode leaves broker, sentinel, and supervisor, not unmanaged
children, as owners after the starter returns. The sentinel places the complete
DAW descendant tree in platform containment: a Windows kill-on-close Job Object,
or on POSIX a dedicated process group plus an inherited liveness descriptor in
every registered DAW descendant. All product spawning goes through the audited
spawn boundary. A new process blocks in a staged bootstrap before product entry;
the sentinel records and fsyncs its platform process identity and containment
membership, then releases that bootstrap. Failed registration kills the blocked
child. Source and link checks forbid an alternate spawn path, changing session/
process group, or dropping the liveness descriptor.

Teardown signals the containment unit, waits for every registered process
identity to exit, escalates, and reaps children where it is the parent. A
non-parent recovery claimant waits for kernel-confirmed exit and lets the OS
reaper collect the process. The sentinel then publishes a content-addressed
`ContainmentClosure` naming the complete membership registry and terminal exit
evidence. It retains the active-run lease until that closure is durable and
exactly one RunResult has been published.

If the supervisor dies, the surviving broker observes control EOF, tears down
the containment unit through the sentinel, replays the valid journal prefix,
marks unfinished outcomes `error: supervisor_lost`, and publishes exactly one
RunResult while holding the result-writer lease after `ContainmentClosure` is
durable. Journal frames carry length, sequence, previous hash, payload hash, and
checksum; recovery truncates an incomplete tail only. No-replace content-
addressed result publication makes retries idempotent.

If the broker dies, the sentinel remains the active-run lease holder, observes
authenticated control EOF, tears down and closes containment, then acquires the
released result-writer lease, validates the journal, and publishes
`error: coordinator_lost` before releasing the active-run lease. The broker's
death therefore never creates an unlocked-but-live interval.

If the sentinel dies while the broker survives, the broker or another recovery
claimant must first acquire the released active-run recovery lease, reconstruct
membership from the durable registry, close containment, publish
`ContainmentClosure`, and only then publish the terminal result. If broker and
sentinel both die, the next launcher or user-invoked recovery/GC command follows
the same path. Two claimants race the active-run recovery lease and then the
result-writer lease; only the winners may mutate the recovery journal or publish.

An unlocked active-run lease is only permission to investigate. It is never
evidence of inactivity and never makes payload eligible for GC. A run becomes
closed only when its valid `ContainmentClosure` accounts for every durably
registered process identity and its RunResult is present. On POSIX, recovery
matches PID plus kernel start identity before signaling, refuses an ambiguous or
reused identity, enumerates the dedicated group, and quarantines the run if
absence cannot be proved. On Windows it retains/reopens creation-time-verified
process handles and verifies the Job Object is empty after kill-on-close. A torn
membership frame yields quarantine or conservative recovery, never deletion.
Listener and control descriptors are `CLOEXEC` by default and spawn actions
expose only the generated descriptor allowlist.

## Physical-audio device lease

Until explicit `--no-audio` equivalence is proven, every current engine launch
is treated as a physical-device claimant, including test mode and offline
render. Such launches are serialized across CTest processes and worktrees.

All default-device opening is routed through one RAII lease in the shared JUCE
audio-backend boundary in `platform_juce/`. It covers `daw_engine`,
`daw_audio_probe`, `juce_host`, and future JUCE device users by construction.

The normative lifecycle is:

```text
acquire lease -> construct/probe/enumerate/open device -> run callbacks
              -> stop/join callbacks -> close/destroy device -> unlock
```

Lease acquisition is the first operation of the default-device API, before any
JUCE device construction, enumeration, capability probe, or open attempt. A
loser performs zero device work. Partial construction/open failures clean up the
backend while retaining the lease, then unlock last. Member destruction order is
tested so callbacks are stopped and the device object is closed/destroyed before
the lease handle can release.

On POSIX, the backend creates or opens one stable per-user/backend lease object
with `O_CREAT|O_RDWR|O_CLOEXEC|O_NOFOLLOW` and mode 0600, then validates owner,
regular-file type, and link count. The stable lease object is never unlinked by
run cleanup or GC. It holds an exclusive `flock` for exactly the lifecycle
above. No process forks after acquiring it; supervised spawns use `posix_spawn`
with an explicit close/allowlist action. Any unavoidable fork child closes the
descriptor before work and `_exit`s on exec failure.

On Windows, the equivalent is a non-inheritable named mutex scoped by user SID
and normalized backend/default-output class, with a DACL limited to that SID and
SYSTEM. `WAIT_ABANDONED` means the kernel transferred ownership and is recorded
diagnostically; it is not a stale-lock-breaking path. The key is deliberately
independent of RunContext; per-run lock paths would not exclude one another.

The backend retains the lease owner for the complete open-device lifetime.
Metadata is diagnostic only and never authorizes stale-lock breaking. A bounded
monotonic timeout returns the distinct fatal result `DeviceLeaseBusy`; it must
not degrade into offline timing. `--no-audio` neither probes the physical device
nor takes the lease. Kernel release on last-handle close or process death is the
only recovery mechanism.

The platform wrapper is the only link-visible default-device API. A source and
link-symbol gate rejects direct JUCE device-manager/open calls elsewhere and
requires every new opener to join the matrix test. This lease coordinates DAW
processes only; it cannot exclude unrelated third-party audio applications or a
plugin that independently opens hardware, and diagnostics must not claim that
it does.

No broad migration to `--no-audio` is authorized until one named,
content-addressed, positive-audio fixture is run three ways under exclusive
lease:

1. default device configuration;
2. explicit sample rate/block size matching that device;
3. `--no-audio` with the same explicit values.

Headers, sample content, state/event hashes, and diagnostics are compared. The
Rust engine e2e suite is then run with the existing device-derived configuration
and with the proposed pinned no-device configuration; outcomes and state/event
hashes must match. Only exact equivalence authorizes conversion of a test
population. Any difference becomes a named product defect or a new ADR, not a
waiver. A deterministic virtual paced realtime backend is a separate fixture/
stability ticket and does not weaken this lease gate.

## Dependency identity

Every selected dependency has a closed `DependencyIdentity` with origin,
resolved native path, digest method, content digest, version labels, and exact
consumers.

- JUCE and Boost use canonical build-relevant tree digests. An unversioned label
  is recorded honestly and blocks baseline/release reproducibility claims until
  the dependency is pinned or vendored.
- Rust records `Cargo.lock`, the resolved package graph, sources/checksums,
  features, target, toolchain, build scripts, proc macros, and native inputs.
- npm records its lockfile and a canonical installed-closure digest. Every
  worktree has its own ignored dependency directory.
- Compiler, linker, SDK, sysroot, target, response/configuration files, and OS
  build identity are recorded.
- Plugin bundles use canonical tree digests plus architecture, signature, and
  runtime-contract identity.

Plugin scanning uses one content-addressed cache service with a kernel-released
single-writer lease. The writer creates a `PluginCacheSnapshot` containing
scanner identity, search-set identity, schema, every bundle tree/architecture/
signature/contract identity, and deterministic entry order. It writes and
fsyncs a temporary object, publishes to the digest path with no-replace atomic
semantics, fsyncs the directory, and only then may update a non-authoritative
"latest" pointer. Recovery discards incomplete temporaries; an existing digest
with different bytes is corruption and stops publication.

A run selects an immutable snapshot by digest in AssemblyContext. Every selected
bundle is independently staged, its tree identity is compared with the snapshot
and ArtifactProvenance, and the host receives only that staged path. No run reads
an ambient CWD-relative or user-global "latest" cache. Test fixtures use an
explicit build-owned snapshot and never a machine plugin chosen by name.

Cargo and npm fetch/install are separate CAS-import operations. Attributable
builds see read-only, content-addressed registry/package stores and action-owned
writable target/cache/home directories; they never mutate a shared user cache.
Imports use the same lease, temporary-write, no-replace publication, fsync, and
crash-recovery rules. Plugin/dependency CAS payloads participate in the retention
registry and cannot be collected while an active AssemblyContext references
them.

Tree digests sort native relative paths and include entry type, content or link
target, executable mode, and runtime-significant extended attributes. Ownership
and timestamps are not content identity. Escaping links are rejected.

## Retention, recovery, and GC

BuildContext, provenance, schema, RunContext, RunResult, and compact evidence
metadata are retained for the lifetime of the selected build tree. They are not
automatically deleted by age.

Bulk staged artifacts and evidence have these default retention classes:

- pass or skip: 7 days;
- fail, timeout, error, or recovered crash: 30 days;
- pinned: indefinitely.

GC runs only through an explicit user-invoked command. It uses one trusted
per-user registry/GC lease and a nonblocking per-run kernel lease. Before the
first RunContext is published, the broker starts the authenticated sentinel in
bootstrap state and the sentinel acquires the active-run lease. It holds that
lease without a gap until containment is closed, the journal is closed, and
RunResult is published. A held lease means active. An unlocked lease with no
valid `ContainmentClosure`, or an unresolved `AllocationIntent`, means recovery
required, not inactive; PID and age never mean active or dead. GC must recover
or quarantine such intents before any retention mark or deletion.

Run storage is physically separated:

- `control/` contains sockets, capabilities, active pointers, containment
  membership journals, and other ephemeral lifecycle state removed only after
  a valid `ContainmentClosure` and terminal result;
- `metadata/` contains immutable contexts, provenance, results, indexes, pins,
  and tombstones retained for build-tree life;
- `payload/` contains staged artifacts and bulk evidence governed by 7/30-day or
  pinned retention.

`PayloadIndex` identifies every payload referenced by an immutable RunResult and
its availability/retention class. Expiry removes the payload, not the reference;
the corresponding GcTombstone lets readers report `expired` rather than
pretending the evidence never existed. `PinDocument` is an immutable,
content-addressed retention root published by an explicit pin/unpin operation.
Unpin publishes a new active pin-set document; history remains attributable.

Every metadata/result/cache publisher, pin operation, and GC mark/rename phase
serializes on the short-held registry mutation lease. GC then attempts each
candidate's run/CAS lease nonblocking. Acquiring an abandoned run lease changes
the operation into recovery: it must prove or force containment closure and
publish the terminal result before recomputing eligibility. This closes
reference-or-pin-after-mark races: a publisher either commits before the mark or
waits until GC's atomic rename transaction completes. Active sentinels and
dependency-cache consumers hold the corresponding payload lease for their full
use.

GC validates schemas and exact owned paths, refuses links/reparse points,
recovers abandoned runs first, and computes separate metadata and retained-
payload mark sets. For each deletion it writes and fsyncs a GC intent inside the
owned candidate, atomically renames that exact directory into an owned trash
root, fsyncs both parents, publishes and fsyncs `GcTombstone`, then removes only
from validated trash. Recovery handles every boundary idempotently: an intent in
the source resumes rename; an intent in trash finishes tombstone/removal; a
published tombstone makes repeated removal harmless. Immutable RunResults are
never rewritten to hide expired payloads.

## Migration inventory

The systematic compiled-test pass covered 48 test mains and 66 linked source
files. The following concrete authority files must be represented in the final
ownership manifest and migrated or explicitly proven context-free:

| Surface | Files | Current authority/risk |
|---|---|---|
| Fixed temp output | `apps/audio_io_tests_main.cpp`; `apps/device_chain_tests_main.cpp`; `apps/engine_automation_commands_tests_main.cpp` | machine-global names; incomplete cleanup |
| PID temp helper | `apps/engine_history_journal_tests_main.cpp` | PID/counter identity; cleanup exists but RunContext must own it |
| CWD fixture/output | `apps/patcher_graph_tests_main.cpp`; `apps/project_file_tests_main.cpp`; `apps/waveform_fixture_tests_main.cpp` | fixed filenames and `../presets` fallbacks |
| PID IPC | `apps/phase2_tests_main.cpp`; `apps/phase3_tests_main.cpp` | PID SHM/socket names; leaked resources |
| Nested engines | `apps/device_chain_ui_tests_main.cpp`; `apps/device_chain_ui_live_tests_main.cpp` | PID SHM/socket, CWD engine/cache/host; live variant opens device |
| Host IPC | `apps/host_controller.cpp` | unchecked `sun_path` truncation and unlink-before-bind |
| Mutable cache/project/preset | `apps/plugin_cache.cpp`; `apps/project_file.cpp`; `apps/patcher_preset_library.cpp` | ambient/CWD paths, non-run-owned writes |
| Device boundary | `platform_juce/juce_wrapper.h`; `platform_juce/juce_wrapper.cpp`; `apps/engine_startup.h`; `apps/engine_startup.cpp`; `apps/audio_probe_main.cpp`; `apps/juce_host_main.cpp` | all physical openers require one lease; these direct entry-point surfaces extend beyond the 66-file compiled-test population |

The wider migration also owns every one of the 103 hard-coded build scripts, the
15 override-aware scripts, the Rust e2e and bridge entry points, and the web
stack/discovery entry points. Their exact tracked paths are generated into the
ownership manifest; a count or glob is not ownership.

## Serialized implementation ownership

There is one explicit bootstrap exception to avoid a circular prerequisite.
After this ADR and AE-P0.1 are accepted, **Lane 0** may add only a pre-enumerated
set of new schema, generator, generated validator, bootstrap trust-anchor,
golden-vector, and bootstrap-test files. It may not edit root CMake, product
code, existing verification entry points, or protocol files. Its hand-audited
task packet is the temporary ownership authority; it receives independent review
before merge.

Lane 0 generates the closed `OwnershipManifest`/`OwnershipTransfer` schemas and
then commits `docs/architecture/tasks/AE-P0.2-ownership.json` from the current
tracked tree. That manifest enumerates every existing and planned file, one
owner, review owner, dependency lane, and transfer state. No product or migration
lane begins before the generated manifest and validator are independently
accepted. CI refuses an unlisted changed file or duplicate ownership.

Temporal ordering is proven from Git topology, not inferred from a final diff.
Each lane has a dedicated branch/worktree whose declared base is the accepted
predecessor SHA; the manifest records those dependency acceptance commits. CI
requires them to be ancestors of every lane commit and validates each commit's
changed paths. A transfer is an immutable `OwnershipTransfer` document accepted
in one commit and becomes effective only for later commits; it cannot
retroactively legalize an overlap.

The dependency lanes after bootstrap are:

1. **Contracts:** source/build launcher, RunContext/RunResult publication, schema
   integration, and root CMake. Root CMake has one owner for
   the entire ticket.
2. **Provenance:** CMake compile/link launchers, Cargo wrapper/integration,
   artifact embedding, source snapshots, and staging graph. Depends on 1.
3. **Platform:** namespace library, endpoint supervisor, JUCE device lease,
   host IPC path validation, and provider handshake. Depends on 1 and the
   artifact-contract part of 2.
4. **Compiled tests:** the complete 66-file inventory, split only along disjoint
   exact file lists. Depends on namespace, staging, and lease contracts.
5. **Shell and fixtures:** all registered shell entry points and explicit cache,
   project, capture, and log inputs. Depends on 1 through 4.
6. **Rust and web lifecycle:** bridge/agent e2e, sidecar, page discovery, and
   persistent/ephemeral supervision. Depends on 1 through 5.
7. **Integration and GC:** two-worktree adversarial gate, crash recovery,
   retention, documentation, and removal of transitional paths. Depends on all
   prior lanes.

`CMakeLists.txt`, the AE-P0.1 root helper/verification entry point,
`platform_juce/juce_wrapper.*`, `apps/host_controller.cpp`, and each protocol
hotspot may never have concurrent owners. Protocol files remain excluded from
AE-P0.2 unless a separately reviewed scope amendment proves the P0 no-layout-
change strategy impossible.

## Executable acceptance matrix

Each row is a required negative or positive control, not an optional review idea.

| Gate | Injection or setup | Required assertion |
|---|---|---|
| Bare CTest | Run `ctest -R` without an InvocationContext | Refuses before any child/artifact and prints exact wrapper command |
| Build-root swap | Point worktree A at B's complete AssemblyContext/build/artifacts | Refuses at source/build anchor before artifact read |
| Relevant source drift | Edit one input in a selected artifact's closure without rebuilding | Closure mismatch; no child starts |
| Unrelated source edit | Edit a file outside an unchanged artifact's `ActionInputManifest` closure | Development assembly may select it; result records mixed snapshots; release mode refuses |
| Conservative source closure | Let configure/build helper enumerate a whole source subtree, then edit one unread-looking file in that subtree | The entire exposed subtree is the action closure; reuse refuses until the action reruns |
| Source race | Change source during snapshot creation | Stability check refuses; no mixed snapshot |
| Dirty input | Add undeclared untracked build input | Snapshot creation refuses |
| Artifact swap before stage | Replace mutable build output after preflight but before/during copy | Stable-descriptor/source hash check stages the validated bytes or refuses; never a mixed artifact |
| Artifact swap after stage | Replace mutable build output after staged publication | Independent staged digest still runs; replacement is irrelevant |
| Output byte identity | Flip a byte outside the embedded section | Complete output digest refuses |
| Bundle byte identity | Mutate/add/remove one bundle member | Complete tree identity refuses |
| Staged mutation | Alter one staged byte before spawn | Final staged check refuses |
| Embedded identity | Forge record/source SHA without rebuilding artifact | Recomputed embedded identity disagrees and refuses |
| Graph omission | Remove host/script/plugin/helper node | Closed graph validation refuses |
| Graph surplus | Add an unreferenced verified node | Reachability/exact-mapping validation refuses |
| Runtime contract | Select compatible then incompatible relinked host | Compatible provider passes; incompatible provider refuses |
| True link input | Replace `patcher_rust` or a static input without relinking | Link-input/embedded identity refuses |
| CMake compile race | Attempt header edit during attributable build | Sealed snapshot excludes the race |
| Cargo resolution | Mutate lockfile/build-script output | `--locked --offline` or provenance refuses |
| Hermetic build read | Build script/proc macro reads a source/home file absent from its `ActionInputManifest` | Platform sandbox denies; no provenance is published |
| Discovery narrowing | Broad discovery proposes a narrow manifest, then authoritative action attempts another source read | Restricted second action denies; broad discovery evidence cannot publish the narrow artifact closure |
| Hermetic build write | Compiler/helper writes to ambient cache/source path | Platform sandbox denies; only action output changes |
| Hermetic build network | Build script attempts loopback/external network | Platform sandbox denies |
| Runtime PATH/import | Substitute PATH tool or Node/Python module outside graph | Runtime sandbox denies/refuses before launch |
| Runtime load | Attempt undeclared `dlopen`/plugin/resource read | Runtime sandbox denies and graph reconciliation fails |
| Runtime write/network | Write ambient HOME/cache or contact undeclared endpoint | Runtime sandbox denies; run-owned paths remain the only writes |
| Schema drift | Hand-edit one generated language binding | Regeneration diff fails |
| Schema swap/rollback | Replace schema and generated validator with stale matching pair | Current independently owned `SchemaTrustAnchor` refuses before generated parsing unless explicitly allowlisted |
| Schema anchor mismatch | Change bundle, validator, or anchor independently | Bootstrap validation refuses before any generated document parser is trusted |
| Document publish race | Race same-ID equal and unequal publishers; crash at each step | Exact equal bytes are idempotent; mismatch refuses; store converges without overwrite |
| Raw path vectors | Cross-language non-UTF-8, surrogate, alias, case, reparse vectors | Generated validators and canonical bytes agree exactly |
| Integer vectors | Cross-language values around `2^53` and every field bound | Canonical decimal grammar/range agrees exactly |
| Canonical JSON | Reorder keys in a stored document or mutate a value/array order | Raw non-JCS bytes or content ID/sort rule refuses |
| Result cardinality | Omit or duplicate one selected-test outcome | RunResult publication refuses |
| Bundle escape | Add external/escaping symlink or path collision | Staging refuses |
| Namespace vectors | Generate every domain/instance, track 0, max ordinary ID, and master in C++/Rust/TS | Exact distinct 26-character tokens and POSIX/Windows names agree |
| Namespace subject kind | Attempt ordinary-track/master with the same raw ID | Ordinary reserved ID refuses and low-level kind-separated tokens differ |
| Namespace cardinality | Request instance zero twice for a multi-instance domain | Schema/reservation refuses the collision |
| Run-ID collision | Force exclusive-create collision | Whole candidate context discarded; new ID; no unlink |
| Allocation crash boundary | Kill broker/sentinel after the intent and after each heterogeneous resource reservation | Recovery sees the fsynced intent, verifies owner tokens, and completes or rolls back only the exact candidate; no orphan or broad sweep |
| Allocation authority ordering | Kill or pause before sentinel lease/journal initialization | No filesystem/SHM/pipe reservation occurs; an unowned candidate cannot become visible |
| Allocation ambiguity | Replace a candidate resource or owner token during recovery | Recovery quarantines the candidate and never deletes an unrelated resource |
| Two worktrees | Concurrent representative CMake/Rust/web/SHM tests | No equal writable path, name, endpoint, capture, or log |
| Socket length | Force an overlength or linked socket root | Refuses before unlink/bind/connect |
| Endpoint race | Foreign process races old reserve-close-rebind window | Service-owned port has no gap; mismatching handshake refuses |
| Readiness authentication | Forge MAC, service ID, run ID, generation, or stale locator | Broker/client refuses without connecting to product protocol |
| Readiness timeout | Service binds or hangs without authenticated readiness | Monotonic deadline fails and rolls back the whole dependency set |
| Partial startup | Fail second service before readiness | First service stopped/reaped; records/descriptors cleaned |
| Service death | Kill ready ephemeral sidecar/page | Whole run fails and tears down |
| Persistent lifecycle | Starter exits, status queried, authenticated stop sent | Supervisor retains services, then stops/reaps cleanly |
| Caller EOF | Kill ephemeral launcher after readiness | Broker tears down descendants and publishes an error result |
| Readiness withdrawal | Race a client while a ready service stops | Pointer withdraws first; new/stale handshakes refuse |
| Restart generation | Restart allowed persistent service | New endpoint/generation; stale clients refuse; dependencies order correctly |
| Restart failure | Fail all persistent restart attempts | Budget/backoff exact; run transitions once to failed |
| Supervisor death | Kill supervisor with descendants live | Broker contains/reaps tree and publishes `supervisor_lost` once |
| Lifecycle crash matrix | Kill supervisor during start, readiness publication, restart, stop, and result handoff | Journal recovery reaches one valid failed terminal state at every boundary |
| Defiant descendant | Descendant ignores liveness EOF and graceful stop | Containment escalation kills and accounts for it before lease release |
| Broker death | Kill broker with supervisor and descendants live | Sentinel retains active-run lease, tears down, publishes `ContainmentClosure` and `coordinator_lost`, then releases |
| Sentinel death | Kill sentinel while broker and a defiant descendant live | One active-run recovery claimant reconstructs membership, kills/waits, closes containment, and publishes once |
| Authority double death | Kill broker and sentinel with descendants live, then race launcher and GC | One recovery claimant wins; no deletion occurs before identity-safe containment closure and terminal result |
| PID reuse during recovery | Reuse a recorded PID with a different kernel start identity | Recovery refuses to signal the new process and quarantines ambiguous evidence |
| Recovery race | Start two recovery claimants on one torn journal | One lease winner truncates valid tail and publishes one result |
| FD allowlist | Inspect child descriptors | Only declared control/listener descriptors inherited |
| Device opener matrix | Pair engine, audio probe, and JUCE host | Exactly one default-device opener acquires |
| Device busy | Hold lease beyond bounded timeout | `DeviceLeaseBusy`, zero loser probe/open work, never offline fallback |
| Device partial open | Fail after construction/probe/open milestones | Cleanup completes while lease held; unlock is last |
| Device close order | Block callback and request close | Callback stops/joins and device destroys before waiter acquires |
| Device crash | `SIGKILL` lease owner | Waiter acquires after kernel release |
| Lease inheritance | Exercise spawn success and exec failure, keep child alive, kill engine | Waiter acquires; no child retained the lease |
| Windows lease recovery | Abandon named mutex and race two waiters | One acquires with diagnostic; DACL and noninheritance hold |
| Device boundary | Add a direct JUCE default-device call outside wrapper | Source/link-symbol gate fails |
| No-audio | Run explicit `--no-audio` | No device probe/open and no lease |
| Device equivalence | Named three-way render plus pinned/unpinned e2e | Exact decision rule above; no conversion on mismatch |
| Dependency mutation | Change JUCE/Boost/npm/plugin content | Dependency identity refuses until rebuilt/reselected |
| Plugin snapshot | Try ambient CWD/user-global cache | Refuses; only selected immutable digest is read |
| Cache writer race | Race two scanner/import writers and kill the winner at each publish boundary | One valid CAS object or clean retry; no corrupt/latest authority |
| Plugin bundle swap | Change machine bundle after snapshot and before host load | Staged bundle digest refuses or remains unaffected |
| Active GC | Age a live locked run, or an unlocked run lacking `ContainmentClosure`, past thresholds | Held run is untouched; unlocked unfinished run is recovered or quarantined, never deleted |
| Retention classes | Age pass/fail/crash/pinned fixtures | Payload policy is exactly 7/30/indefinite |
| GC path safety | Insert link/reparse point or forged owned path | Candidate refused without broader deletion |
| GC publication race | Publish result/cache reference while GC marks | Registry serialization produces before-mark retention or after-rename refusal, never loss |
| GC pin race | Pin while GC marks/renames and crash at every boundary | Pin is retained or fails atomically; intent/tombstone recovery converges |
| Ownership | Change unlisted or multiply-owned file | Gate refuses before build/test |
| Ownership order | Start lane before predecessor SHA or use pre-transfer commit | Commit-topology/transfer gate refuses |

Every row is mandatory before AE-P0.2 closes. A full CMake build, full CTest,
Rust workspace/e2e, web suite, schema freshness, and source-tree cleanliness
then remain mandatory release gates.

## Consequences

Positive:

- A test result is a recomputable chain, not a directory containing plausible
  text.
- Incremental artifacts can coexist when their real link/runtime contracts are
  compatible.
- Mutable relinks cannot change bytes after they are selected for execution.
- Two worktrees receive disjoint runtime and writable resources.
- Endpoint and physical-device ownership survive the relevant crash cases.
- Cross-language identity and schema drift become executable failures.

Costs:

- Canonical tests move behind a wrapper; bare CTest intentionally refuses.
- Attributable builds require source materialization and compiler/Cargo
  instrumentation.
- Staging increases I/O and storage.
- Web endpoint discovery and persistent supervision are nontrivial changes.
- Release reproducibility remains blocked until unversioned external
  dependencies are pinned or vendored.
- Physical-device tests remain globally serialized until each no-device
  conversion passes the equivalence experiment.

## Rejected alternatives

- One sidecar provenance file for a whole build: cannot describe incremental
  generations or prove compile inputs.
- Artifact rehash plus a recorded source SHA: detects byte replacement but not a
  fabricated source claim.
- One build-session generation: rejects valid compatible incremental assembly.
- Hardlinked staging: mutable build writes change the supposedly staged inode.
- PID or wall-clock resource identity/recovery: fails on PID reuse, pause, and
  concurrent stale breakers.
- CTest fixture-created context: cannot export one fresh immutable value to all
  selected sibling tests safely.
- Per-test independent top-level authority: loses one selected-build anchor and
  aggregate result cardinality.
- Port reservation followed by close: retains the TOCTOU race.
- Consecutive ports: encodes an unnecessary browser/process coupling.
- Per-RunContext audio locks: two runs would take two locks for one device.
- Hand-written C++/Rust/TypeScript/shell schema mirrors: repeats the contract
  drift this architecture program is removing.
- Automatic startup GC and PID/mtime liveness: can delete a concurrent or paused
  run.

## Ratification and implementation gate

To accept this ADR:

1. `codex-worker-2` independently reviews this exact committed document and
   returns `APPROVE` or evidence-backed `CHANGES_REQUESTED`.
2. The decision owner resolves every requested change in a new commit and review
   repeats against the exact SHA.
3. The ledger records `ACCEPTED`; merely writing this file is not acceptance.

Implementation begins only after AE-P0.1 is also approved and integrated, the
exact ownership manifest is committed and validated, the physical-device owner
is clear, and no protocol merge hotspot is concurrently assigned.
