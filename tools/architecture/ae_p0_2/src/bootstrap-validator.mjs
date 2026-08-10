import crypto from "node:crypto";
import { types as utilTypes } from "node:util";

import { TRUST_ANCHOR_ID } from "../bootstrap/schema-trust-anchor-id.mjs";
import {
  decodeUtf8RejectingBom,
  documentId,
  jcs,
  parseCanonicalJson,
  schemaId,
  sha256,
  snapshotBytes,
} from "./canonical.mjs";

const HEX_DIGEST = /^[0-9a-f]{64}$/;
const ANCHOR_KEYS = [
  "anchor_id",
  "anchor_version",
  "compatibility",
  "current_bundle_id",
  "schema_id",
  "validator_sha256",
];
const BUNDLE_KEYS = ["bundle_id", "schema_id", "schemas", "version"];
const COMPATIBILITY_RECORD_KEYS = ["bundle_id", "validator_sha256"];
const SCHEMA_RECORD_KEYS = ["canonical_sha256", "path", "schema_id"];

export const REQUIRED_SCHEMA_PATHS = Object.freeze([
  "ownership-manifest.schema.json",
  "ownership-transfer.schema.json",
  "schema-bundle-identity.schema.json",
  "schema-trust-anchor.schema.json",
]);

// Uint8Array instances are mutable even when their binding is const. Snapshot
// the separately reviewed module export during bootstrap module evaluation so
// later application code cannot rewrite the root of trust in memory.
const FIXED_TRUST_ANCHOR_ID = (() => {
  const fixed = snapshotBytes(TRUST_ANCHOR_ID, "fixed trust-anchor ID");
  if (fixed.length !== 32) {
    throw new Error("fixed trust-anchor ID must be exactly 32 raw bytes");
  }
  return fixed;
})();

function reject(message) {
  throw new Error(message);
}

function assertHexDigest(value, label) {
  if (typeof value !== "string" || value.length !== 64 || !HEX_DIGEST.test(value)) {
    reject(`${label} must be 64 lowercase hexadecimal characters`);
  }
  return value;
}

function assertExactObject(value, expectedKeys, label) {
  // Besides establishing the ordinary JSON domain recursively, this rejects a
  // Proxy before ownKeys/getPrototypeOf can invoke attacker-controlled traps.
  jcs(value);
  if (value === null || typeof value !== "object" || Array.isArray(value)) reject(`${label} must be an object`);
  const keys = Object.keys(value).sort();
  if (keys.length !== expectedKeys.length || keys.some((key, index) => key !== expectedKeys[index])) {
    reject(`${label} has missing or unknown properties`);
  }
  return value;
}

function fixedAnchorIdBytes() {
  return Buffer.from(FIXED_TRUST_ANCHOR_ID);
}

function compareFixedAnchorId(anchorIdHex) {
  const candidate = Buffer.from(assertHexDigest(anchorIdHex, "anchor_id"), "hex");
  if (!crypto.timingSafeEqual(candidate, fixedAnchorIdBytes())) reject("trust anchor does not match the fixed ID");
}

function validateAnchorShape(anchor) {
  assertExactObject(anchor, ANCHOR_KEYS, "trust anchor");
  assertHexDigest(anchor.anchor_id, "anchor_id");
  if (anchor.anchor_version !== "1") reject("anchor_version must be \"1\"");
  assertHexDigest(anchor.current_bundle_id, "current_bundle_id");
  assertHexDigest(anchor.schema_id, "anchor schema_id");
  assertHexDigest(anchor.validator_sha256, "validator_sha256");
  if (!Array.isArray(anchor.compatibility)) reject("compatibility must be an array");
  let previous;
  for (const record of anchor.compatibility) {
    assertExactObject(record, COMPATIBILITY_RECORD_KEYS, "compatibility record");
    assertHexDigest(record.bundle_id, "compatibility bundle_id");
    assertHexDigest(record.validator_sha256, "compatibility validator_sha256");
    if (previous !== undefined && previous >= record.bundle_id) reject("compatibility must be sorted and unique by bundle_id");
    if (record.bundle_id === anchor.current_bundle_id) reject("compatibility must not repeat current_bundle_id");
    previous = record.bundle_id;
  }
  return anchor;
}

function anchorBody(anchor) {
  validateAnchorShape(anchor);
  return {
    anchor_version: anchor.anchor_version,
    compatibility: anchor.compatibility,
    current_bundle_id: anchor.current_bundle_id,
    schema_id: anchor.schema_id,
    validator_sha256: anchor.validator_sha256,
  };
}

export function anchorId(anchor, anchorSchemaId) {
  return documentId(anchorSchemaId, anchorBody(anchor));
}

