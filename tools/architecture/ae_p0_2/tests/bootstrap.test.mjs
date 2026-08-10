import assert from 'node:assert/strict'; import {validateAnchor,TRUST_ANCHOR_ID} from '../src/bootstrap-validator.mjs';
assert.doesNotThrow(()=>validateAnchor({anchor_id:TRUST_ANCHOR_ID,schema_bundle_id:'ae-p0-2.schema-bundle-identity'})); assert.throws(()=>validateAnchor({anchor_id:'bad'})); console.log('bootstrap PASS');
