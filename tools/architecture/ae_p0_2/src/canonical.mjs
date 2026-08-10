import crypto from 'node:crypto';
export const jcs = value => { if (Array.isArray(value)) return '['+value.map(jcs).join(',')+']'; if (value && typeof value==='object') return '{'+Object.keys(value).sort().map(k=>JSON.stringify(k)+':'+jcs(value[k])).join(',')+'}'; return JSON.stringify(value); };
export const digest = (domain, value) => crypto.createHash('sha256').update(domain+'\0'+jcs(value)).digest('hex');
export const canonical = value => jcs(value)+'\n';
