import crypto from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import { types as utilTypes } from 'node:util';
import {
  canonical,
  documentId,
  documentPreimage,
  jcs,
  parseCanonicalJson,
  schemaId,
  schemaPreimage,
  sha256,
} from './canonical.mjs';
import {
  BASELINE_SHA,
  amendedBaseDeltaPaths,
  baselinePaths,
  existingPaths,
  plannedPaths,
  referencePaths,
} from './inventory.mjs';

const architectureRoot = path.resolve(import.meta.dirname, '..');
const repositoryRoot = path.resolve(architectureRoot, '../../..');
const outputRoot = path.resolve(process.env.AE_P0_2_OUTPUT_ROOT || repositoryRoot);
const schemaRoot = path.join(architectureRoot, 'schemas');
const architectureOutput = path.join(outputRoot, 'tools/architecture/ae_p0_2');
const HEX64 = /^[0-9a-f]{64}$/;

function fail(message) {
  throw new Error(message);
}

function scalarString(value, label) {
  if (typeof value !== 'string') fail(label + ' must be a string');
  for (let index = 0; index < value.length; index += 1) {
    const unit = value.charCodeAt(index);
    if (unit >= 0xd800 && unit <= 0xdbff) {
      const low = value.charCodeAt(index + 1);
      if (!(low >= 0xdc00 && low <= 0xdfff)) fail(label + ' contains a lone high surrogate');
      index += 1;
    } else if (unit >= 0xdc00 && unit <= 0xdfff) {
      fail(label + ' contains a lone low surrogate');
    }
  }
  return value;
}

function inspectJsonDomain(value, active) {
  if (value === null || ['boolean', 'string', 'number'].includes(typeof value)) return;
  if (['undefined', 'function', 'symbol', 'bigint'].includes(typeof value)) {
    fail('JSON domain rejects ' + typeof value);
  }
  if (utilTypes.isProxy(value)) fail('JSON domain rejects Proxy values');
  if (active.has(value)) fail('JSON domain rejects cyclic values');
  active.add(value);
  try {
    const isArray = Array.isArray(value);
    const prototype = Object.getPrototypeOf(value);
    if (isArray ? prototype !== Array.prototype
      : prototype !== Object.prototype && prototype !== null) {
      fail('JSON domain accepts only ordinary arrays and plain objects');
    }
    if (Object.getOwnPropertySymbols(value).length) fail('JSON domain rejects symbol properties');
    const keys = Reflect.ownKeys(value);
    if (isArray) {
      if (keys.length !== value.length + 1 || keys[keys.length - 1] !== 'length') {
        fail('JSON domain rejects sparse arrays or extra array properties');
      }
      for (let index = 0; index < value.length; index += 1) {
        if (keys[index] !== String(index)) fail('JSON domain rejects sparse arrays');
        const descriptor = Object.getOwnPropertyDescriptor(value, String(index));
        if (!descriptor || !('value' in descriptor) || !descriptor.enumerable) {
          fail('JSON domain rejects array accessors');
        }
        inspectJsonDomain(descriptor.value, active);
      }
    } else {
      for (const key of keys) {
        if (typeof key !== 'string') fail('JSON domain rejects symbol properties');
        scalarString(key, 'object key');
        const descriptor = Object.getOwnPropertyDescriptor(value, key);
        if (!descriptor || !('value' in descriptor) || !descriptor.enumerable) {
          fail('JSON domain rejects accessors and hidden properties');
        }
        inspectJsonDomain(descriptor.value, active);
      }
    }
  } finally {
    active.delete(value);
  }
}

function assertJsonDomain(value) {
  inspectJsonDomain(value, new Set());
  return value;
}

function encodeJson(value, active) {
  if (value === null) return 'null';
  if (typeof value === 'boolean') return value ? 'true' : 'false';
  if (typeof value === 'string') return JSON.stringify(scalarString(value, 'string'));
  if (typeof value === 'number') {
    if (!Number.isFinite(value)) fail('JCS rejects non-finite numbers');
    return Object.is(value, -0) ? '0' : JSON.stringify(value);
  }
  if (utilTypes.isProxy(value)) fail('JCS rejects Proxy values');
  if (active.has(value)) fail('JCS rejects cyclic values');
  active.add(value);
  try {
    if (Array.isArray(value)) {
      return '[' + value.map((item) => encodeJson(item, active)).join(',') + ']';
    }
    const keys = Object.keys(value).sort();
    return '{' + keys.map((key) => JSON.stringify(key) + ':' + encodeJson(value[key], active)).join(',') + '}';
  } finally {
    active.delete(value);
  }
}

function canonicalWriter(value) {
  assertJsonDomain(value);
  return encodeJson(value, new Set());
}

function sha256Hex(bytes) {
  return crypto.createHash('sha256').update(bytes).digest('hex');
}

function documentIdHex(schemaIdHex, body) {
  if (typeof schemaIdHex !== 'string' || schemaIdHex.length !== 64 || !HEX64.test(schemaIdHex)) {
    fail('schema ID must be 64 lowercase hexadecimal characters');
  }
  const preimage = Buffer.concat([
    Buffer.from('daw-doc-v1\0', 'ascii'),
    Buffer.from(schemaIdHex, 'hex'),
    Buffer.from([0]),
    Buffer.from(canonicalWriter(body), 'utf8'),
  ]);
  return sha256Hex(preimage);
}

function withoutOwnId(value, idField) {
  const body = Object.create(null);
  for (const key of Object.keys(value)) {
    if (key !== idField) body[key] = value[key];
  }
  return body;
}

function resolveSchemaReference(rootSchema, reference) {
  if (typeof reference !== 'string' || !reference.startsWith('#/$defs/')) {
    fail('unsupported schema reference ' + String(reference));
  }
  const name = reference.slice('#/$defs/'.length);
  const resolved = rootSchema.$defs && rootSchema.$defs[name];
  if (!resolved) fail('unknown schema reference ' + reference);
  return resolved;
}

function validateSchemaValue(value, schema, rootSchema, location) {
  if (schema.$ref) return validateSchemaValue(value, resolveSchemaReference(rootSchema, schema.$ref), rootSchema, location);
  if (Object.hasOwn(schema, 'const') && value !== schema.const) {
    fail(location + ' must equal the schema constant');
  }
  if (schema.enum && !schema.enum.includes(value)) fail(location + ' is outside the closed enum');
  if (schema.type === 'object') {
    if (!value || typeof value !== 'object' || Array.isArray(value)) fail(location + ' must be an object');
    const properties = schema.properties || {};
    for (const required of schema.required || []) {
      if (!Object.hasOwn(value, required)) fail(location + ' is missing required field ' + required);
    }
    if (schema.additionalProperties === false) {
      for (const key of Object.keys(value)) {
        if (!Object.hasOwn(properties, key)) fail(location + ' has unknown field ' + key);
      }
    }
    for (const [key, childSchema] of Object.entries(properties)) {
      if (Object.hasOwn(value, key)) validateSchemaValue(value[key], childSchema, rootSchema, location + '.' + key);
    }
  } else if (schema.type === 'array') {
    if (!Array.isArray(value)) fail(location + ' must be an array');
    if (schema.minItems !== undefined && value.length < schema.minItems) fail(location + ' has too few items');
    if (schema.maxItems !== undefined && value.length > schema.maxItems) fail(location + ' has too many items');
    if (schema.items) value.forEach((item, index) => validateSchemaValue(item, schema.items, rootSchema, location + '[' + index + ']'));
  } else if (schema.type === 'string') {
    if (typeof value !== 'string') fail(location + ' must be a string');
    if (schema.minLength !== undefined && value.length < schema.minLength) fail(location + ' is too short');
    if (schema.maxLength !== undefined && value.length > schema.maxLength) fail(location + ' is too long');
    if (schema.pattern && !(new RegExp(schema.pattern)).test(value)) fail(location + ' does not match its pattern');
  }
  return true;
}

