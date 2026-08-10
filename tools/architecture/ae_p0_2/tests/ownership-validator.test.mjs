import assert from 'node:assert/strict';
import { validateManifest } from '../generated/validator.mjs';

const entry = (path) => ({ path, state: 'planned', owner: 'x', reviewer: 'y', dependency: 'z', transfer: 'lane-0' });
assert.doesNotThrow(() => validateManifest({ entries: [entry('a')] }));
assert.doesNotThrow(() => validateManifest({ entries: [entry('a'), entry('b')] }));
assert.throws(() => validateManifest({ entries: [entry('a'), entry('a')] }), /duplicate|unsorted/);
assert.throws(() => validateManifest({ entries: [entry('b'), entry('a')] }), /unsorted/);
console.log('ownership PASS');
