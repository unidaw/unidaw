import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import test from 'node:test';

import {
  SCHEMA_BUNDLE_ID,
  SCHEMA_IDS,
  canonicalWriter,
  documentIdHex,
  requireAcceptedTransfer,
  validRepositoryPath,
  validateOwnershipManifest,
  validateOwnershipTransfer,
  validateSchemaBundleIdentity,
  validateSchemaTrustAnchor,
  writeOwnershipManifest,
  writeOwnershipTransfer,
  writeSchemaBundleIdentity,
  writeSchemaTrustAnchor,
} from '../generated/validator.mjs';
import {
  BASELINE_SHA,
  IMPLEMENTATION_BASE_SHA,
  amendedBaseDeltaPaths,
  baselinePaths,
  existingPaths,
  plannedPaths,
  referencePaths,
} from '../src/inventory.mjs';
import { parseCanonicalJson } from '../src/canonical.mjs';

const architectureRoot = path.resolve(import.meta.dirname, '..');
const repositoryRoot = path.resolve(architectureRoot, '../../..');
const manifestPath = path.join(repositoryRoot, 'docs/architecture/tasks/AE-P0.2-ownership.json');
const manifestBytes = fs.readFileSync(manifestPath);
const manifest = parseCanonicalJson(manifestBytes);
const bundleBytes = fs.readFileSync(path.join(architectureRoot, 'generated/schema-bundle-identity.json'));
const bundle = parseCanonicalJson(bundleBytes);
const anchorBytes = fs.readFileSync(path.join(architectureRoot, 'bootstrap/schema-trust-anchor.json'));
const anchor = parseCanonicalJson(anchorBytes);
const authorityExistingPaths = existingPaths();
const authority = Object.freeze({
  existingPaths: authorityExistingPaths,
  referencePaths: referencePaths(authorityExistingPaths),
});

function clone(value) {
  return structuredClone(value);
}

function signDocument(value, idField) {
  const body = Object.create(null);
  for (const key of Object.keys(value)) {
    if (key !== idField) body[key] = value[key];
  }
  value[idField] = documentIdHex(value.schema_id, body);
  return value;
}

function changedManifest(change) {
  const candidate = clone(manifest);
  change(candidate);
  return signDocument(candidate, 'manifest_id');
}

function transfer(status = 'proposed') {
  return signDocument({
    from: 'codex-worker-2',
    path: 'tools/architecture/ae_p0_2/tests/cross-language.test.mjs',
    reviewer: 'claude-worker-2',
    schema_bundle_id: SCHEMA_BUNDLE_ID,
    schema_id: SCHEMA_IDS['ownership-transfer.schema.json'],
    status,
    to: 'backend',
    transfer_id: '0'.repeat(64),
  }, 'transfer_id');
}

test('all generated document families validate as closed typed documents', () => {
  assert.doesNotThrow(() => validateSchemaBundleIdentity(bundle));
  assert.equal(writeSchemaBundleIdentity(bundle), bundleBytes.toString('utf8'));
  assert.doesNotThrow(() => validateSchemaTrustAnchor(anchor));
  assert.equal(writeSchemaTrustAnchor(anchor), anchorBytes.toString('utf8'));

  const missingBundleId = clone(bundle);
  delete missingBundleId.bundle_id;
  assert.throws(() => validateSchemaBundleIdentity(missingBundleId), /missing required field bundle_id/);
  const unknownBundleField = clone(bundle);
  unknownBundleField.schema_count = 4;
  assert.throws(() => validateSchemaBundleIdentity(unknownBundleField), /unknown field schema_count/);
  const unknownSchemaRecordField = clone(bundle);
  unknownSchemaRecordField.schemas[0].digest = unknownSchemaRecordField.schemas[0].canonical_sha256;
  assert.throws(() => validateSchemaBundleIdentity(unknownSchemaRecordField), /unknown field digest/);
  const wrongSchemaRecordType = clone(bundle);
  wrongSchemaRecordType.schemas[0].canonical_sha256 = 42;
  assert.throws(() => validateSchemaBundleIdentity(wrongSchemaRecordType), /must be a string/);

  const missingAnchorId = clone(anchor);
  delete missingAnchorId.anchor_id;
  assert.throws(() => validateSchemaTrustAnchor(missingAnchorId), /missing required field anchor_id/);
  const unknownAnchorField = clone(anchor);
  unknownAnchorField.authority = 'local';
  assert.throws(() => validateSchemaTrustAnchor(unknownAnchorField), /unknown field authority/);
  const unknownCompatibilityField = clone(anchor);
  unknownCompatibilityField.compatibility.push({
    bundle_id: 'a'.repeat(64),
    validator_sha256: 'b'.repeat(64),
    note: 'not in the closed schema',
  });
  assert.throws(() => validateSchemaTrustAnchor(unknownCompatibilityField), /unknown field note/);
  const wrongCompatibilityType = clone(anchor);
  wrongCompatibilityType.compatibility.push({
    bundle_id: 42,
    validator_sha256: 'b'.repeat(64),
  });
  assert.throws(() => validateSchemaTrustAnchor(wrongCompatibilityType), /must be a string/);
});