function validRepositoryPath(value) {
  if (typeof value !== 'string' || !value || value.startsWith('/') || value.endsWith('/')) return false;
  if (/^[A-Za-z]:/.test(value)) return false;
  if (value.includes('\0') || value.includes('\\')) return false;
  const parts = value.split('/');
  return parts.every((part) => part && part !== '.' && part !== '..');
}

function assertSortedUnique(values, key, label) {
  let previous;
  for (const value of values) {
    const current = key(value);
    if (previous !== undefined && previous >= current) fail(label + ' is unsorted or duplicated');
    previous = current;
  }
  return values;
}

function validateWithSchema(schemaName, value) {
  assertJsonDomain(value);
  const schema = SCHEMAS[schemaName];
  if (!schema) fail('unknown schema ' + schemaName);
  validateSchemaValue(value, schema, schema, schemaName);
  return true;
}

function validateSchemaBundleIdentity(value) {
  validateWithSchema('schema-bundle-identity.schema.json', value);
  if (value.schema_id !== SCHEMA_IDS['schema-bundle-identity.schema.json']) fail('bundle schema ID mismatch');
  assertSortedUnique(value.schemas, (record) => record.path, 'bundle schema records');
  if (canonicalWriter(value.schemas) !== canonicalWriter(SCHEMA_RECORDS)) fail('bundle schema record mismatch');
  const expected = documentIdHex(value.schema_id, withoutOwnId(value, 'bundle_id'));
  if (value.bundle_id !== expected || value.bundle_id !== SCHEMA_BUNDLE_ID) fail('bundle document ID mismatch');
  return true;
}

function validateSchemaTrustAnchor(value) {
  validateWithSchema('schema-trust-anchor.schema.json', value);
  if (value.schema_id !== SCHEMA_IDS['schema-trust-anchor.schema.json']) fail('anchor schema ID mismatch');
  assertSortedUnique(value.compatibility, (record) => record.bundle_id, 'anchor compatibility');
  const expected = documentIdHex(value.schema_id, withoutOwnId(value, 'anchor_id'));
  if (value.anchor_id !== expected) fail('anchor document ID mismatch');
  return true;
}

function validateOwnershipManifest(value, authority) {
  validateWithSchema('ownership-manifest.schema.json', value);
  if (value.schema_id !== SCHEMA_IDS['ownership-manifest.schema.json']) fail('manifest schema ID mismatch');
  if (value.schema_bundle_id !== SCHEMA_BUNDLE_ID) fail('manifest bundle ID mismatch');
  assertSortedUnique(value.entries, (entry) => entry.path, 'manifest entries');
  for (const entry of value.entries) if (!validRepositoryPath(entry.path)) fail('invalid repository path ' + entry.path);
  if (authority === undefined) fail('independent manifest authority is required');
  assertJsonDomain(authority);
  if (authority === null || typeof authority !== 'object' || Array.isArray(authority)) fail('independent manifest authority is required');
  const authorityKeys = Object.keys(authority).sort();
  if (authorityKeys.length !== 2
    || authorityKeys[0] !== 'existingPaths'
    || authorityKeys[1] !== 'referencePaths') {
    fail('independent manifest authority must contain exactly existingPaths and referencePaths');
  }
  const expectedPaths = authority.referencePaths;
  const expectedExistingPaths = authority.existingPaths;
  if (!Array.isArray(expectedPaths) || !Array.isArray(expectedExistingPaths)) fail('independent manifest authority is required');
  assertSortedUnique(expectedPaths, (item) => item, 'authority reference paths');
  assertSortedUnique(expectedExistingPaths, (item) => item, 'authority existing paths');
  const expectedSet = new Set(expectedPaths);
  const existing = new Set(expectedExistingPaths);
  if (existing.size !== expectedExistingPaths.length
    || expectedSet.size !== expectedPaths.length
    || expectedExistingPaths.some((item) => !expectedSet.has(item))) {
    fail('independent manifest authority is inconsistent');
  }
  if (value.entries.length !== expectedPaths.length) fail('manifest path count mismatch');
  for (let index = 0; index < expectedPaths.length; index += 1) {
    const entry = value.entries[index];
    if (entry.path !== expectedPaths[index]) fail('manifest path set mismatch');
    const isExisting = existing.has(entry.path);
    if (entry.state !== (isExisting ? 'existing' : 'planned')) fail('manifest path state mismatch');
    if (entry.dependency !== (isExisting ? 'frozen' : 'lane-0-bootstrap')) fail('manifest dependency mismatch');
    if (entry.owner !== (isExisting ? 'frozen' : 'codex-worker-2')) fail('manifest owner mismatch');
    if (entry.reviewer !== 'claude-worker-2') fail('manifest reviewer mismatch');
    if (entry.transfer !== (isExisting ? 'frozen' : 'lane-0')) fail('manifest transfer mismatch');
  }
  const expected = documentIdHex(value.schema_id, withoutOwnId(value, 'manifest_id'));
  if (value.manifest_id !== expected) fail('manifest document ID mismatch');
  return true;
}

function validateOwnershipTransfer(value) {
  validateWithSchema('ownership-transfer.schema.json', value);
  if (value.schema_id !== SCHEMA_IDS['ownership-transfer.schema.json']) fail('transfer schema ID mismatch');
  if (value.schema_bundle_id !== SCHEMA_BUNDLE_ID) fail('transfer bundle ID mismatch');
  if (!validRepositoryPath(value.path)) fail('invalid transfer path');
  const expected = documentIdHex(value.schema_id, withoutOwnId(value, 'transfer_id'));
  if (value.transfer_id !== expected) fail('transfer document ID mismatch');
  return true;
}

function requireAcceptedTransfer(value) {
  validateOwnershipTransfer(value);
  if (value.status !== 'accepted') fail('ownership transfer is not accepted');
  return true;
}

function writeSchemaBundleIdentity(value) {
  validateSchemaBundleIdentity(value);
  return canonicalWriter(value);
}

function writeSchemaTrustAnchor(value) {
  validateSchemaTrustAnchor(value);
  return canonicalWriter(value);
}

function writeOwnershipManifest(value, authority) {
  validateOwnershipManifest(value, authority);
  return canonicalWriter(value);
}

function writeOwnershipTransfer(value) {
  validateOwnershipTransfer(value);
  return canonicalWriter(value);
}

function readSchemas() {
  const schemas = Object.create(null);
  const records = [];
  for (const name of fs.readdirSync(schemaRoot).sort()) {
    if (!name.endsWith('.schema.json')) continue;
    const bytes = fs.readFileSync(path.join(schemaRoot, name));
    const schema = parseCanonicalJson(bytes);
    const identity = schemaId(schema);
    if (schema.$id !== 'urn:daw:schema:' + identity) fail(name + ' has a stale top-level $id');
    schemas[name] = schema;
    records.push({
      canonical_sha256: sha256(bytes),
      path: name,
      schema_id: identity,
    });
  }
  if (records.length !== 4) fail('Lane 0 requires exactly four source schemas');
  return { records, schemas };
}

