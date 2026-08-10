import assert from 'node:assert/strict'; import {validateManifest} from '../generated/validator.mjs';
const e={path:'a',state:'planned',owner:'x',reviewer:'y',dependency:'z',transfer:'lane-0'}; assert.doesNotThrow(()=>validateManifest({entries:[e]})); assert.throws(()=>validateManifest({entries:[e,e]})); console.log('ownership PASS');