test('every digest-bearing document family rejects trailing line terminators', () => {
  for (const terminator of ['\n', '\r']) {
    assert.throws(() => documentIdHex('a'.repeat(64) + terminator, {}), /schema ID/);

    const bundleDigest = clone(bundle);
    bundleDigest.schemas[0].canonical_sha256 = 'a'.repeat(64) + terminator;
    assert.throws(() => validateSchemaBundleIdentity(bundleDigest), /too long/);

    for (const field of ['bundle_id', 'validator_sha256']) {
      const anchorDigest = clone(anchor);
      anchorDigest.compatibility.push({
        bundle_id: 'a'.repeat(64),
        validator_sha256: 'b'.repeat(64),
      });
      anchorDigest.compatibility[0][field] += terminator;
      assert.throws(() => validateSchemaTrustAnchor(anchorDigest), /too long/);
    }

    const manifestDigest = clone(manifest);
    manifestDigest.manifest_id += terminator;
    assert.throws(() => validateOwnershipManifest(manifestDigest, authority), /too long/);
    const manifestBaseline = clone(manifest);
    manifestBaseline.baseline += terminator;
    assert.throws(() => validateOwnershipManifest(manifestBaseline, authority), /schema constant|too long/);

    const transferDigest = transfer();
    transferDigest.transfer_id += terminator;
    assert.throws(() => validateOwnershipTransfer(transferDigest), /too long/);
  }
});

test('all schema hex domains pin exact lengths independently of regex anchoring', () => {
  const domains = [];
  const visit = (value, location) => {
    if (value === null || typeof value !== 'object') return;
    if (value.pattern === '^[0-9a-f]{64}$') domains.push({ bits: 256, location, value });
    if (value.pattern === '^[0-9a-f]{40}$') domains.push({ bits: 160, location, value });
    for (const [key, child] of Object.entries(value)) visit(child, `${location}.${key}`);
  };
  for (const schemaName of [
    'ownership-manifest.schema.json',
    'ownership-transfer.schema.json',
    'schema-bundle-identity.schema.json',
    'schema-trust-anchor.schema.json',
  ]) {
    visit(parseCanonicalJson(fs.readFileSync(path.join(architectureRoot, 'schemas', schemaName))), schemaName);
  }
  assert.equal(domains.filter(({ bits }) => bits === 256).length, 16);
  assert.equal(domains.filter(({ bits }) => bits === 160).length, 1);
  for (const { bits, location, value } of domains) {
    const length = bits / 4;
    assert.equal(value.minLength, length, `${location} minLength`);
    assert.equal(value.maxLength, length, `${location} maxLength`);
    for (const terminator of ['\n', '\r']) {
      assert.equal(new RegExp(value.pattern).test('a'.repeat(length) + terminator), false, location);
    }
  }
});

test('the exact 756-entry ownership manifest reconciles with independent authority', () => {
  assert.equal(BASELINE_SHA, 'c33da66fe1a66f20eee931335b18465cfddfdb0e');
  assert.equal(IMPLEMENTATION_BASE_SHA, '7710401d72029482c8f3d15869d58dce7e246def');
  assert.deepEqual(amendedBaseDeltaPaths, ['tools/gesture_drag_check.sh']);
  assert.equal(baselinePaths().length, 730);
  assert.equal(authority.existingPaths.length, 731);
  assert.equal(plannedPaths.length, 25);
  assert.equal(authority.referencePaths.length, 756);
  assert.equal(manifest.entries.length, 756);

  assert.doesNotThrow(() => validateOwnershipManifest(manifest, authority));
  assert.equal(writeOwnershipManifest(manifest, authority), manifestBytes.toString('utf8'));

  const amendedExisting = manifest.entries.find((entry) => entry.path === 'tools/gesture_drag_check.sh');
  assert.deepEqual({ ...amendedExisting }, {
    dependency: 'frozen',
    owner: 'frozen',
    path: 'tools/gesture_drag_check.sh',
    reviewer: 'claude-worker-2',
    state: 'existing',
    transfer: 'frozen',
  });

  for (const plannedPath of plannedPaths) {
    assert.equal(manifest.entries.find((entry) => entry.path === plannedPath)?.state, 'planned');
  }
});