function emittedValidator(bundle, schemaRecords, schemas) {
  const schemaIds = Object.fromEntries(schemaRecords.map((record) => [record.path, record.schema_id]));
  const functions = [
    fail,
    scalarString,
    inspectJsonDomain,
    assertJsonDomain,
    encodeJson,
    canonicalWriter,
    sha256Hex,
    documentIdHex,
    withoutOwnId,
    resolveSchemaReference,
    validateSchemaValue,
    validRepositoryPath,
    assertSortedUnique,
    validateWithSchema,
    validateSchemaBundleIdentity,
    validateSchemaTrustAnchor,
    validateOwnershipManifest,
    validateOwnershipTransfer,
    requireAcceptedTransfer,
    writeSchemaBundleIdentity,
    writeSchemaTrustAnchor,
    writeOwnershipManifest,
    writeOwnershipTransfer,
  ];
  return [
    "import crypto from 'node:crypto';",
    "import { types as utilTypes } from 'node:util';",
    'export const SCHEMA_BUNDLE_ID = ' + JSON.stringify(bundle.bundle_id) + ';',
    'export const SCHEMA_IDS = Object.freeze(' + canonical(schemaIds) + ');',
    'const SCHEMA_RECORDS = Object.freeze(' + canonical(schemaRecords) + ');',
    'const SCHEMAS = Object.freeze(' + canonical(schemas) + ');',
    'const HEX64 = /^[0-9a-f]{64}$/;',
    functions.map((fn) => fn.toString().replace(/^function /, 'export function ')).join('\n\n'),
    'export const validateManifest = validateOwnershipManifest;',
    'export const validateTransfer = validateOwnershipTransfer;',
    '',
  ].join('\n');
}

