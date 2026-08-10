import fs from 'node:fs'; import {validateManifest} from '../generated/validator.mjs';
const file=process.argv[2]; if(!file) throw Error('usage: validate-cli.mjs FILE'); const value=JSON.parse(fs.readFileSync(file,'utf8')); validateManifest(value); console.log('PASS');
