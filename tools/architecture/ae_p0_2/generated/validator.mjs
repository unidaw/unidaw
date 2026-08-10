import crypto from 'node:crypto';
import { types as utilTypes } from 'node:util';
export const SCHEMA_BUNDLE_ID = "27eb0a899abce5a888e2bdab5932e5cd9541e43e6c2fac3ca42df84ba980a080";
export const SCHEMA_IDS = Object.freeze({"ownership-manifest.schema.json":"440fd1b7b03314e42d89e003a6ed1c5070a1fc8c7a2ce285f59c56b4b07bda77","ownership-transfer.schema.json":"92633e63eec51c7afed83f5c7477d137b27d26c1922eb0eed35ed2ef22c3f541","schema-bundle-identity.schema.json":"e20decb688c5eeb444900c39d82275e2b98f7cd08e0eb98ce1114ec5bfff0437","schema-trust-anchor.schema.json":"dc24b657d17c28a043bb29306582f2730a939b5eb81d5e3fda8d8bd1108b6630"});
const SCHEMA_RECORDS = Object.freeze([{"canonical_sha256":"351646238d3d406991944bd46acf3a7258519b50d8dc460973ffd427dcf4fba0","path":"ownership-manifest.schema.json","schema_id":"440fd1b7b03314e42d89e003a6ed1c5070a1fc8c7a2ce285f59c56b4b07bda77"},{"canonical_sha256":"deea806f42b946f693d8fe28730c68eea3880a677cb000c1c88f315abf08e924","path":"ownership-transfer.schema.json","schema_id":"92633e63eec51c7afed83f5c7477d137b27d26c1922eb0eed35ed2ef22c3f541"},{"canonical_sha256":"79de93d6c90f97b5b708679ecf0046bd78e95e3539f2cf908f4a4d145557a3a4","path":"schema-bundle-identity.schema.json","schema_id":"e20decb688c5eeb444900c39d82275e2b98f7cd08e0eb98ce1114ec5bfff0437"},{"canonical_sha256":"6f504483925a4eae24a6a6e8cb097f6a05aba06fee5a66dbbafbe7089ae2420a","path":"schema-trust-anchor.schema.json","schema_id":"dc24b657d17c28a043bb29306582f2730a939b5eb81d5e3fda8d8bd1108b6630"}]);
const SCHEMAS = Object.freeze({"ownership-manifest.schema.json":{"$defs":{"entry":{"additionalProperties":false,"properties":{"dependency":{"enum":["frozen","lane-0-bootstrap"]},"owner":{"enum":["backend","codex-worker-1","codex-worker-2","frontend","frozen"]},"path":{"minLength":1,"pattern":"^[^/\\u0000\\\\](?:[^\\u0000\\\\]*[^/\\u0000\\\\])?$","type":"string"},"reviewer":{"enum":["claude-worker-1","claude-worker-2"]},"state":{"enum":["existing","planned"]},"transfer":{"enum":["frozen","lane-0","transfer-required"]}},"required":["dependency","owner","path","reviewer","state","transfer"],"type":"object"}},"$id":"urn:daw:schema:440fd1b7b03314e42d89e003a6ed1c5070a1fc8c7a2ce285f59c56b4b07bda77","$schema":"https://json-schema.org/draft/2020-12/schema","additionalProperties":false,"properties":{"baseline":{"const":"c33da66fe1a66f20eee931335b18465cfddfdb0e","maxLength":40,"minLength":40,"pattern":"^[0-9a-f]{40}$","type":"string"},"entries":{"items":{"$ref":"#/$defs/entry"},"minItems":1,"type":"array"},"manifest_id":{"maxLength":64,"minLength":64,"pattern":"^[0-9a-f]{64}$","type":"string"},"schema_bundle_id":{"maxLength":64,"minLength":64,"pattern":"^[0-9a-f]{64}$","type":"string"},"schema_id":{"maxLength":64,"minLength":64,"pattern":"^[0-9a-f]{64}$","type":"string"}},"required":["baseline","entries","manifest_id","schema_bundle_id","schema_id"],"type":"object"},"ownership-transfer.schema.json":{"$id":"urn:daw:schema:92633e63eec51c7afed83f5c7477d137b27d26c1922eb0eed35ed2ef22c3f541","$schema":"https://json-schema.org/draft/2020-12/schema","additionalProperties":false,"properties":{"from":{"enum":["backend","codex-worker-1","codex-worker-2","frontend","frozen"]},"path":{"minLength":1,"pattern":"^[^/\\u0000\\\\](?:[^\\u0000\\\\]*[^/\\u0000\\\\])?$","type":"string"},"reviewer":{"enum":["claude-worker-1","claude-worker-2"]},"schema_bundle_id":{"maxLength":64,"minLength":64,"pattern":"^[0-9a-f]{64}$","type":"string"},"schema_id":{"maxLength":64,"minLength":64,"pattern":"^[0-9a-f]{64}$","type":"string"},"status":{"enum":["accepted","proposed","rejected"]},"to":{"enum":["backend","codex-worker-1","codex-worker-2","frontend","frozen"]},"transfer_id":{"maxLength":64,"minLength":64,"pattern":"^[0-9a-f]{64}$","type":"string"}},"required":["from","path","reviewer","schema_bundle_id","schema_id","status","to","transfer_id"],"type":"object"},"schema-bundle-identity.schema.json":{"$defs":{"schema_record":{"additionalProperties":false,"properties":{"canonical_sha256":{"maxLength":64,"minLength":64,"pattern":"^[0-9a-f]{64}$","type":"string"},"path":{"enum":["ownership-manifest.schema.json","ownership-transfer.schema.json","schema-bundle-identity.schema.json","schema-trust-anchor.schema.json"]},"schema_id":{"maxLength":64,"minLength":64,"pattern":"^[0-9a-f]{64}$","type":"string"}},"required":["canonical_sha256","path","schema_id"],"type":"object"}},"$id":"urn:daw:schema:e20decb688c5eeb444900c39d82275e2b98f7cd08e0eb98ce1114ec5bfff0437","$schema":"https://json-schema.org/draft/2020-12/schema","additionalProperties":false,"properties":{"bundle_id":{"maxLength":64,"minLength":64,"pattern":"^[0-9a-f]{64}$","type":"string"},"schema_id":{"maxLength":64,"minLength":64,"pattern":"^[0-9a-f]{64}$","type":"string"},"schemas":{"items":{"$ref":"#/$defs/schema_record"},"maxItems":4,"minItems":4,"type":"array"},"version":{"const":"1","pattern":"^(0|[1-9][0-9]*)$","type":"string"}},"required":["bundle_id","schema_id","schemas","version"],"type":"object"},"schema-trust-anchor.schema.json":{"$defs":{"compatibility_record":{"additionalProperties":false,"properties":{"bundle_id":{"maxLength":64,"minLength":64,"pattern":"^[0-9a-f]{64}$","type":"string"},"validator_sha256":{"maxLength":64,"minLength":64,"pattern":"^[0-9a-f]{64}$","type":"string"}},"required":["bundle_id","validator_sha256"],"type":"object"}},"$id":"urn:daw:schema:dc24b657d17c28a043bb29306582f2730a939b5eb81d5e3fda8d8bd1108b6630","$schema":"https://json-schema.org/draft/2020-12/schema","additionalProperties":false,"properties":{"anchor_id":{"maxLength":64,"minLength":64,"pattern":"^[0-9a-f]{64}$","type":"string"},"anchor_version":{"const":"1","pattern":"^(0|[1-9][0-9]*)$","type":"string"},"compatibility":{"items":{"$ref":"#/$defs/compatibility_record"},"type":"array"},"current_bundle_id":{"maxLength":64,"minLength":64,"pattern":"^[0-9a-f]{64}$","type":"string"},"schema_id":{"maxLength":64,"minLength":64,"pattern":"^[0-9a-f]{64}$","type":"string"},"validator_sha256":{"maxLength":64,"minLength":64,"pattern":"^[0-9a-f]{64}$","type":"string"}},"required":["anchor_id","anchor_version","compatibility","current_bundle_id","schema_id","validator_sha256"],"type":"object"}});
const HEX64 = /^[0-9a-f]{64}$/;
export function fail(message) {
  throw new Error(message);
}