function cppContract(bundle, schemaRecords, literals) {
  const ids = Object.fromEntries(schemaRecords.map((record) => [record.path, record.schema_id]));
  return [
    "#pragma once",
    "#include <algorithm>",
    "#include <cstddef>",
    "#include <string>",
    "#include <string_view>",
    "#include <vector>",
    "namespace daw::ae_p0_2 {",
    'inline constexpr std::string_view kSchemaBundleId = "' + bundle.bundle_id + '";',
    'inline constexpr std::string_view kSchemaBundleSchemaId = "' + ids['schema-bundle-identity.schema.json'] + '";',
    'inline constexpr std::string_view kSchemaTrustAnchorSchemaId = "' + ids['schema-trust-anchor.schema.json'] + '";',
    'inline constexpr std::string_view kOwnershipManifestSchemaId = "' + ids['ownership-manifest.schema.json'] + '";',
    'inline constexpr std::string_view kOwnershipTransferSchemaId = "' + ids['ownership-transfer.schema.json'] + '";',
    'inline constexpr std::string_view kEmptySchemaPreimageHex = "' + literals.empty_schema_preimage_hex + '";',
    'inline constexpr std::string_view kEmptySchemaId = "' + literals.empty_schema_id + '";',
    'inline constexpr std::string_view kEmptyDocumentPreimageHex = "' + literals.empty_document_preimage_hex + '";',
    'inline constexpr std::string_view kEmptyDocumentId = "' + literals.empty_document_id + '";',
    "struct SchemaRecord { std::string canonical_sha256; std::string path; std::string schema_id; };",
    "struct SchemaBundleIdentity { std::string bundle_id; std::string schema_id; std::vector<SchemaRecord> schemas; std::string version; };",
    "struct CompatibilityRecord { std::string bundle_id; std::string validator_sha256; };",
    "struct SchemaTrustAnchor { std::string anchor_id; std::string anchor_version; std::vector<CompatibilityRecord> compatibility; std::string current_bundle_id; std::string schema_id; std::string validator_sha256; };",
    "struct OwnershipEntry { std::string dependency; std::string owner; std::string path; std::string reviewer; std::string state; std::string transfer; };",
    "struct OwnershipManifest { std::string baseline; std::vector<OwnershipEntry> entries; std::string manifest_id; std::string schema_bundle_id; std::string schema_id; };",
    "struct OwnershipTransfer { std::string from; std::string path; std::string reviewer; std::string schema_bundle_id; std::string schema_id; std::string status; std::string to; std::string transfer_id; };",
    "inline bool lowerHex64(std::string_view value) { return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }); }",
    "inline bool validPath(std::string_view value) { const bool drive = value.size() >= 2 && ((value[0] >= 'A' && value[0] <= 'Z') || (value[0] >= 'a' && value[0] <= 'z')) && value[1] == ':'; if (value.empty() || value.front() == '/' || value.back() == '/' || drive || value.find('\\\\') != std::string_view::npos || value.find('\\0') != std::string_view::npos) return false; std::size_t start = 0; while (start <= value.size()) { const auto end = value.find('/', start); const auto part = value.substr(start, end == std::string_view::npos ? value.size() - start : end - start); if (part.empty() || part == \".\" || part == \"..\") return false; if (end == std::string_view::npos) break; start = end + 1; } return true; }",
    "inline std::string quote(std::string_view value) { static constexpr char hex[] = \"0123456789abcdef\"; std::string out = \"\\\"\"; for (unsigned char c : value) { if (c == '\"' || c == '\\\\') { out.push_back('\\\\'); out.push_back(static_cast<char>(c)); } else if (c == '\\b') { out += \"\\\\b\"; } else if (c == '\\t') { out += \"\\\\t\"; } else if (c == '\\n') { out += \"\\\\n\"; } else if (c == '\\f') { out += \"\\\\f\"; } else if (c == '\\r') { out += \"\\\\r\"; } else if (c < 0x20) { out += \"\\\\u00\"; out.push_back(hex[c >> 4]); out.push_back(hex[c & 15]); } else { out.push_back(static_cast<char>(c)); } } out.push_back('\"'); return out; }",
    "inline bool validSchemaPath(std::string_view value) { return value == \"ownership-manifest.schema.json\" || value == \"ownership-transfer.schema.json\" || value == \"schema-bundle-identity.schema.json\" || value == \"schema-trust-anchor.schema.json\"; }",
    "inline std::string_view expectedSchemaId(std::string_view path) { if (path == \"ownership-manifest.schema.json\") return kOwnershipManifestSchemaId; if (path == \"ownership-transfer.schema.json\") return kOwnershipTransferSchemaId; if (path == \"schema-bundle-identity.schema.json\") return kSchemaBundleSchemaId; if (path == \"schema-trust-anchor.schema.json\") return kSchemaTrustAnchorSchemaId; return {}; }",
    "inline bool validOwner(std::string_view value) { return value == \"backend\" || value == \"codex-worker-1\" || value == \"codex-worker-2\" || value == \"frontend\" || value == \"frozen\"; }",
    "inline bool validReviewer(std::string_view value) { return value == \"claude-worker-1\" || value == \"claude-worker-2\"; }",
    "inline bool validate(const SchemaRecord& value) { return lowerHex64(value.canonical_sha256) && validSchemaPath(value.path) && value.schema_id == expectedSchemaId(value.path); }",
    "inline bool validate(const CompatibilityRecord& value) { return lowerHex64(value.bundle_id) && lowerHex64(value.validator_sha256); }",
    "inline bool validate(const SchemaBundleIdentity& value) { if (value.bundle_id != kSchemaBundleId || value.schema_id != kSchemaBundleSchemaId || value.version != \"1\" || value.schemas.size() != 4) return false; std::string previous; for (const auto& item : value.schemas) { if (!validate(item) || (!previous.empty() && previous >= item.path)) return false; previous = item.path; } return true; }",
    "inline bool validate(const SchemaTrustAnchor& value) { if (!lowerHex64(value.anchor_id) || value.anchor_version != \"1\" || value.current_bundle_id != kSchemaBundleId || value.schema_id != kSchemaTrustAnchorSchemaId || !lowerHex64(value.validator_sha256)) return false; std::string previous; for (const auto& item : value.compatibility) { if (!validate(item) || (!previous.empty() && previous >= item.bundle_id)) return false; previous = item.bundle_id; } return true; }",
    "inline bool validate(const OwnershipEntry& value) { return (value.dependency == \"frozen\" || value.dependency == \"lane-0-bootstrap\") && validOwner(value.owner) && validPath(value.path) && validReviewer(value.reviewer) && (value.state == \"existing\" || value.state == \"planned\") && (value.transfer == \"frozen\" || value.transfer == \"lane-0\" || value.transfer == \"transfer-required\"); }",
    'inline bool validate(const OwnershipManifest& value) { if (value.baseline != "' + BASELINE_SHA + '" || !lowerHex64(value.manifest_id) || value.schema_bundle_id != kSchemaBundleId || value.schema_id != kOwnershipManifestSchemaId || value.entries.empty()) return false; std::string previous; for (const auto& item : value.entries) { if (!validate(item) || (!previous.empty() && previous >= item.path)) return false; previous = item.path; } return true; }',
    "inline bool validate(const OwnershipTransfer& value) { return validOwner(value.from) && validPath(value.path) && validReviewer(value.reviewer) && value.schema_bundle_id == kSchemaBundleId && value.schema_id == kOwnershipTransferSchemaId && (value.status == \"accepted\" || value.status == \"proposed\" || value.status == \"rejected\") && validOwner(value.to) && lowerHex64(value.transfer_id); }",
    "inline std::string canonicalWriter(const SchemaRecord& value) { return \"{\\\"canonical_sha256\\\":\" + quote(value.canonical_sha256) + \",\\\"path\\\":\" + quote(value.path) + \",\\\"schema_id\\\":\" + quote(value.schema_id) + \"}\"; }",
    "inline std::string canonicalWriter(const CompatibilityRecord& value) { return \"{\\\"bundle_id\\\":\" + quote(value.bundle_id) + \",\\\"validator_sha256\\\":\" + quote(value.validator_sha256) + \"}\"; }",
    "inline std::string canonicalWriter(const SchemaBundleIdentity& value) { std::string schemas = \"[\"; for (std::size_t index = 0; index < value.schemas.size(); ++index) { if (index) schemas += \",\"; schemas += canonicalWriter(value.schemas[index]); } schemas += \"]\"; return \"{\\\"bundle_id\\\":\" + quote(value.bundle_id) + \",\\\"schema_id\\\":\" + quote(value.schema_id) + \",\\\"schemas\\\":\" + schemas + \",\\\"version\\\":\" + quote(value.version) + \"}\"; }",
    "inline std::string canonicalWriter(const SchemaTrustAnchor& value) { std::string compatibility = \"[\"; for (std::size_t index = 0; index < value.compatibility.size(); ++index) { if (index) compatibility += \",\"; compatibility += canonicalWriter(value.compatibility[index]); } compatibility += \"]\"; return \"{\\\"anchor_id\\\":\" + quote(value.anchor_id) + \",\\\"anchor_version\\\":\" + quote(value.anchor_version) + \",\\\"compatibility\\\":\" + compatibility + \",\\\"current_bundle_id\\\":\" + quote(value.current_bundle_id) + \",\\\"schema_id\\\":\" + quote(value.schema_id) + \",\\\"validator_sha256\\\":\" + quote(value.validator_sha256) + \"}\"; }",
    "inline std::string canonicalWriter(const OwnershipEntry& value) { return \"{\\\"dependency\\\":\" + quote(value.dependency) + \",\\\"owner\\\":\" + quote(value.owner) + \",\\\"path\\\":\" + quote(value.path) + \",\\\"reviewer\\\":\" + quote(value.reviewer) + \",\\\"state\\\":\" + quote(value.state) + \",\\\"transfer\\\":\" + quote(value.transfer) + \"}\"; }",
    "inline std::string canonicalWriter(const OwnershipManifest& value) { std::string entries = \"[\"; for (std::size_t index = 0; index < value.entries.size(); ++index) { if (index) entries += \",\"; entries += canonicalWriter(value.entries[index]); } entries += \"]\"; return \"{\\\"baseline\\\":\" + quote(value.baseline) + \",\\\"entries\\\":\" + entries + \",\\\"manifest_id\\\":\" + quote(value.manifest_id) + \",\\\"schema_bundle_id\\\":\" + quote(value.schema_bundle_id) + \",\\\"schema_id\\\":\" + quote(value.schema_id) + \"}\"; }",
    "inline std::string canonicalWriter(const OwnershipTransfer& value) { return \"{\\\"from\\\":\" + quote(value.from) + \",\\\"path\\\":\" + quote(value.path) + \",\\\"reviewer\\\":\" + quote(value.reviewer) + \",\\\"schema_bundle_id\\\":\" + quote(value.schema_bundle_id) + \",\\\"schema_id\\\":\" + quote(value.schema_id) + \",\\\"status\\\":\" + quote(value.status) + \",\\\"to\\\":\" + quote(value.to) + \",\\\"transfer_id\\\":\" + quote(value.transfer_id) + \"}\"; }",
    "}",
    "",
  ].join('\n');
}


