#!/usr/bin/env node
/**
 * AE-RING-02 NEGATIVE CONTROL.
 *
 * The paired cli-note-rapid.mjs suite runs with the sidecar's event-drain thread attached. This
 * wrapper changes only that condition and is a separately discovered all.mjs suite, so the default
 * gate cannot accidentally exercise just one causal arm.
 */

import { spawnSync } from 'node:child_process';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const result = spawnSync(
  process.execPath,
  [join(here, 'cli-note-rapid.mjs'), '--without-sidecar'],
  { stdio: 'inherit' },
);

if (result.error) {
  console.error(`cli-note-rapid control could not start: ${result.error.message}`);
  process.exit(1);
}
process.exit(result.status ?? 1);