export function scalarString(value, label) {
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

export function inspectJsonDomain(value, active) {
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

export function assertJsonDomain(value) {
  inspectJsonDomain(value, new Set());
  return value;
}

export function encodeJson(value, active) {
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

export function canonicalWriter(value) {
  assertJsonDomain(value);
  return encodeJson(value, new Set());
}

export function sha256Hex(bytes) {
  return crypto.createHash('sha256').update(bytes).digest('hex');
}

export function documentIdHex(schemaIdHex, body) {
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

export function withoutOwnId(value, idField) {
  const body = Object.create(null);
  for (const key of Object.keys(value)) {
    if (key !== idField) body[key] = value[key];
  }
  return body;
}

export function resolveSchemaReference(rootSchema, reference) {
  if (typeof reference !== 'string' || !reference.startsWith('#/$defs/')) {
    fail('unsupported schema reference ' + String(reference));
  }
  const name = reference.slice('#/$defs/'.length);
  const resolved = rootSchema.$defs && rootSchema.$defs[name];
  if (!resolved) fail('unknown schema reference ' + reference);
  return resolved;
}

export function validateSchemaValue(value, schema, rootSchema, location) {
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

export function validRepositoryPath(value) {
  if (typeof value !== 'string' || !value || value.startsWith('/') || value.endsWith('/')) return false;
  if (/^[A-Za-z]:/.test(value)) return false;
  if (value.includes('\0') || value.includes('\\')) return false;
  const parts = value.split('/');
  return parts.every((part) => part && part !== '.' && part !== '..');
}

export function assertSortedUnique(values, key, label) {
  let previous;
  for (const value of values) {
    const current = key(value);
    if (previous !== undefined && previous >= current) fail(label + ' is unsorted or duplicated');
    previous = current;
  }
  return values;
}

export function validateWithSchema(schemaName, value) {
  assertJsonDomain(value);
  const schema = SCHEMAS[schemaName];
  if (!schema) fail('unknown schema ' + schemaName);
  validateSchemaValue(value, schema, schema, schemaName);
  return true;
}

export function validateSchemaBundleIdentity(value) {
  validateWithSchema('schema-bundle-identity.schema.json', value);
  if (value.schema_id !== SCHEMA_IDS['schema-bundle-identity.schema.json']) fail('bundle schema ID mismatch');
  assertSortedUnique(value.schemas, (record) => record.path, 'bundle schema records');
  if (canonicalWriter(value.schemas) !== canonicalWriter(SCHEMA_RECORDS)) fail('bundle schema record mismatch');
  const expected = documentIdHex(value.schema_id, withoutOwnId(value, 'bundle_id'));
  if (value.bundle_id !== expected || value.bundle_id !== SCHEMA_BUNDLE_ID) fail('bundle document ID mismatch');
  return true;
}

export function validateSchemaTrustAnchor(value) {
  validateWithSchema('schema-trust-anchor.schema.json', value);
  if (value.schema_id !== SCHEMA_IDS['schema-trust-anchor.schema.json']) fail('anchor schema ID mismatch');
  assertSortedUnique(value.compatibility, (record) => record.bundle_id, 'anchor compatibility');
  const expected = documentIdHex(value.schema_id, withoutOwnId(value, 'anchor_id'));
  if (value.anchor_id !== expected) fail('anchor document ID mismatch');
  return true;
}

export function validateOwnershipManifest(value, authority) {
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

export function validateOwnershipTransfer(value) {
  validateWithSchema('ownership-transfer.schema.json', value);
  if (value.schema_id !== SCHEMA_IDS['ownership-transfer.schema.json']) fail('transfer schema ID mismatch');
  if (value.schema_bundle_id !== SCHEMA_BUNDLE_ID) fail('transfer bundle ID mismatch');
  if (!validRepositoryPath(value.path)) fail('invalid transfer path');
  const expected = documentIdHex(value.schema_id, withoutOwnId(value, 'transfer_id'));
  if (value.transfer_id !== expected) fail('transfer document ID mismatch');
  return true;
}

export function requireAcceptedTransfer(value) {
  validateOwnershipTransfer(value);
  if (value.status !== 'accepted') fail('ownership transfer is not accepted');
  return true;
}

export function writeSchemaBundleIdentity(value) {
  validateSchemaBundleIdentity(value);
  return canonicalWriter(value);
}

export function writeSchemaTrustAnchor(value) {
  validateSchemaTrustAnchor(value);
  return canonicalWriter(value);
}

export function writeOwnershipManifest(value, authority) {
  validateOwnershipManifest(value, authority);
  return canonicalWriter(value);
}

export function writeOwnershipTransfer(value) {
  validateOwnershipTransfer(value);
  return canonicalWriter(value);
}
export const validateManifest = validateOwnershipManifest;
export const validateTransfer = validateOwnershipTransfer;