function rustContract(bundle, schemaRecords, literals) {
  const ids = Object.fromEntries(schemaRecords.map((record) => [record.path, record.schema_id]));
  return [
    'pub const SCHEMA_BUNDLE_ID: &str = "' + bundle.bundle_id + '";',
    'pub const SCHEMA_BUNDLE_SCHEMA_ID: &str = "' + ids['schema-bundle-identity.schema.json'] + '";',
    'pub const SCHEMA_TRUST_ANCHOR_SCHEMA_ID: &str = "' + ids['schema-trust-anchor.schema.json'] + '";',
    'pub const OWNERSHIP_MANIFEST_SCHEMA_ID: &str = "' + ids['ownership-manifest.schema.json'] + '";',
    'pub const OWNERSHIP_TRANSFER_SCHEMA_ID: &str = "' + ids['ownership-transfer.schema.json'] + '";',
    'pub const EMPTY_SCHEMA_PREIMAGE_HEX: &str = "' + literals.empty_schema_preimage_hex + '";',
    'pub const EMPTY_SCHEMA_ID: &str = "' + literals.empty_schema_id + '";',
    'pub const EMPTY_DOCUMENT_PREIMAGE_HEX: &str = "' + literals.empty_document_preimage_hex + '";',
    'pub const EMPTY_DOCUMENT_ID: &str = "' + literals.empty_document_id + '";',
    "#[derive(Clone, Debug, Eq, PartialEq)] pub struct SchemaRecord { pub canonical_sha256: String, pub path: String, pub schema_id: String }",
    "#[derive(Clone, Debug, Eq, PartialEq)] pub struct SchemaBundleIdentity { pub bundle_id: String, pub schema_id: String, pub schemas: Vec<SchemaRecord>, pub version: String }",
    "#[derive(Clone, Debug, Eq, PartialEq)] pub struct CompatibilityRecord { pub bundle_id: String, pub validator_sha256: String }",
    "#[derive(Clone, Debug, Eq, PartialEq)] pub struct SchemaTrustAnchor { pub anchor_id: String, pub anchor_version: String, pub compatibility: Vec<CompatibilityRecord>, pub current_bundle_id: String, pub schema_id: String, pub validator_sha256: String }",
    "#[derive(Clone, Debug, Eq, PartialEq)] pub struct OwnershipEntry { pub dependency: String, pub owner: String, pub path: String, pub reviewer: String, pub state: String, pub transfer: String }",
    "#[derive(Clone, Debug, Eq, PartialEq)] pub struct OwnershipManifest { pub baseline: String, pub entries: Vec<OwnershipEntry>, pub manifest_id: String, pub schema_bundle_id: String, pub schema_id: String }",
    "#[derive(Clone, Debug, Eq, PartialEq)] pub struct OwnershipTransfer { pub from: String, pub path: String, pub reviewer: String, pub schema_bundle_id: String, pub schema_id: String, pub status: String, pub to: String, pub transfer_id: String }",
    "pub fn lower_hex_64(value: &str) -> bool { value.len() == 64 && value.bytes().all(|c| c.is_ascii_digit() || (b'a'..=b'f').contains(&c)) }",
    "pub fn valid_path(value: &str) -> bool { let bytes = value.as_bytes(); let drive = bytes.len() >= 2 && bytes[0].is_ascii_alphabetic() && bytes[1] == b':'; !value.is_empty() && !value.starts_with('/') && !value.ends_with('/') && !drive && !value.contains('\\\\') && !value.contains('\\0') && value.split('/').all(|part| !part.is_empty() && part != \".\" && part != \"..\") }",
    "pub fn quote(value: &str) -> String { let mut out = String::from(\"\\\"\"); for c in value.chars() { match c { '\"' | '\\\\' => { out.push('\\\\'); out.push(c); }, '\\u{0008}' => out.push_str(\"\\\\b\"), '\\t' => out.push_str(\"\\\\t\"), '\\n' => out.push_str(\"\\\\n\"), '\\u{000c}' => out.push_str(\"\\\\f\"), '\\r' => out.push_str(\"\\\\r\"), c if (c as u32) < 0x20 => out.push_str(&format!(\"\\\\u{:04x}\", c as u32)), _ => out.push(c), } } out.push('\"'); out }",
    "pub fn expected_schema_id(path: &str) -> Option<&'static str> { match path { \"ownership-manifest.schema.json\" => Some(OWNERSHIP_MANIFEST_SCHEMA_ID), \"ownership-transfer.schema.json\" => Some(OWNERSHIP_TRANSFER_SCHEMA_ID), \"schema-bundle-identity.schema.json\" => Some(SCHEMA_BUNDLE_SCHEMA_ID), \"schema-trust-anchor.schema.json\" => Some(SCHEMA_TRUST_ANCHOR_SCHEMA_ID), _ => None } }",
    "pub fn valid_owner(value: &str) -> bool { matches!(value, \"backend\" | \"codex-worker-1\" | \"codex-worker-2\" | \"frontend\" | \"frozen\") }",
    "pub fn valid_reviewer(value: &str) -> bool { matches!(value, \"claude-worker-1\" | \"claude-worker-2\") }",
    "impl SchemaRecord { pub fn validate(&self) -> bool { lower_hex_64(&self.canonical_sha256) && expected_schema_id(&self.path) == Some(self.schema_id.as_str()) } pub fn canonical_writer(&self) -> String { let mut out = String::from(\"{\\\"canonical_sha256\\\":\"); out.push_str(&quote(&self.canonical_sha256)); out.push_str(\",\\\"path\\\":\"); out.push_str(&quote(&self.path)); out.push_str(\",\\\"schema_id\\\":\"); out.push_str(&quote(&self.schema_id)); out.push('}'); out } }",
    "impl CompatibilityRecord { pub fn validate(&self) -> bool { lower_hex_64(&self.bundle_id) && lower_hex_64(&self.validator_sha256) } pub fn canonical_writer(&self) -> String { let mut out = String::from(\"{\\\"bundle_id\\\":\"); out.push_str(&quote(&self.bundle_id)); out.push_str(\",\\\"validator_sha256\\\":\"); out.push_str(&quote(&self.validator_sha256)); out.push('}'); out } }",
    "impl SchemaBundleIdentity { pub fn validate(&self) -> bool { self.bundle_id == SCHEMA_BUNDLE_ID && self.schema_id == SCHEMA_BUNDLE_SCHEMA_ID && self.version == \"1\" && self.schemas.len() == 4 && self.schemas.iter().all(SchemaRecord::validate) && self.schemas.windows(2).all(|items| items[0].path < items[1].path) } pub fn canonical_writer(&self) -> String { let schemas = self.schemas.iter().map(SchemaRecord::canonical_writer).collect::<Vec<_>>().join(\",\"); let mut out = String::from(\"{\\\"bundle_id\\\":\"); out.push_str(&quote(&self.bundle_id)); out.push_str(\",\\\"schema_id\\\":\"); out.push_str(&quote(&self.schema_id)); out.push_str(\",\\\"schemas\\\":[\"); out.push_str(&schemas); out.push_str(\"],\\\"version\\\":\"); out.push_str(&quote(&self.version)); out.push('}'); out } }",
    "impl SchemaTrustAnchor { pub fn validate(&self) -> bool { self.anchor_version == \"1\" && lower_hex_64(&self.anchor_id) && self.current_bundle_id == SCHEMA_BUNDLE_ID && self.schema_id == SCHEMA_TRUST_ANCHOR_SCHEMA_ID && lower_hex_64(&self.validator_sha256) && self.compatibility.iter().all(CompatibilityRecord::validate) && self.compatibility.windows(2).all(|items| items[0].bundle_id < items[1].bundle_id) } pub fn canonical_writer(&self) -> String { let compatibility = self.compatibility.iter().map(CompatibilityRecord::canonical_writer).collect::<Vec<_>>().join(\",\"); let mut out = String::from(\"{\\\"anchor_id\\\":\"); out.push_str(&quote(&self.anchor_id)); out.push_str(\",\\\"anchor_version\\\":\"); out.push_str(&quote(&self.anchor_version)); out.push_str(\",\\\"compatibility\\\":[\"); out.push_str(&compatibility); out.push_str(\"],\\\"current_bundle_id\\\":\"); out.push_str(&quote(&self.current_bundle_id)); out.push_str(\",\\\"schema_id\\\":\"); out.push_str(&quote(&self.schema_id)); out.push_str(\",\\\"validator_sha256\\\":\"); out.push_str(&quote(&self.validator_sha256)); out.push('}'); out } }",
    "impl OwnershipEntry { pub fn validate(&self) -> bool { matches!(self.dependency.as_str(), \"frozen\" | \"lane-0-bootstrap\") && valid_owner(&self.owner) && valid_path(&self.path) && valid_reviewer(&self.reviewer) && matches!(self.state.as_str(), \"existing\" | \"planned\") && matches!(self.transfer.as_str(), \"frozen\" | \"lane-0\" | \"transfer-required\") } pub fn canonical_writer(&self) -> String { let mut out = String::from(\"{\\\"dependency\\\":\"); out.push_str(&quote(&self.dependency)); out.push_str(\",\\\"owner\\\":\"); out.push_str(&quote(&self.owner)); out.push_str(\",\\\"path\\\":\"); out.push_str(&quote(&self.path)); out.push_str(\",\\\"reviewer\\\":\"); out.push_str(&quote(&self.reviewer)); out.push_str(\",\\\"state\\\":\"); out.push_str(&quote(&self.state)); out.push_str(\",\\\"transfer\\\":\"); out.push_str(&quote(&self.transfer)); out.push('}'); out } }",
    'impl OwnershipManifest { pub fn validate(&self) -> bool { self.baseline == "' + BASELINE_SHA + '" && lower_hex_64(&self.manifest_id) && self.schema_bundle_id == SCHEMA_BUNDLE_ID && self.schema_id == OWNERSHIP_MANIFEST_SCHEMA_ID && !self.entries.is_empty() && self.entries.iter().all(OwnershipEntry::validate) && self.entries.windows(2).all(|items| items[0].path < items[1].path) } pub fn canonical_writer(&self) -> String { let entries = self.entries.iter().map(OwnershipEntry::canonical_writer).collect::<Vec<_>>().join(","); let mut out = String::from("{\\\"baseline\\\":"); out.push_str(&quote(&self.baseline)); out.push_str(",\\\"entries\\\":["); out.push_str(&entries); out.push_str("],\\\"manifest_id\\\":"); out.push_str(&quote(&self.manifest_id)); out.push_str(",\\\"schema_bundle_id\\\":"); out.push_str(&quote(&self.schema_bundle_id)); out.push_str(",\\\"schema_id\\\":"); out.push_str(&quote(&self.schema_id)); out.push(\'}\'); out } }',
    "impl OwnershipTransfer { pub fn validate(&self) -> bool { valid_owner(&self.from) && valid_path(&self.path) && valid_reviewer(&self.reviewer) && self.schema_bundle_id == SCHEMA_BUNDLE_ID && self.schema_id == OWNERSHIP_TRANSFER_SCHEMA_ID && matches!(self.status.as_str(), \"accepted\" | \"proposed\" | \"rejected\") && valid_owner(&self.to) && lower_hex_64(&self.transfer_id) } pub fn canonical_writer(&self) -> String { let mut out = String::from(\"{\\\"from\\\":\"); out.push_str(&quote(&self.from)); out.push_str(\",\\\"path\\\":\"); out.push_str(&quote(&self.path)); out.push_str(\",\\\"reviewer\\\":\"); out.push_str(&quote(&self.reviewer)); out.push_str(\",\\\"schema_bundle_id\\\":\"); out.push_str(&quote(&self.schema_bundle_id)); out.push_str(\",\\\"schema_id\\\":\"); out.push_str(&quote(&self.schema_id)); out.push_str(\",\\\"status\\\":\"); out.push_str(&quote(&self.status)); out.push_str(\",\\\"to\\\":\"); out.push_str(&quote(&self.to)); out.push_str(\",\\\"transfer_id\\\":\"); out.push_str(&quote(&self.transfer_id)); out.push('}'); out } }",
    "",
  ].join('\n');
}


