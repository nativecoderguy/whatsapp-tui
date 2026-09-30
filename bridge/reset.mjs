import fs from 'node:fs';
import path from 'node:path';
const dir = path.join(process.env.XDG_STATE_HOME || path.join(process.env.HOME, '.local/state'), 'whatsapp-tui');
fs.rmSync(path.join(dir, 'auth'), {recursive: true, force: true});
fs.rmSync(path.join(dir, 'history.json'), {force: true});
fs.rmSync(path.join(dir, 'history.json.tmp'), {force: true});
fs.rmSync(path.join(dir, 'media'), {recursive:true,force:true});
fs.rmSync(path.join(dir, 'attachments'), {recursive:true,force:true});
console.log('Local pairing and history removed. Run whatsapp-tui to scan a new QR.');
