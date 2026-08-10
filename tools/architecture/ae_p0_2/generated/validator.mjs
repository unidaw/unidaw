import fs from 'node:fs';
export function validate(value){ if(!value||typeof value!=='object'||Array.isArray(value)) throw Error('object required'); return true; }
export function validateManifest(value){ validate(value); if(!Array.isArray(value.entries)) throw Error('entries required'); const seen=new Set(); for(const e of value.entries){if(!e.path||seen.has(e.path)) throw Error('duplicate or missing path');seen.add(e.path)} return true; }