function typeScriptContract(bundle, schemaRecords, literals) {
  const ids = Object.fromEntries(schemaRecords.map((record) => [record.path, record.schema_id]));
  return [
    'export const SCHEMA_BUNDLE_ID = ' + JSON.stringify(bundle.bundle_id) + ' as const;',
    'export const OWNERSHIP_MANIFEST_SCHEMA_ID = ' + JSON.stringify(ids['ownership-manifest.schema.json']) + ' as const;',
    'export const OWNERSHIP_TRANSFER_SCHEMA_ID = ' + JSON.stringify(ids['ownership-transfer.schema.json']) + ' as const;',
    'export const SCHEMA_BUNDLE_SCHEMA_ID = ' + JSON.stringify(ids['schema-bundle-identity.schema.json']) + ' as const;',
    'export const SCHEMA_TRUST_ANCHOR_SCHEMA_ID = ' + JSON.stringify(ids['schema-trust-anchor.schema.json']) + ' as const;',
    'export const EMPTY_SCHEMA_PREIMAGE_HEX = ' + JSON.stringify(literals.empty_schema_preimage_hex) + ' as const;',
    'export const EMPTY_SCHEMA_ID = ' + JSON.stringify(literals.empty_schema_id) + ' as const;',
    'export const EMPTY_DOCUMENT_PREIMAGE_HEX = ' + JSON.stringify(literals.empty_document_preimage_hex) + ' as const;',
    'export const EMPTY_DOCUMENT_ID = ' + JSON.stringify(literals.empty_document_id) + ' as const;',
    "export type SchemaPath=\"ownership-manifest.schema.json\"|\"ownership-transfer.schema.json\"|\"schema-bundle-identity.schema.json\"|\"schema-trust-anchor.schema.json\";",
    "export type Dependency=\"frozen\"|\"lane-0-bootstrap\";",
    "export type Owner=\"backend\"|\"codex-worker-1\"|\"codex-worker-2\"|\"frontend\"|\"frozen\";",
    "export type Reviewer=\"claude-worker-1\"|\"claude-worker-2\";",
    "export type OwnershipState=\"existing\"|\"planned\";",
    "export type TransferState=\"frozen\"|\"lane-0\"|\"transfer-required\";",
    "export type TransferStatus=\"accepted\"|\"proposed\"|\"rejected\";",
    "export interface SchemaRecord{canonical_sha256:string;path:SchemaPath;schema_id:string}",
    "export interface SchemaBundleIdentity{bundle_id:string;schema_id:string;schemas:SchemaRecord[];version:\"1\"}",
    "export interface CompatibilityRecord{bundle_id:string;validator_sha256:string}",
    "export interface SchemaTrustAnchor{anchor_id:string;anchor_version:\"1\";compatibility:CompatibilityRecord[];current_bundle_id:string;schema_id:string;validator_sha256:string}",
    "export interface OwnershipEntry{dependency:Dependency;owner:Owner;path:string;reviewer:Reviewer;state:OwnershipState;transfer:TransferState}",
    "export interface OwnershipManifest{baseline:string;entries:OwnershipEntry[];manifest_id:string;schema_bundle_id:string;schema_id:string}",
    "export interface OwnershipTransfer{from:Owner;path:string;reviewer:Reviewer;schema_bundle_id:string;schema_id:string;status:TransferStatus;to:Owner;transfer_id:string}",
    "export type ProxyDetector=(value:object)=>boolean;",
    "type JsonRecord=Record<string,unknown>;",
    "const rejectProxy=(value:object,isProxy:ProxyDetector):void=>{if(typeof isProxy!==\"function\")throw new Error(\"Proxy detector is required\");if(isProxy(value))throw new Error(\"Proxy values are not supported\");};",
    "const isRecord=(value:unknown,isProxy:ProxyDetector):value is JsonRecord=>{if(typeof value!==\"object\"||value===null)return false;rejectProxy(value,isProxy);if(Array.isArray(value))return false;const prototype=Object.getPrototypeOf(value);return prototype===Object.prototype||prototype===null;};",
    "const exact=(value:unknown,keys:readonly string[],isProxy:ProxyDetector):value is JsonRecord=>{if(!isRecord(value,isProxy))return false;const ownKeys=Reflect.ownKeys(value);if(ownKeys.length!==keys.length||ownKeys.some(key=>typeof key!==\"string\"))return false;const actual=(ownKeys as string[]).sort();if(actual.some((key,index)=>key!==keys[index]))return false;return ownKeys.every(key=>{const descriptor=Object.getOwnPropertyDescriptor(value,key);return descriptor!==undefined&&\"value\" in descriptor&&descriptor.enumerable;});};",
    "const ordinaryArray=(value:unknown,isProxy:ProxyDetector):value is unknown[]=>{if(typeof value!==\"object\"||value===null)return false;rejectProxy(value,isProxy);if(!Array.isArray(value)||Object.getPrototypeOf(value)!==Array.prototype)return false;const lengthDescriptor=Object.getOwnPropertyDescriptor(value,\"length\");if(!lengthDescriptor||!(\"value\" in lengthDescriptor)||typeof lengthDescriptor.value!==\"number\"||!Number.isSafeInteger(lengthDescriptor.value)||lengthDescriptor.value<0||lengthDescriptor.enumerable)return false;const length=lengthDescriptor.value;const ownKeys=Reflect.ownKeys(value);if(ownKeys.length!==length+1)return false;for(let index=0;index<length;index+=1){const descriptor=Object.getOwnPropertyDescriptor(value,String(index));if(!descriptor||!(\"value\" in descriptor)||!descriptor.enumerable)return false;}return true;};",
    "const hex64=(value:unknown):value is string=>typeof value===\"string\"&&value.length===64&&/^[0-9a-f]{64}$/.test(value);",
    "const validPath=(value:unknown):value is string=>typeof value===\"string\"&&value.length>0&&!value.startsWith(\"/\")&&!value.endsWith(\"/\")&&!/^[A-Za-z]:/.test(value)&&!value.includes(\"\\\\\")&&!value.includes(\"\\0\")&&value.split(\"/\").every(part=>part!==\"\"&&part!==\".\"&&part!==\"..\");",
    "const sortedUnique=<T>(values:T[],key:(value:T)=>string):boolean=>values.every((value,index)=>index===0||key(values[index-1])<key(value));",
    "const oneOf=<T extends string>(value:unknown,choices:readonly T[]):value is T=>typeof value===\"string\"&&(choices as readonly string[]).includes(value);",
    "const schemaIds={\"ownership-manifest.schema.json\":OWNERSHIP_MANIFEST_SCHEMA_ID,\"ownership-transfer.schema.json\":OWNERSHIP_TRANSFER_SCHEMA_ID,\"schema-bundle-identity.schema.json\":SCHEMA_BUNDLE_SCHEMA_ID,\"schema-trust-anchor.schema.json\":SCHEMA_TRUST_ANCHOR_SCHEMA_ID} as const;",
    "const schemaIdFor=(value:unknown):string|undefined=>typeof value===\"string\"?(schemaIds as Record<string,string>)[value]:undefined;",
    "export function validateSchemaRecord(value:unknown,isProxy:ProxyDetector):value is SchemaRecord{return exact(value,[\"canonical_sha256\",\"path\",\"schema_id\"],isProxy)&&hex64(value.canonical_sha256)&&schemaIdFor(value.path)!==undefined&&value.schema_id===schemaIdFor(value.path);}",
    "export function validateCompatibilityRecord(value:unknown,isProxy:ProxyDetector):value is CompatibilityRecord{return exact(value,[\"bundle_id\",\"validator_sha256\"],isProxy)&&hex64(value.bundle_id)&&hex64(value.validator_sha256);}",
    "export function validateSchemaBundleIdentity(value:unknown,isProxy:ProxyDetector):value is SchemaBundleIdentity{return exact(value,[\"bundle_id\",\"schema_id\",\"schemas\",\"version\"],isProxy)&&value.bundle_id===SCHEMA_BUNDLE_ID&&value.schema_id===SCHEMA_BUNDLE_SCHEMA_ID&&value.version===\"1\"&&ordinaryArray(value.schemas,isProxy)&&value.schemas.length===4&&value.schemas.every(record=>validateSchemaRecord(record,isProxy))&&sortedUnique(value.schemas,record=>record.path);}",
    "export function validateSchemaTrustAnchor(value:unknown,isProxy:ProxyDetector):value is SchemaTrustAnchor{return exact(value,[\"anchor_id\",\"anchor_version\",\"compatibility\",\"current_bundle_id\",\"schema_id\",\"validator_sha256\"],isProxy)&&hex64(value.anchor_id)&&value.anchor_version===\"1\"&&ordinaryArray(value.compatibility,isProxy)&&value.compatibility.every(record=>validateCompatibilityRecord(record,isProxy))&&sortedUnique(value.compatibility,record=>record.bundle_id)&&value.current_bundle_id===SCHEMA_BUNDLE_ID&&value.schema_id===SCHEMA_TRUST_ANCHOR_SCHEMA_ID&&hex64(value.validator_sha256);}",
    "export function validateOwnershipEntry(value:unknown,isProxy:ProxyDetector):value is OwnershipEntry{return exact(value,[\"dependency\",\"owner\",\"path\",\"reviewer\",\"state\",\"transfer\"],isProxy)&&oneOf(value.dependency,[\"frozen\",\"lane-0-bootstrap\"] as const)&&oneOf(value.owner,[\"backend\",\"codex-worker-1\",\"codex-worker-2\",\"frontend\",\"frozen\"] as const)&&validPath(value.path)&&oneOf(value.reviewer,[\"claude-worker-1\",\"claude-worker-2\"] as const)&&oneOf(value.state,[\"existing\",\"planned\"] as const)&&oneOf(value.transfer,[\"frozen\",\"lane-0\",\"transfer-required\"] as const);}",
    'export function validateOwnershipManifest(value:unknown,isProxy:ProxyDetector):value is OwnershipManifest{return exact(value,["baseline","entries","manifest_id","schema_bundle_id","schema_id"],isProxy)&&value.baseline===' + JSON.stringify(BASELINE_SHA) + '&&ordinaryArray(value.entries,isProxy)&&value.entries.length>0&&value.entries.every(entry=>validateOwnershipEntry(entry,isProxy))&&sortedUnique(value.entries,entry=>entry.path)&&hex64(value.manifest_id)&&value.schema_bundle_id===SCHEMA_BUNDLE_ID&&value.schema_id===OWNERSHIP_MANIFEST_SCHEMA_ID;}',
    "export function validateOwnershipTransfer(value:unknown,isProxy:ProxyDetector):value is OwnershipTransfer{return exact(value,[\"from\",\"path\",\"reviewer\",\"schema_bundle_id\",\"schema_id\",\"status\",\"to\",\"transfer_id\"],isProxy)&&oneOf(value.from,[\"backend\",\"codex-worker-1\",\"codex-worker-2\",\"frontend\",\"frozen\"] as const)&&validPath(value.path)&&oneOf(value.reviewer,[\"claude-worker-1\",\"claude-worker-2\"] as const)&&value.schema_bundle_id===SCHEMA_BUNDLE_ID&&value.schema_id===OWNERSHIP_TRANSFER_SCHEMA_ID&&oneOf(value.status,[\"accepted\",\"proposed\",\"rejected\"] as const)&&oneOf(value.to,[\"backend\",\"codex-worker-1\",\"codex-worker-2\",\"frontend\",\"frozen\"] as const)&&hex64(value.transfer_id);}",
    "const scalarString=(value:string):string=>{for(let index=0;index<value.length;index+=1){const unit=value.charCodeAt(index);if(unit>=0xd800&&unit<=0xdbff){const low=value.charCodeAt(index+1);if(!(low>=0xdc00&&low<=0xdfff))throw new Error(\"lone high surrogate\");index+=1;}else if(unit>=0xdc00&&unit<=0xdfff){throw new Error(\"lone low surrogate\");}}return value;};",
    "function canonicalValue(value:unknown,isProxy:ProxyDetector,active:Set<object>):string{if(value===null)return\"null\";if(typeof value===\"boolean\")return value?\"true\":\"false\";if(typeof value===\"string\")return JSON.stringify(scalarString(value));if(typeof value===\"number\"){if(!Number.isFinite(value))throw new Error(\"non-finite number\");return Object.is(value,-0)?\"0\":JSON.stringify(value);}if(typeof value!==\"object\")throw new Error(\"non-JSON value\");rejectProxy(value,isProxy);if(active.has(value))throw new Error(\"cyclic value\");active.add(value);try{if(Array.isArray(value)){if(Object.getPrototypeOf(value)!==Array.prototype)throw new Error(\"non-ordinary array\");const lengthDescriptor=Object.getOwnPropertyDescriptor(value,\"length\");if(!lengthDescriptor||!(\"value\" in lengthDescriptor)||typeof lengthDescriptor.value!==\"number\"||!Number.isSafeInteger(lengthDescriptor.value)||lengthDescriptor.value<0||lengthDescriptor.enumerable)throw new Error(\"invalid array length\");const length=lengthDescriptor.value;const ownKeys=Reflect.ownKeys(value);if(ownKeys.length!==length+1)throw new Error(\"sparse array or extra array property\");const items:string[]=[];for(let index=0;index<length;index+=1){const descriptor=Object.getOwnPropertyDescriptor(value,String(index));if(!descriptor||!(\"value\" in descriptor)||!descriptor.enumerable)throw new Error(\"sparse array or array accessor\");items.push(canonicalValue(descriptor.value,isProxy,active));}return\"[\"+items.join(\",\")+\"]\";}const prototype=Object.getPrototypeOf(value);if(prototype!==Object.prototype&&prototype!==null)throw new Error(\"non-plain object\");const ownKeys=Reflect.ownKeys(value);if(ownKeys.some(key=>typeof key!==\"string\"))throw new Error(\"symbol property\");const keys=(ownKeys as string[]).sort();const members:string[]=[];for(const key of keys){scalarString(key);const descriptor=Object.getOwnPropertyDescriptor(value,key);if(!descriptor||!(\"value\" in descriptor)||!descriptor.enumerable)throw new Error(\"accessor or hidden property\");members.push(JSON.stringify(key)+\":\"+canonicalValue(descriptor.value,isProxy,active));}return\"{\"+members.join(\",\")+\"}\";}finally{active.delete(value);}}",
    "export const canonicalWriter=(value:unknown,isProxy:ProxyDetector):string=>canonicalValue(value,isProxy,new Set<object>());",
    "export const writeSchemaBundleIdentity=(value:SchemaBundleIdentity,isProxy:ProxyDetector):string=>{if(!validateSchemaBundleIdentity(value,isProxy))throw new Error(\"invalid SchemaBundleIdentity\");return canonicalWriter(value,isProxy);};",
    "export const writeSchemaTrustAnchor=(value:SchemaTrustAnchor,isProxy:ProxyDetector):string=>{if(!validateSchemaTrustAnchor(value,isProxy))throw new Error(\"invalid SchemaTrustAnchor\");return canonicalWriter(value,isProxy);};",
    "export const writeOwnershipManifest=(value:OwnershipManifest,isProxy:ProxyDetector):string=>{if(!validateOwnershipManifest(value,isProxy))throw new Error(\"invalid OwnershipManifest\");return canonicalWriter(value,isProxy);};",
    "export const writeOwnershipTransfer=(value:OwnershipTransfer,isProxy:ProxyDetector):string=>{if(!validateOwnershipTransfer(value,isProxy))throw new Error(\"invalid OwnershipTransfer\");return canonicalWriter(value,isProxy);};",
    "",
  ].join('\n');
}


