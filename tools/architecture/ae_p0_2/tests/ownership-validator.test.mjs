import assert from 'node:assert/strict'; import {validateManifest} from '../generated/validator.mjs';
assert.doesNotThrow(()=>validateManifest({entries:[{path:'a'}]})); assert.throws(()=>validateManifest({entries:[{path:'a'},{path:'a'}]})); console.log('ownership PASS');
