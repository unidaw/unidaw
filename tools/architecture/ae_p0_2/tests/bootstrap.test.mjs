import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

import { TRUST_ANCHOR_ID } from "../bootstrap/schema-trust-anchor-id.mjs";
import {
  anchorId,
  bundleId,
  extractValidatorBundleId,
  REQUIRED_SCHEMA_PATHS,
  validateAnchor,
  verifyBootstrap,
} from "../src/bootstrap-validator.mjs";
import {
  assertSortedUnique,
  canonical,
  canonicalDecimal,
  digest,
  documentId,
  documentPreimage,
  jcs,
  parseCanonicalJson,
  parseJsonRejectingDuplicates,
  schemaId,
  schemaPreimage,
  schemaWithoutSelfId,
  sha256,
} from "../src/canonical.mjs";

const read = (relativePath) => fs.readFileSync(new URL(relativePath, import.meta.url));
const canonicalBytes = (value) => Buffer.from(canonical(value), "utf8");
const cloneCanonical = (value) => parseCanonicalJson(canonicalBytes(value));

const anchorBytes = read("../bootstrap/schema-trust-anchor.json");
const bundleBytes = read("../generated/schema-bundle-identity.json");
const validatorBytes = read("../generated/validator.mjs");
const schemaBytesByName = Object.fromEntries(REQUIRED_SCHEMA_PATHS.map((name) => [name, read(`../schemas/${name}`)]));
const anchor = parseCanonicalJson(anchorBytes);
const bundle = parseCanonicalJson(bundleBytes);
const records = new Map(bundle.schemas.map((record) => [record.path, record]));
const anchorSchemaId = records.get("schema-trust-anchor.schema.json").schema_id;
const bundleSchemaId = records.get("schema-bundle-identity.schema.json").schema_id;

assert.ok(TRUST_ANCHOR_ID instanceof Uint8Array, "fixed trust-anchor ID must be raw bytes");
assert.equal(TRUST_ANCHOR_ID.byteLength, 32, "fixed trust-anchor ID must be 32 bytes");
assert.equal(Buffer.from(TRUST_ANCHOR_ID).toString("hex"), anchor.anchor_id);
assert.equal(anchorId(anchor, anchorSchemaId), anchor.anchor_id);
assert.equal(bundleId(bundle, bundleSchemaId), bundle.bundle_id);
assert.doesNotThrow(() => validateAnchor(anchor, anchorSchemaId, bundle.bundle_id));
assert.equal(extractValidatorBundleId(validatorBytes), bundle.bundle_id);

let importedBytes;
const verifiedWithSpy = await verifyBootstrap({
  anchorBytes,
  bundleBytes,
  schemaBytesByName,
  validatorBytes,
  importValidator: async (bytes) => {
    importedBytes = Buffer.from(bytes);
    return { SCHEMA_BUNDLE_ID: bundle.bundle_id };
  },
});
assert.deepEqual(importedBytes, validatorBytes, "bootstrap must import the exact bytes it authenticated");
assert.equal(verifiedWithSpy.validator.SCHEMA_BUNDLE_ID, bundle.bundle_id);

const verified = await verifyBootstrap({ anchorBytes, bundleBytes, schemaBytesByName, validatorBytes });
assert.equal(verified.validator.SCHEMA_BUNDLE_ID, bundle.bundle_id);

let byteMetadataTrapCalls = 0;
function bytesWithHostileMetadata(bytes) {
  const value = new Uint8Array(bytes);
  const trap = {
    configurable: true,
    get() {
      byteMetadataTrapCalls += 1;
      throw new Error("typed-array metadata trap ran");
    },
  };
  for (const property of ["buffer", "byteLength", "byteOffset", "length"]) {
    Object.defineProperty(value, property, trap);
  }
  Object.defineProperty(value, Symbol.iterator, trap);
  return value;
}
const hostileSchemaBytesByName = Object.fromEntries(
  Object.entries(schemaBytesByName).map(([name, bytes]) => [name, bytesWithHostileMetadata(bytes)]),
);
const verifiedHostileBytes = await verifyBootstrap({
  anchorBytes: bytesWithHostileMetadata(anchorBytes),
  bundleBytes: bytesWithHostileMetadata(bundleBytes),
  schemaBytesByName: hostileSchemaBytesByName,
  validatorBytes: bytesWithHostileMetadata(validatorBytes),
  importValidator: async () => ({ SCHEMA_BUNDLE_ID: bundle.bundle_id }),
});
assert.equal(verifiedHostileBytes.bundle.bundle_id, bundle.bundle_id);
assert.equal(byteMetadataTrapCalls, 0, "bootstrap read attacker-controlled typed-array metadata");