export function validateAnchor(anchor, anchorSchemaId, expectedBundleId = undefined) {
  validateAnchorShape(anchor);
  compareFixedAnchorId(anchor.anchor_id);
  // Recompute immediately with the schema ID carried by the anchor. The later
  // bundle walk independently proves that this claimed schema ID is the current
  // trust-anchor schema's actual ADR identity.
  if (anchor.anchor_id !== anchorId(anchor, anchor.schema_id)) reject("trust anchor digest mismatch");
  if (expectedBundleId !== undefined && anchor.current_bundle_id !== expectedBundleId) {
    reject("schema bundle substitution");
  }
  if (anchor.schema_id !== anchorSchemaId) reject("trust anchor names the wrong schema ID");
  return true;
}

function validateBundleShape(bundle) {
  assertExactObject(bundle, BUNDLE_KEYS, "schema bundle");
  assertHexDigest(bundle.bundle_id, "bundle_id");
  assertHexDigest(bundle.schema_id, "bundle schema_id");
  if (bundle.version !== "1") reject("bundle version must be \"1\"");
  if (!Array.isArray(bundle.schemas) || bundle.schemas.length !== REQUIRED_SCHEMA_PATHS.length) {
    reject(`schema bundle must contain exactly ${REQUIRED_SCHEMA_PATHS.length} schema records`);
  }

  let previous;
  for (const record of bundle.schemas) {
    assertExactObject(record, SCHEMA_RECORD_KEYS, "schema record");
    if (typeof record.path !== "string" || !REQUIRED_SCHEMA_PATHS.includes(record.path)) {
      reject("schema record has an unknown path");
    }
    assertHexDigest(record.schema_id, `schema ID for ${record.path}`);
    assertHexDigest(record.canonical_sha256, `canonical digest for ${record.path}`);
    if (previous !== undefined && previous >= record.path) reject("schema records must be sorted and unique by path");
    previous = record.path;
  }
  if (bundle.schemas.some((record, index) => record.path !== REQUIRED_SCHEMA_PATHS[index])) {
    reject("schema bundle does not contain the exact required schema path set");
  }
  return bundle;
}

function bundleBody(bundle) {
  validateBundleShape(bundle);
  return { schema_id: bundle.schema_id, schemas: bundle.schemas, version: bundle.version };
}

export function bundleId(bundle, bundleSchemaId) {
  return documentId(bundleSchemaId, bundleBody(bundle));
}

function copyBytes(value, label) {
  return snapshotBytes(value, label);
}

function copySchemaBytesByName(value) {
  if (value !== null && (typeof value === "object" || typeof value === "function") && utilTypes.isProxy(value)) {
    reject("schemaBytesByName must not be a Proxy");
  }
  if (value === null || typeof value !== "object" || Array.isArray(value)) {
    reject("schemaBytesByName must be a plain object");
  }
  const prototype = Object.getPrototypeOf(value);
  if (prototype !== Object.prototype && prototype !== null) reject("schemaBytesByName must be a plain object");
  const keys = Reflect.ownKeys(value);
  if (keys.some((key) => typeof key !== "string")) reject("schemaBytesByName rejects symbol properties");
  const result = new Map();
  for (const key of keys) {
    const descriptor = Object.getOwnPropertyDescriptor(value, key);
    if (!descriptor || !("value" in descriptor) || !descriptor.enumerable) {
      reject("schemaBytesByName rejects accessors and hidden properties");
    }
    result.set(key, copyBytes(descriptor.value, `schema bytes for ${key}`));
  }
  return result;
}

function assertExactSchemaByteSet(schemaBytes, records) {
  const expected = records.map((record) => record.path);
  const actual = [...schemaBytes.keys()].sort();
  if (actual.length !== expected.length || actual.some((path, index) => path !== expected[index])) {
    reject("schemaBytesByName does not contain the exact bundle schema path set");
  }
}

function validatorBundleIdFromBytes(bytes) {
  const source = decodeUtf8RejectingBom(bytes, "generated validator");
  const declarations = [...source.matchAll(/^export const SCHEMA_BUNDLE_ID[ \t]*=[ \t]*"([0-9a-f]{64})";[ \t]*\r?$/gm)];
  if (declarations.length !== 1) reject("generated validator must contain exactly one static SCHEMA_BUNDLE_ID declaration");
  return declarations[0][1];
}

export function extractValidatorBundleId(validatorBytes) {
  return validatorBundleIdFromBytes(copyBytes(validatorBytes, "generated validator bytes"));
}

async function importVerifiedValidator(bytes) {
  const specifier = `data:text/javascript;base64,${bytes.toString("base64")}`;
  return import(specifier);
}

