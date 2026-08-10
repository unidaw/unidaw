import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

const architectureRoot = path.resolve(import.meta.dirname, '..');
const repositoryRoot = path.resolve(architectureRoot, '../../..');
const temporaryRoot = fs.realpathSync(os.tmpdir());
const temporaryPrefix = 'daw-ae-p0-2-freshness-';
const scratch = fs.mkdtempSync(path.join(temporaryRoot, temporaryPrefix));

function assertOwnedScratchDirectory(directory) {
  const resolved = fs.realpathSync(directory);
  assert.equal(path.dirname(resolved), temporaryRoot);
  assert.match(path.basename(resolved), /^daw-ae-p0-2-freshness-[A-Za-z0-9_-]+$/);
  return resolved;
}

function generate(outputRoot) {
  execFileSync(process.execPath, ['src/generate.mjs'], {
    cwd: architectureRoot,
    env: { ...process.env, AE_P0_2_OUTPUT_ROOT: outputRoot },
    stdio: 'pipe',
  });
}

const generatedPaths = [
  'tools/architecture/ae_p0_2/generated/schema-bundle-identity.json',
  'tools/architecture/ae_p0_2/generated/validator.mjs',
  'tools/architecture/ae_p0_2/generated/contracts.hpp',
  'tools/architecture/ae_p0_2/generated/contracts.rs',
  'tools/architecture/ae_p0_2/generated/contracts.ts',
  'tools/architecture/ae_p0_2/testdata/golden-vectors.json',
  'docs/architecture/tasks/AE-P0.2-ownership.json',
];

const authoritativeSentinels = new Map([
  [
    'tools/architecture/ae_p0_2/bootstrap/schema-trust-anchor.json',
    Buffer.from('{"sentinel":"the trust anchor is not generator-owned"}\n'),
  ],
  [
    'tools/architecture/ae_p0_2/bootstrap/schema-trust-anchor-id.mjs',
    Buffer.from('export const TRUST_ANCHOR_ID = "generator-must-not-write-this";\n'),
  ],
]);

const ownedScratch = assertOwnedScratchDirectory(scratch);
try {
  const seededOutput = path.join(ownedScratch, 'seeded-output');
  const cleanOutput = path.join(ownedScratch, 'clean-output');
  fs.mkdirSync(seededOutput);
  fs.mkdirSync(cleanOutput);

  for (const [relativePath, sentinel] of authoritativeSentinels) {
    const destination = path.join(seededOutput, relativePath);
    fs.mkdirSync(path.dirname(destination), { recursive: true });
    fs.writeFileSync(destination, sentinel, { flag: 'wx' });
  }

  generate(seededOutput);

  for (const relativePath of generatedPaths) {
    assert.deepEqual(
      fs.readFileSync(path.join(seededOutput, relativePath)),
      fs.readFileSync(path.join(repositoryRoot, relativePath)),
      `${relativePath} is stale`,
    );
  }

  for (const [relativePath, sentinel] of authoritativeSentinels) {
    assert.deepEqual(
      fs.readFileSync(path.join(seededOutput, relativePath)),
      sentinel,
      `${relativePath} was overwritten by the generator`,
    );
  }

  generate(cleanOutput);
  for (const relativePath of authoritativeSentinels.keys()) {
    assert.equal(
      fs.existsSync(path.join(cleanOutput, relativePath)),
      false,
      `${relativePath} was created by the generator`,
    );
  }
} finally {
  fs.rmSync(ownedScratch, { recursive: true });
}

console.log('freshness PASS');