async function assertRejectedBeforeImport(overrides, pattern) {
  let importCalls = 0;
  await assert.rejects(
    verifyBootstrap({
      anchorBytes,
      bundleBytes,
      schemaBytesByName,
      validatorBytes,
      ...overrides,
      importValidator: async () => {
        importCalls += 1;
        return { SCHEMA_BUNDLE_ID: bundle.bundle_id };
      },
    }),
    pattern,
  );
  assert.equal(importCalls, 0, "generated code ran before bootstrap validation finished");
}

await assertRejectedBeforeImport(
  { anchorBytes: Buffer.concat([Buffer.from([0xef, 0xbb, 0xbf]), anchorBytes]) },
  /BOM/,
);
await assertRejectedBeforeImport(
  { anchorBytes: Buffer.from(` ${anchorBytes.toString("utf8")}`) },
  /not byte-for-byte/,
);
await assertRejectedBeforeImport(
  { bundleBytes: canonicalBytes({ ...bundle, unexpected: true }) },
  /missing or unknown properties/,
);
await assertRejectedBeforeImport(
  { bundleBytes: canonicalBytes({ ...bundle, version: 1 }) },
  /version/,
);
await assertRejectedBeforeImport(
  { schemaBytesByName: { ...schemaBytesByName, "ownership-transfer.schema.json": Buffer.from("{}") } },
  /top-level \$id|canonical schema digest|schema ID/,
);
await assertRejectedBeforeImport(
  { validatorBytes: Buffer.concat([validatorBytes, Buffer.from("\n")]) },
  /validator byte digest/,
);

const wrongEmbeddedId = Buffer.from(
  validatorBytes.toString("utf8").replace(
    /^(export const SCHEMA_BUNDLE_ID[ \t]*=[ \t]*")[0-9a-f]{64}(";)/m,
    `$1${"0".repeat(64)}$2`,
  ),
);
assert.equal(extractValidatorBundleId(wrongEmbeddedId), "0".repeat(64));
assert.notEqual(extractValidatorBundleId(wrongEmbeddedId), bundle.bundle_id);

assert.throws(() => validateAnchor({ ...anchor, unexpected: true }, anchorSchemaId), /missing or unknown/);
assert.throws(() => validateAnchor({ ...anchor, anchor_version: 1 }, anchorSchemaId), /anchor_version/);
assert.throws(
  () => validateAnchor({ ...anchor, compatibility: [{ bundle_id: "0".repeat(64), validator_sha256: 1 }] }, anchorSchemaId),
  /compatibility validator_sha256/,
);
for (const terminator of ["\n", "\r"]) {
  assert.throws(
    () => validateAnchor({ ...anchor, validator_sha256: anchor.validator_sha256 + terminator }, anchorSchemaId),
    /validator_sha256 must be 64 lowercase hexadecimal characters/,
  );
  assert.throws(
    () => validateAnchor({
      ...anchor,
      compatibility: [{ bundle_id: "a".repeat(64) + terminator, validator_sha256: "b".repeat(64) }],
    }, anchorSchemaId),
    /compatibility bundle_id must be 64 lowercase hexadecimal characters/,
  );
}
assert.throws(() => validateAnchor({ ...anchor, anchor_id: "0".repeat(64) }, anchorSchemaId), /fixed ID/);