function readOptions(options) {
  if (options !== null && (typeof options === "object" || typeof options === "function") && utilTypes.isProxy(options)) {
    reject("bootstrap options must not be a Proxy");
  }
  if (options === null || typeof options !== "object" || Array.isArray(options)) reject("bootstrap options must be an object");
  const prototype = Object.getPrototypeOf(options);
  if (prototype !== Object.prototype && prototype !== null) reject("bootstrap options must be a plain object");
  const allowed = ["anchorBytes", "bundleBytes", "importValidator", "schemaBytesByName", "validatorBytes"];
  const keys = Reflect.ownKeys(options);
  if (keys.some((key) => typeof key !== "string")) reject("bootstrap options reject symbol properties");
  if (keys.some((key) => !allowed.includes(key))) reject("bootstrap options have unknown properties");
  const result = Object.create(null);
  for (const key of keys) {
    const descriptor = Object.getOwnPropertyDescriptor(options, key);
    if (!descriptor || !("value" in descriptor) || !descriptor.enumerable) {
      reject("bootstrap options reject accessors and hidden properties");
    }
    result[key] = descriptor.value;
  }
  for (const required of ["anchorBytes", "bundleBytes", "schemaBytesByName", "validatorBytes"]) {
    if (!Object.hasOwn(result, required)) reject(`bootstrap options are missing ${required}`);
  }
  if (Object.hasOwn(result, "importValidator")) {
    if (typeof result.importValidator !== "function" || utilTypes.isProxy(result.importValidator)) {
      reject("importValidator must be a non-Proxy function");
    }
  }
  return result;
}

// This is the only path that may hand generated validator code to the module
// loader. Every byte and identity check above the call to importValidator is
// deliberately independent of that generated code.
export async function verifyBootstrap(options) {
  const inputs = readOptions(options);
  // Copy every caller-owned byte range before validation so the hashes, source
  // inspection, and eventual import all refer to one stable snapshot.
  const anchorBytes = copyBytes(inputs.anchorBytes, "trust anchor bytes");
  const bundleBytes = copyBytes(inputs.bundleBytes, "schema bundle bytes");
  const validatorBytes = copyBytes(inputs.validatorBytes, "generated validator bytes");
  const schemaBytesByName = copySchemaBytesByName(inputs.schemaBytesByName);

  // 1. Authenticate the tiny, independently parsed anchor against the fixed
  // raw pin before using any bundle claim or evaluating generated code.
  const anchor = parseCanonicalJson(anchorBytes);
  validateAnchor(anchor, anchor.schema_id);

  // 2. Validate the candidate bundle and require the ID selected by the fixed
  // anchor. Its self-ID is recomputed only after all schema records check out.
  const bundle = parseCanonicalJson(bundleBytes);
  validateBundleShape(bundle);
  if (bundle.bundle_id !== anchor.current_bundle_id) reject("schema bundle substitution");
  assertExactSchemaByteSet(schemaBytesByName, bundle.schemas);

  const schemas = new Map();
  const records = new Map(bundle.schemas.map((record) => [record.path, record]));
  for (const record of bundle.schemas) {
    const bytes = schemaBytesByName.get(record.path);
    const schema = parseCanonicalJson(bytes);
    if (sha256(bytes) !== record.canonical_sha256) reject(`canonical schema digest mismatch for ${record.path}`);
    const computedSchemaId = schemaId(schema);
    if (computedSchemaId !== record.schema_id) reject(`schema ID mismatch for ${record.path}`);
    if (schema.$id !== `urn:daw:schema:${computedSchemaId}`) reject(`schema self-ID mismatch for ${record.path}`);
    schemas.set(record.path, schema);
  }

  const bundleSchemaId = records.get("schema-bundle-identity.schema.json").schema_id;
  if (bundle.schema_id !== bundleSchemaId) reject("schema bundle names the wrong schema ID");
  if (bundle.bundle_id !== bundleId(bundle, bundleSchemaId)) reject("schema bundle digest mismatch");
  const anchorSchemaId = records.get("schema-trust-anchor.schema.json").schema_id;
  validateAnchor(anchor, anchorSchemaId, bundle.bundle_id);

  // 3. Authenticate and inspect the exact generated source snapshot before it
  // can be evaluated. Merely importing a path and hashing a later reread would
  // leave a substitution window, so the default importer consumes these bytes.
  if (sha256(validatorBytes) !== anchor.validator_sha256) reject("generated validator byte digest mismatch");
  const embeddedBundleId = validatorBundleIdFromBytes(validatorBytes);
  if (embeddedBundleId !== bundle.bundle_id) reject("generated validator embeds the wrong schema bundle ID");

  const importer = inputs.importValidator ?? importVerifiedValidator;
  const validator = await importer(validatorBytes);
  if (validator === null || (typeof validator !== "object" && typeof validator !== "function")) {
    reject("generated validator import did not return a module namespace");
  }
  if (validator.SCHEMA_BUNDLE_ID !== embeddedBundleId) {
    reject("generated validator export does not match its authenticated embedded bundle ID");
  }
  return { anchor, bundle, schemas, validator };
}