function write(relative, bytes) {
  const destination = path.join(outputRoot, relative);
  fs.mkdirSync(path.dirname(destination), { recursive: true });
  fs.writeFileSync(destination, bytes);
}

const { records: schemaRecords, schemas } = readSchemas();
const schemaIds = Object.fromEntries(schemaRecords.map((record) => [record.path, record.schema_id]));
const bundleBody = {
  schema_id: schemaIds['schema-bundle-identity.schema.json'],
  schemas: schemaRecords,
  version: '1',
};
const bundle = {
  bundle_id: documentId(bundleBody.schema_id, bundleBody),
  ...bundleBody,
};

const emptySchemaPreimage = Buffer.from('daw-schema-v1\0{}', 'ascii');
const emptyDocumentPreimage = documentPreimage('0'.repeat(64), {});
const literals = {
  empty_document_id: sha256(emptyDocumentPreimage),
  empty_document_preimage_hex: emptyDocumentPreimage.toString('hex'),
  empty_schema_id: sha256(emptySchemaPreimage),
  empty_schema_preimage_hex: emptySchemaPreimage.toString('hex'),
};
const vectors = {
  bundle,
  literals,
  schemas: schemaRecords.map((record) => ({
    ...record,
    schema_preimage_hex: schemaPreimage(schemas[record.path]).toString('hex'),
  })),
};