// A coherent stale schema + bundle + validator + anchor chain must still fail
// against the independently pinned current anchor before generated code runs.
const staleSchemaName = "ownership-transfer.schema.json";
const originalStaleSchema = parseCanonicalJson(schemaBytesByName[staleSchemaName]);
const staleSchema = cloneCanonical(originalStaleSchema);
staleSchema.title = "coherent stale-chain negative";
staleSchema.$id = `urn:daw:schema:${schemaId(staleSchema)}`;
const staleSchemaBytes = canonicalBytes(staleSchema);
const staleSchemaBytesByName = { ...schemaBytesByName, [staleSchemaName]: staleSchemaBytes };
const staleBundle = cloneCanonical(bundle);
const staleRecord = staleBundle.schemas.find((record) => record.path === staleSchemaName);
staleRecord.schema_id = schemaId(staleSchema);
staleRecord.canonical_sha256 = sha256(staleSchemaBytes);
staleBundle.bundle_id = bundleId(staleBundle, bundleSchemaId);
const originalRecord = records.get(staleSchemaName);
const staleValidatorSource = validatorBytes.toString("utf8")
  .replace(canonical(originalStaleSchema), canonical(staleSchema))
  .replaceAll(bundle.bundle_id, staleBundle.bundle_id)
  .replaceAll(originalRecord.schema_id, staleRecord.schema_id)
  .replaceAll(originalRecord.canonical_sha256, staleRecord.canonical_sha256);
assert.match(staleValidatorSource, new RegExp(staleBundle.bundle_id));
assert.doesNotMatch(staleValidatorSource, new RegExp(bundle.bundle_id));
const staleValidatorBytes = Buffer.from(staleValidatorSource);
const staleAnchor = cloneCanonical(anchor);
staleAnchor.current_bundle_id = staleBundle.bundle_id;
staleAnchor.validator_sha256 = sha256(staleValidatorBytes);
staleAnchor.anchor_id = anchorId(staleAnchor, anchorSchemaId);
await assertRejectedBeforeImport(
  {
    anchorBytes: canonicalBytes(staleAnchor),
    bundleBytes: canonicalBytes(staleBundle),
    schemaBytesByName: staleSchemaBytesByName,
    validatorBytes: staleValidatorBytes,
  },
  /fixed ID/,
);

const cliPath = fileURLToPath(new URL("../src/validate-cli.mjs", import.meta.url));
const anchorPath = fileURLToPath(new URL("../bootstrap/schema-trust-anchor.json", import.meta.url));
const ownershipManifestPath = fileURLToPath(
  new URL("../../../../docs/architecture/tasks/AE-P0.2-ownership.json", import.meta.url),
);
const cliCurrentAnchor = spawnSync(process.execPath, [cliPath, anchorPath], {
  cwd: os.tmpdir(),
  encoding: "utf8",
});
assert.equal(cliCurrentAnchor.status, 0, cliCurrentAnchor.stderr);
assert.equal(cliCurrentAnchor.stdout.trim(), "PASS");

const cliCurrentOwnership = spawnSync(process.execPath, [cliPath, ownershipManifestPath], {
  cwd: os.tmpdir(),
  encoding: "utf8",
});
assert.equal(cliCurrentOwnership.status, 0, cliCurrentOwnership.stderr);
assert.equal(cliCurrentOwnership.stdout.trim(), "PASS");

const cliTempRoot = fs.realpathSync(os.tmpdir());
const cliTempDirectory = fs.realpathSync(
  fs.mkdtempSync(path.join(cliTempRoot, "ae-p0-2-bootstrap-cli-")),
);
assert.equal(path.dirname(cliTempDirectory), cliTempRoot);
assert.match(path.basename(cliTempDirectory), /^ae-p0-2-bootstrap-cli-[A-Za-z0-9_-]+$/);
try {
  const rogueAnchorPath = path.join(cliTempDirectory, "rogue-anchor.json");
  fs.writeFileSync(rogueAnchorPath, canonicalBytes(staleAnchor));
  const cliRogueAnchor = spawnSync(process.execPath, [cliPath, rogueAnchorPath], {
    cwd: os.tmpdir(),
    encoding: "utf8",
  });
  assert.notEqual(cliRogueAnchor.status, 0, "CLI trusted a coherent anchor that did not match the fixed pin");
  assert.match(cliRogueAnchor.stderr, /fixed ID/);

  const bomAnchorPath = path.join(cliTempDirectory, "bom-anchor.json");
  fs.writeFileSync(bomAnchorPath, Buffer.concat([Buffer.from([0xef, 0xbb, 0xbf]), anchorBytes]));
  const cliBomAnchor = spawnSync(process.execPath, [cliPath, bomAnchorPath], {
    cwd: os.tmpdir(),
    encoding: "utf8",
  });
  assert.notEqual(cliBomAnchor.status, 0, "CLI accepted a target document with a raw UTF-8 BOM");
  assert.match(cliBomAnchor.stderr, /BOM/);
} finally {
  fs.rmSync(cliTempDirectory, { recursive: true });
}