test('root and nested documents reject every missing field', () => {
  for (const field of ['baseline', 'entries', 'manifest_id', 'schema_bundle_id', 'schema_id']) {
    const candidate = clone(manifest);
    delete candidate[field];
    assert.throws(() => validateOwnershipManifest(candidate, authority), undefined, `missing root field ${field}`);
  }

  for (const field of ['dependency', 'owner', 'path', 'reviewer', 'state', 'transfer']) {
    const candidate = clone(manifest);
    delete candidate.entries[0][field];
    assert.throws(() => validateOwnershipManifest(candidate, authority), undefined, `missing entry field ${field}`);
  }
});

test('unknown and wrong-typed root and nested fields are closed', () => {
  const unknownRoot = clone(manifest);
  unknownRoot.unreviewed_extension = true;
  assert.throws(() => validateOwnershipManifest(unknownRoot, authority), /unknown field/);

  const unknownNested = clone(manifest);
  unknownNested.entries[0].owners = ['frozen', 'backend'];
  assert.throws(() => validateOwnershipManifest(unknownNested, authority), /unknown field/);

  const wrongRootValues = {
    baseline: 7,
    entries: {},
    manifest_id: 7,
    schema_bundle_id: [],
    schema_id: false,
  };
  for (const [field, value] of Object.entries(wrongRootValues)) {
    const candidate = clone(manifest);
    candidate[field] = value;
    assert.throws(() => validateOwnershipManifest(candidate, authority), undefined, `wrong root type ${field}`);
  }

  const wrongEntryValues = {
    dependency: 7,
    owner: ['frozen'],
    path: 42,
    reviewer: {},
    state: false,
    transfer: null,
  };
  for (const [field, value] of Object.entries(wrongEntryValues)) {
    const candidate = clone(manifest);
    candidate.entries[0][field] = value;
    assert.throws(() => validateOwnershipManifest(candidate, authority), undefined, `wrong entry type ${field}`);
  }
});

test('dotfiles are valid, while aliases and non-repository paths are rejected', () => {
  assert.equal(validRepositoryPath('.gitignore'), true);
  assert.equal(validRepositoryPath('presets/projects/.dawlint'), true);
  assert.equal(manifest.entries.some((entry) => entry.path === '.gitignore'), true);
  assert.equal(manifest.entries.some((entry) => entry.path === 'presets/projects/.dawlint'), true);

  for (const invalidPath of [
    '',
    '/absolute',
    'C:/absolute',
    'C:drive-relative',
    'trailing/',
    './alias',
    'path/../alias',
    'path/./alias',
    'path//alias',
    'windows\\alias',
    'nul\0alias',
  ]) {
    assert.equal(validRepositoryPath(invalidPath), false, invalidPath);
    const candidate = transfer();
    candidate.path = invalidPath;
    signDocument(candidate, 'transfer_id');
    assert.throws(() => validateOwnershipTransfer(candidate), undefined, `invalid path ${JSON.stringify(invalidPath)}`);
  }
});