const base = baselinePaths();
const amendedExisting = existingPaths(base);
const existingSet = new Set(amendedExisting);
const pinnedReference = referencePaths(amendedExisting);
const entries = pinnedReference.map((entryPath) => ({
  dependency: existingSet.has(entryPath) ? 'frozen' : 'lane-0-bootstrap',
  owner: existingSet.has(entryPath) ? 'frozen' : 'codex-worker-2',
  path: entryPath,
  reviewer: 'claude-worker-2',
  state: existingSet.has(entryPath) ? 'existing' : 'planned',
  transfer: existingSet.has(entryPath) ? 'frozen' : 'lane-0',
}));
if (base.length !== 730
  || amendedBaseDeltaPaths.length !== 1
  || amendedExisting.length !== 731
  || plannedPaths.length !== 25
  || entries.length !== 756) {
  fail('pinned ownership reference set changed unexpectedly');
}
const manifestBody = {
  baseline: BASELINE_SHA,
  entries,
  schema_bundle_id: bundle.bundle_id,
  schema_id: schemaIds['ownership-manifest.schema.json'],
};
const manifest = {
  ...manifestBody,
  manifest_id: documentId(manifestBody.schema_id, manifestBody),
};

write('tools/architecture/ae_p0_2/generated/schema-bundle-identity.json', canonical(bundle));
write('tools/architecture/ae_p0_2/generated/validator.mjs', emittedValidator(bundle, schemaRecords, schemas));
write('tools/architecture/ae_p0_2/generated/contracts.hpp', cppContract(bundle, schemaRecords, literals));
write('tools/architecture/ae_p0_2/generated/contracts.rs', rustContract(bundle, schemaRecords, literals));
write('tools/architecture/ae_p0_2/generated/contracts.ts', typeScriptContract(bundle, schemaRecords, literals));
write('tools/architecture/ae_p0_2/testdata/golden-vectors.json', canonical(vectors));
write('docs/architecture/tasks/AE-P0.2-ownership.json', canonical(manifest));