assert.equal(jcs({ b: 1, a: [true, null, -0] }), '{"a":[true,null,0],"b":1}');
assert.equal(canonical({}), "{}", "canonical publication bytes must not add an LF");
assert.equal(
  jcs({ numbers: [333333333.33333329, 1e30, 4.5, 0.002, 1e-27] }),
  '{"numbers":[333333333.3333333,1e+30,4.5,0.002,1e-27]}',
);

const sparse = [];
sparse.length = 1;
const extraArrayProperty = [];
extraArrayProperty.extra = 1;
const hiddenArrayProperty = [];
Object.defineProperty(hiddenArrayProperty, "toJSON", { value: () => ({}) });
const cyclic = {};
cyclic.self = cyclic;
const accessor = {};
Object.defineProperty(accessor, "value", { enumerable: true, get: () => 1 });
const hiddenObjectProperty = {};
Object.defineProperty(hiddenObjectProperty, "hidden", { value: 1 });
const symbolObjectProperty = { [Symbol("hidden")]: 1 };
const nonordinaryArray = [];
Object.setPrototypeOf(nonordinaryArray, null);
class NonJsonObject {}
class NonordinaryArraySubclass extends Array {}

for (const value of [
  undefined,
  () => 1,
  Symbol("value"),
  1n,
  NaN,
  Infinity,
  -Infinity,
  [undefined],
  sparse,
  extraArrayProperty,
  hiddenArrayProperty,
  cyclic,
  new Date(0),
  new NonJsonObject(),
  new NonordinaryArraySubclass(),
  nonordinaryArray,
  { toJSON() { return {}; } },
  accessor,
  hiddenObjectProperty,
  symbolObjectProperty,
  { nested: accessor },
  { nested: hiddenObjectProperty },
  { nested: symbolObjectProperty },
  { nested: new NonJsonObject() },
  { nested: nonordinaryArray },
  "\ud800",
  { "\udfff": 1 },
]) {
  assert.throws(() => jcs(value), `non-JSON-domain value was accepted: ${typeof value}`);
}

let proxyTrapCalls = 0;
const trappedProxy = new Proxy({}, {
  getPrototypeOf() { proxyTrapCalls += 1; throw new Error("trap ran"); },
  ownKeys() { proxyTrapCalls += 1; throw new Error("trap ran"); },
  getOwnPropertyDescriptor() { proxyTrapCalls += 1; throw new Error("trap ran"); },
});
assert.throws(() => jcs(trappedProxy), /Proxy/);
assert.throws(() => jcs({ nested: trappedProxy }), /Proxy/);
assert.equal(proxyTrapCalls, 0, "JCS touched a Proxy before rejecting it");
const revocable = Proxy.revocable({}, {});
revocable.revoke();
assert.throws(() => jcs(revocable.proxy), /Proxy/);
assert.throws(() => jcs(new Proxy([], {})), /Proxy/);

assert.equal(jcs(Object.assign(Object.create(null), { value: 1 })), '{"value":1}');
assert.equal(jcs(Object.freeze([1, 2])), "[1,2]");

assert.throws(() => parseJsonRejectingDuplicates('{"a":1,"a":2}'), /duplicate JSON key/);
assert.throws(() => parseJsonRejectingDuplicates('{"a":1,"\\u0061":2}'), /duplicate JSON key/);
assert.throws(() => parseJsonRejectingDuplicates(Buffer.from([0xc3, 0x28])), /UTF-8/);
const bomJson = Buffer.concat([Buffer.from([0xef, 0xbb, 0xbf]), Buffer.from('{"a":1}')]);
assert.throws(() => parseJsonRejectingDuplicates(bomJson), /BOM/);
assert.throws(() => parseCanonicalJson(bomJson), /BOM/);
assert.throws(() => parseCanonicalJson(`\ufeff{"a":1}`), /BOM/);
const sharedJsonBytes = new Uint8Array(new SharedArrayBuffer(2));
sharedJsonBytes.set(Buffer.from("{}"));
assert.throws(() => parseCanonicalJson(sharedJsonBytes), /SharedArrayBuffer/);
assert.equal(jcs(parseCanonicalJson('{"a":1,"b":2}')), '{"a":1,"b":2}');
for (const bytes of [
  '{"b":2,"a":1}',
  '{ "a":1,"b":2}',
  '{"a":1,"b":2}\n',
  '{"a":1.0,"b":2}',
  '{"a":-0,"b":2}',
]) {
  assert.throws(() => parseCanonicalJson(bytes), /not byte-for-byte/);
}

