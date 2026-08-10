import fs from 'node:fs'; import crypto from 'node:crypto';
export const TRUST_ANCHOR_ID='ae-p0-2-anchor-'+crypto.createHash('sha256').update('AE-P0.2/trust-anchor').digest('hex');
export function validateAnchor(anchor){if(!anchor||anchor.anchor_id!==TRUST_ANCHOR_ID) throw Error('trust anchor mismatch'); if(typeof anchor.schema_bundle_id!=='string') throw Error('schema bundle missing'); return true;}
if(import.meta.url===`file://${process.argv[1]}`) validateAnchor(JSON.parse(fs.readFileSync(process.argv[2],'utf8')));
