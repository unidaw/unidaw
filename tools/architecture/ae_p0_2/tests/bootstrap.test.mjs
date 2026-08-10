import assert from 'node:assert/strict';
import fs from 'node:fs';
import { validateAnchor } from '../src/bootstrap-validator.mjs';
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
} from '../src/canonical.mjs';

const anchorBytes = fs.readFileSync(new URL('../bootstrap/schema-trust-anchor.json', import.meta.url));
const anchor = parseCanonicalJson(anchorBytes);
assert.doesNotThrow(() => validateAnchor(anchor, anchor.schema_bundle_digest));
assert.throws(() => validateAnchor({ ...anchor, anchor_id: 'bad' }));
assert.throws(() => validateAnchor({ ...anchor, schema_bundle_digest: 'bad' }));

assert.equal(jcs({ b: 1, a: [true, null, -0] }), '{"a":[true,null,0],"b":1}');
assert.equal(canonical({}), '{}', 'canonical publication bytes must not add an LF');

const sparse = [];
sparse.length = 1;
const extraArrayProperty = [];
extraArrayProperty.extra = 1;
const hiddenArrayProperty = [];
Object.defineProperty(hiddenArrayProperty, 'toJSON', { value: () => ({}) });
const cyclic = {};
cyclic.self = cyclic;
const accessor = {};
Object.defineProperty(accessor, 'value', { enumerable: true, get: () => 1 });
const hiddenObjectProperty = {};
Object.defineProperty(hiddenObjectProperty, 'hidden', { value: 1 });
const symbolObjectProperty = { [Symbol('hidden')]: 1 };
class NonJsonObject {}

for (const value of [
  undefined,
  () => 1,
  Symbol('value'),
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
  { toJSON() { return {}; } },
  accessor,
  hiddenObjectProperty,
  symbolObjectProperty,
  '\ud800',
  { '\udfff': 1 },
]) {
  assert.throws(() => jcs(value), `non-JSON-domain value was accepted: ${typeof value}`);
}

assert.equal(jcs(Object.assign(Object.create(null), { value: 1 })), '{"value":1}');
assert.equal(jcs(Object.freeze([1, 2])), '[1,2]');

assert.throws(() => parseJsonRejectingDuplicates('{"a":1,"a":2}'), /duplicate JSON key/);
assert.throws(() => parseJsonRejectingDuplicates('{"a":1,"\\u0061":2}'), /duplicate JSON key/);
assert.throws(() => parseJsonRejectingDuplicates(Buffer.from([0xc3, 0x28])), /UTF-8/);
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

const vectors = JSON.parse(fs.readFileSync(new URL('../testdata/golden-vectors.json', import.meta.url)));
const preimage = vectors.vectors.find((vector) => vector.name === 'adr-empty-object-preimages');
assert.ok(preimage, 'missing literal ADR preimage vector');
assert.equal(schemaPreimage({}).toString('hex'), preimage.schema_preimage_hex);
assert.equal(schemaId({}), preimage.schema_id);
assert.equal(documentPreimage('0'.repeat(64), {}).toString('hex'), preimage.document_preimage_hex);
assert.equal(documentId('0'.repeat(64), {}), preimage.document_id);
assert.equal(digest('test-domain', {}), preimage.test_domain_digest);
assert.throws(() => digest('', {}), /digest domain/);
assert.throws(() => digest('bad\0domain', {}), /digest domain/);
assert.throws(() => digest('non-ascii-\u00e5', {}), /digest domain/);
assert.throws(() => documentId('A'.repeat(64), {}), /schema ID/);

assert.equal(canonicalDecimal('18446744073709551615'), 18446744073709551615n);
assert.throws(() => canonicalDecimal('18446744073709551616'), /outside unsigned 64-bit/);
assert.equal(canonicalDecimal('-9223372036854775808', { signed: true }), -9223372036854775808n);
assert.equal(canonicalDecimal('9223372036854775807', { signed: true }), 9223372036854775807n);
assert.throws(() => canonicalDecimal('-9223372036854775809', { signed: true }), /outside signed 64-bit/);
assert.throws(() => canonicalDecimal('9223372036854775808', { signed: true }), /outside signed 64-bit/);
for (const value of ['-0', '00', '+1', '1.0', '1e0']) assert.throws(() => canonicalDecimal(value, { signed: true }));

assert.deepEqual(assertSortedUnique([{ id: 'a' }, { id: 'b' }], (value) => value.id), [{ id: 'a' }, { id: 'b' }]);
assert.throws(() => assertSortedUnique([{ id: 'b' }, { id: 'a' }], (value) => value.id), /unsorted/);
assert.throws(() => assertSortedUnique([{ id: 'a' }, { id: 'a' }], (value) => value.id), /duplicate/);

console.log('bootstrap PASS');