const vectors = JSON.parse(read("../testdata/golden-vectors.json"));
const preimage = vectors.literals;
assert.ok(preimage, "missing literal ADR preimage vectors");
const emptySchema = { $id: "urn:daw:test:ignored-self-id" };
assert.equal(schemaPreimage(emptySchema).toString("hex"), preimage.empty_schema_preimage_hex);
assert.equal(schemaId(emptySchema), preimage.empty_schema_id);
assert.equal(documentPreimage("0".repeat(64), {}).toString("hex"), preimage.empty_document_preimage_hex);
assert.equal(documentId("0".repeat(64), {}), preimage.empty_document_id);
assert.equal(digest("test-domain", {}), "1347a82fea5ec850870ef1971094997185e121ba632c3dc51e3b9439568cdb0b");
assert.throws(() => digest("", {}), /digest domain/);
assert.throws(() => digest("bad\0domain", {}), /digest domain/);
assert.throws(() => digest("non-ascii-\u00e5", {}), /digest domain/);
assert.throws(() => documentId("A".repeat(64), {}), /schema ID/);
for (const terminator of ["\n", "\r"]) {
  assert.throws(() => documentId("a".repeat(64) + terminator, {}), /schema ID/);
}

const selfIdSchema = {
  $id: "urn:daw:schema:first",
  $schema: "https://json-schema.org/draft/2020-12/schema",
  properties: { nested: { $id: "urn:daw:nested" }, schema_id: { const: "preserved" } },
  type: "object",
};
const withoutSelfId = schemaWithoutSelfId(selfIdSchema);
assert.equal(Object.hasOwn(withoutSelfId, "$id"), false);
assert.equal(withoutSelfId.$schema, selfIdSchema.$schema);
assert.equal(withoutSelfId.properties.nested.$id, "urn:daw:nested");
assert.equal(withoutSelfId.properties.schema_id.const, "preserved");
assert.equal(schemaId(selfIdSchema), schemaId({ ...selfIdSchema, $id: "urn:daw:schema:second" }));
assert.notEqual(
  schemaId(selfIdSchema),
  schemaId({ ...selfIdSchema, properties: { ...selfIdSchema.properties, nested: { $id: "urn:daw:other-nested" } } }),
  "nested $id must remain identity-bearing",
);
assert.notEqual(
  schemaId(selfIdSchema),
  schemaId({ ...selfIdSchema, properties: { ...selfIdSchema.properties, schema_id: { const: "changed" } } }),
  "schema_id-like document fields must remain identity-bearing",
);
assert.throws(() => schemaId({ type: "object" }), /top-level \$id/);
assert.throws(() => schemaId({ $id: 1, type: "object" }), /top-level \$id/);

assert.equal(canonicalDecimal("18446744073709551615"), 18446744073709551615n);
assert.throws(() => canonicalDecimal("18446744073709551616"), /outside unsigned 64-bit/);
assert.equal(canonicalDecimal("-9223372036854775808", { signed: true }), -9223372036854775808n);
assert.equal(canonicalDecimal("9223372036854775807", { signed: true }), 9223372036854775807n);
assert.throws(() => canonicalDecimal("-9223372036854775809", { signed: true }), /outside signed 64-bit/);
assert.throws(() => canonicalDecimal("9223372036854775808", { signed: true }), /outside signed 64-bit/);
for (const value of ["-0", "00", "+1", "1.0", "1e0"]) assert.throws(() => canonicalDecimal(value, { signed: true }));

assert.deepEqual(assertSortedUnique([{ id: "a" }, { id: "b" }], (value) => value.id), [{ id: "a" }, { id: "b" }]);
assert.throws(() => assertSortedUnique([{ id: "b" }, { id: "a" }], (value) => value.id), /unsorted/);
assert.throws(() => assertSortedUnique([{ id: "a" }, { id: "a" }], (value) => value.id), /duplicate/);

console.log("bootstrap PASS");
