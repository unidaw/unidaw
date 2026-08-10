import crypto from 'node:crypto';
export function jcs(v) { if (v === null || typeof v !== 'object') return JSON.stringify(v); if (Array.isArray(v)) return '['+v.map(jcs).join(',')+']'; return '{'+Object.keys(v).sort().map(k=>JSON.stringify(k)+':'+jcs(v[k])).join(',')+'}'; }
export const canonical = v => jcs(v)+'\n';
export const sha256 = bytes => crypto.createHash('sha256').update(bytes).digest('hex');
export const digest = (domain, value) => sha256(Buffer.concat([Buffer.from(domain+'\0'),Buffer.from(jcs(value))]));