test('candidate baseline, reference path set, duplicates, and ownership state cannot redefine authority', () => {
  const candidateBaseline = changedManifest((candidate) => {
    candidate.baseline = IMPLEMENTATION_BASE_SHA;
  });
  assert.throws(() => validateOwnershipManifest(candidateBaseline, authority), /schema constant|baseline/i);

  assert.throws(() => validateOwnershipManifest(manifest, {
    existingPaths: authority.referencePaths,
    referencePaths: authority.referencePaths,
  }), /state mismatch|authority/i);

  assert.throws(() => validateOwnershipManifest(manifest, {
    existingPaths: authority.existingPaths,
    referencePaths: authority.referencePaths.slice(0, -1),
  }), /path count mismatch|path set mismatch|authority/i);

  const missingPath = changedManifest((candidate) => {
    candidate.entries.splice(candidate.entries.length - 1, 1);
  });
  assert.throws(() => validateOwnershipManifest(missingPath, authority), /path count mismatch|path set mismatch/);

  const unknownPath = changedManifest((candidate) => {
    candidate.entries[candidate.entries.length - 1].path = 'zzzz-unknown-authority-path';
  });
  assert.throws(() => validateOwnershipManifest(unknownPath, authority), /path set mismatch/);

  const duplicatePath = changedManifest((candidate) => {
    candidate.entries.splice(1, 0, clone(candidate.entries[0]));
  });
  assert.throws(() => validateOwnershipManifest(duplicatePath, authority), /unsorted|duplicate/);

  const wrongState = changedManifest((candidate) => {
    const entry = candidate.entries.find((item) => item.path === 'tools/gesture_drag_check.sh');
    entry.state = 'planned';
  });
  assert.throws(() => validateOwnershipManifest(wrongState, authority), /state mismatch/);

  for (const [field, value, message] of [
    ['dependency', 'lane-0-bootstrap', /dependency mismatch/],
    ['owner', 'backend', /owner mismatch/],
    ['reviewer', 'claude-worker-1', /reviewer mismatch/],
    ['transfer', 'transfer-required', /transfer mismatch/],
  ]) {
    const wrongAuthorityMetadata = changedManifest((candidate) => {
      const entry = candidate.entries.find((item) => item.path === 'tools/gesture_drag_check.sh');
      entry[field] = value;
    });
    assert.throws(() => validateOwnershipManifest(wrongAuthorityMetadata, authority), message);
  }

  assert.throws(() => validateOwnershipManifest(manifest, {}), /independent manifest authority/);
  assert.throws(() => validateOwnershipManifest(manifest), /independent manifest authority/);
  assert.throws(() => validateOwnershipManifest(manifest, {
    ...authority,
    candidateBaselinePaths: authority.referencePaths,
  }), /unknown authority field|independent manifest authority/);
  assert.throws(() => validateOwnershipManifest(manifest, {
    existingPaths: [...authority.existingPaths, authority.existingPaths.at(-1)],
    referencePaths: authority.referencePaths,
  }), /authority existing paths.*unsorted|duplicate/);
  assert.throws(() => validateOwnershipManifest(manifest, {
    existingPaths: [...authority.existingPaths, 'zzzz-not-in-reference'],
    referencePaths: authority.referencePaths,
  }), /authority is inconsistent/);
});

test('manifest validation rejects stale IDs and Proxy-backed documents without invoking traps', () => {
  const staleId = clone(manifest);
  staleId.manifest_id = '0'.repeat(64);
  assert.throws(() => validateOwnershipManifest(staleId, authority), /document ID mismatch/);

  let traps = 0;
  const trap = () => {
    traps += 1;
    throw new Error('validator invoked a Proxy trap');
  };
  const proxy = new Proxy({}, {
    get: trap,
    getOwnPropertyDescriptor: trap,
    getPrototypeOf: trap,
    ownKeys: trap,
  });
  assert.throws(() => validateOwnershipManifest(proxy, authority), /Proxy/);
  assert.equal(traps, 0);

  const nestedProxy = clone(manifest);
  nestedProxy.entries[0] = new Proxy({}, {
    get: trap,
    getOwnPropertyDescriptor: trap,
    getPrototypeOf: trap,
    ownKeys: trap,
  });
  assert.throws(() => validateOwnershipManifest(nestedProxy, authority), /Proxy/);
  assert.equal(traps, 0);
});

test('ownership transfers validate all statuses but only accepted transfers pass the gate', () => {
  for (const status of ['accepted', 'proposed', 'rejected']) {
    const candidate = transfer(status);
    assert.doesNotThrow(() => validateOwnershipTransfer(candidate));
    assert.equal(writeOwnershipTransfer(candidate), canonicalWriter(candidate));
    if (status === 'accepted') {
      assert.doesNotThrow(() => requireAcceptedTransfer(candidate));
    } else {
      assert.throws(() => requireAcceptedTransfer(candidate), /not accepted/);
    }
  }

  const missingId = transfer('accepted');
  delete missingId.transfer_id;
  assert.throws(() => validateOwnershipTransfer(missingId), /missing required field transfer_id/);

  const unknownField = transfer('accepted');
  unknownField.approver = 'backend';
  assert.throws(() => validateOwnershipTransfer(unknownField), /unknown field approver/);

  const wrongType = transfer('accepted');
  wrongType.from = ['codex-worker-2'];
  assert.throws(() => validateOwnershipTransfer(wrongType), /closed enum/);

  const staleId = transfer('accepted');
  staleId.transfer_id = 'f'.repeat(64);
  assert.throws(() => validateOwnershipTransfer(staleId), /document ID mismatch/);
});
