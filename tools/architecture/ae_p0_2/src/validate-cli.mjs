import fs from 'node:fs';
import path from 'node:path';

import { parseCanonicalJson } from './canonical.mjs';
import {
  REQUIRED_SCHEMA_PATHS,
  validateAnchor,
  verifyBootstrap,
} from './bootstrap-validator.mjs';
import { existingPaths, referencePaths } from './inventory.mjs';

const architectureRoot = path.resolve(import.meta.dirname, '..');
const targetPath = process.argv[2];
if (!targetPath) throw new Error('usage: validate-cli.mjs FILE');

const read = (relative) => fs.readFileSync(path.join(architectureRoot, relative));
const schemaBytesByName = Object.fromEntries(REQUIRED_SCHEMA_PATHS.map((name) => [
  name,
  read(path.join('schemas', name)),
]));

const trusted = await verifyBootstrap({
  anchorBytes: read('bootstrap/schema-trust-anchor.json'),
  bundleBytes: read('generated/schema-bundle-identity.json'),
  schemaBytesByName,
  validatorBytes: read('generated/validator.mjs'),
});

const document = parseCanonicalJson(fs.readFileSync(path.resolve(targetPath)));
const schemaName = Object.entries(trusted.validator.SCHEMA_IDS)
  .find(([, schemaId]) => schemaId === document.schema_id)?.[0];
if (!schemaName) throw new Error('document names an unknown schema ID');

switch (schemaName) {
  case 'schema-bundle-identity.schema.json':
    trusted.validator.validateSchemaBundleIdentity(document);
    break;
  case 'schema-trust-anchor.schema.json':
    validateAnchor(
      document,
      trusted.bundle.schemas.find((record) => record.path === 'schema-trust-anchor.schema.json').schema_id,
      trusted.bundle.bundle_id,
    );
    trusted.validator.validateSchemaTrustAnchor(document);
    break;
  case 'ownership-manifest.schema.json':
    {
      const existing = existingPaths();
      const reference = referencePaths(existing);
      trusted.validator.validateOwnershipManifest(document, {
        existingPaths: existing,
        referencePaths: reference,
      });
    }
    break;
  case 'ownership-transfer.schema.json':
    trusted.validator.validateOwnershipTransfer(document);
    break;
  default:
    throw new Error('document schema is not supported');
}

console.log('PASS');
