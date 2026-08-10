import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import test from 'node:test';

import { canonical, parseCanonicalJson } from '../src/canonical.mjs';

const architectureRoot = path.resolve(import.meta.dirname, '..');
const repositoryRoot = path.resolve(architectureRoot, '../../..');
const generatedRoot = path.join(architectureRoot, 'generated');
const vectorPath = path.join(architectureRoot, 'testdata/golden-vectors.json');
const manifestPath = path.join(repositoryRoot, 'docs/architecture/tasks/AE-P0.2-ownership.json');
const vectors = parseCanonicalJson(fs.readFileSync(vectorPath));
const manifest = parseCanonicalJson(fs.readFileSync(manifestPath));

function schemaId(name) {
  const record = vectors.bundle.schemas.find((candidate) => candidate.path === name);
  assert.ok(record, `missing golden-vector schema record ${name}`);
  return record.schema_id;
}

const fixtureArguments = [
  vectors.literals.empty_schema_id,
  vectors.literals.empty_document_id,
  vectors.bundle.bundle_id,
  vectors.bundle.schema_id,
  schemaId('ownership-manifest.schema.json'),
  schemaId('ownership-transfer.schema.json'),
  schemaId('schema-trust-anchor.schema.json'),
  manifest.baseline,
  canonical(vectors.bundle),
  ...vectors.bundle.schemas.flatMap((record) => [
    record.canonical_sha256,
    record.path,
    record.schema_id,
  ]),
  vectors.literals.empty_schema_preimage_hex,
  vectors.literals.empty_document_preimage_hex,
];
assert.equal(fixtureArguments.length, 23);

function withScratchDirectory(callback) {
  const temporaryRoot = fs.realpathSync(os.tmpdir());
  const prefix = 'daw-ae-p0-2-cross-language-';
  const created = fs.mkdtempSync(path.join(temporaryRoot, prefix));
  const scratch = fs.realpathSync(created);
  assert.equal(path.dirname(scratch), temporaryRoot);
  assert.match(path.basename(scratch), /^daw-ae-p0-2-cross-language-[A-Za-z0-9_-]+$/);
  try {
    return callback(scratch);
  } finally {
    fs.rmSync(scratch, { recursive: true });
  }
}

test('generated C++ contract compiles as C++17 and its fixture executes', () => {
  withScratchDirectory((scratch) => {
    const executable = path.join(scratch, 'contracts-cpp-test');
    execFileSync('clang++', [
      '-std=c++17',
      '-Wall',
      '-Wextra',
      '-pedantic-errors',
      path.join(import.meta.dirname, 'contracts_cpp_test.cpp'),
      '-o',
      executable,
    ], {
      cwd: architectureRoot,
      stdio: 'pipe',
    });
    execFileSync(executable, fixtureArguments, {
      cwd: architectureRoot,
      stdio: 'pipe',
    });
  });
});

test('generated Rust contract compiles as Rust 2021 and its fixture executes', () => {
  withScratchDirectory((scratch) => {
    const executable = path.join(scratch, 'contracts-rust-test');
    execFileSync('rustc', [
      '--edition=2021',
      path.join(import.meta.dirname, 'contracts_rust_test.rs'),
      '-o',
      executable,
    ], {
      cwd: architectureRoot,
      stdio: 'pipe',
    });
    execFileSync(executable, fixtureArguments, {
      cwd: architectureRoot,
      stdio: 'pipe',
    });
  });
});

test('TypeScript artifact contains concrete types, validators, writers, and literal constants', () => {
  const source = fs.readFileSync(path.join(generatedRoot, 'contracts.ts'), 'utf8');

  for (const name of [
    'SchemaRecord',
    'SchemaBundleIdentity',
    'CompatibilityRecord',
    'SchemaTrustAnchor',
    'OwnershipEntry',
    'OwnershipManifest',
    'OwnershipTransfer',
  ]) {
    assert.match(source, new RegExp(`export interface ${name}\\b`), `missing TypeScript type ${name}`);
  }

  for (const name of [
    'validateSchemaRecord',
    'validateSchemaBundleIdentity',
    'validateSchemaTrustAnchor',
    'validateOwnershipEntry',
    'validateOwnershipManifest',
    'validateOwnershipTransfer',
    'writeSchemaBundleIdentity',
    'writeSchemaTrustAnchor',
    'writeOwnershipManifest',
    'writeOwnershipTransfer',
    'canonicalWriter',
  ]) {
    assert.match(source, new RegExp(`export (?:function|const) ${name}\\b`), `missing TypeScript API ${name}`);
  }

  assert.match(source, /export type ProxyDetector=\(value:object\)=>boolean;/);
  assert.match(source, /value\.length===64&&\/\^\[0-9a-f\]\{64\}\$\//);
  for (const name of [
    'validateSchemaRecord',
    'validateSchemaBundleIdentity',
    'validateSchemaTrustAnchor',
    'validateOwnershipEntry',
    'validateOwnershipManifest',
    'validateOwnershipTransfer',
  ]) {
    assert.match(source, new RegExp(`function ${name}\\(value:unknown,isProxy:ProxyDetector\\)`));
  }
  for (const name of [
    'canonicalWriter',
    'writeSchemaBundleIdentity',
    'writeSchemaTrustAnchor',
    'writeOwnershipManifest',
    'writeOwnershipTransfer',
  ]) {
    assert.match(source, new RegExp(`${name}=\\(value:[^,]+,isProxy:ProxyDetector\\)`));
  }
  assert.match(source, /rejectProxy\(value,isProxy\).*Object\.getPrototypeOf\(value\)/);
  assert.match(source, /Object\.getOwnPropertyDescriptor\(value,key\)/);
  assert.match(source, /canonicalValue\(descriptor\.value,isProxy,active\)/);
  assert.equal(source.includes('canonicalValue(value[key]'), false);
  assert.equal(source.includes('.map(canonicalValue)'), false);

  const literalConstants = new Map([
    ['SCHEMA_BUNDLE_ID', vectors.bundle.bundle_id],
    ['OWNERSHIP_MANIFEST_SCHEMA_ID', schemaId('ownership-manifest.schema.json')],
    ['OWNERSHIP_TRANSFER_SCHEMA_ID', schemaId('ownership-transfer.schema.json')],
    ['SCHEMA_BUNDLE_SCHEMA_ID', schemaId('schema-bundle-identity.schema.json')],
    ['SCHEMA_TRUST_ANCHOR_SCHEMA_ID', schemaId('schema-trust-anchor.schema.json')],
    ['EMPTY_SCHEMA_ID', vectors.literals.empty_schema_id],
    ['EMPTY_SCHEMA_PREIMAGE_HEX', vectors.literals.empty_schema_preimage_hex],
    ['EMPTY_DOCUMENT_ID', vectors.literals.empty_document_id],
    ['EMPTY_DOCUMENT_PREIMAGE_HEX', vectors.literals.empty_document_preimage_hex],
  ]);
  for (const [name, value] of literalConstants) {
    assert.equal(
      source.includes(`export const ${name} = ${JSON.stringify(value)} as const;`),
      true,
      `TypeScript literal ${name} does not match the committed vectors`,
    );
  }
  console.log('TypeScript artifact inspected structurally; no TypeScript compiler was invoked');
});
