import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { execFile } from 'node:child_process';
import { promisify } from 'node:util';

const run = promisify(execFile);
export const limit = 20 * 1024 * 1024;
export const dir = path.join(process.env.XDG_STATE_HOME || path.join(process.env.HOME, '.local/state'), 'whatsapp-tui');
export const mediaDir = path.join(dir, 'media');
const attachments = path.join(dir, 'attachments');
process.umask(0o077);
for (const folder of [dir, mediaDir, attachments]) fs.mkdirSync(folder, {recursive:true, mode:0o700});
export function mediaKey(jid, id) {
  return crypto.createHash('sha256').update(jid + '\0' + id).digest('hex');
}
export function attachmentPath(file) {
  if (typeof file !== 'string' || !file) throw Error('No photo attached');
  const resolved = fs.realpathSync(file);
  if (path.dirname(resolved) !== fs.realpathSync(attachments) || !fs.statSync(resolved).isFile()) throw Error('Invalid attachment');
  if (fs.statSync(resolved).size > limit) throw Error('Photo exceeds the 20 MB limit');
  return resolved;
}
export function discardAttachment(file) {
  if (file) fs.unlinkSync(attachmentPath(file));
}
export async function photoContent(file,caption) {
  const header=Buffer.alloc(24),fd=fs.openSync(file,'r');
  try {fs.readSync(fd,header,0,24,0);} finally {fs.closeSync(fd);}
  const {stdout:jpegThumbnail}=await run('magick',[file,'-thumbnail','64x64>','-alpha','off','-quality','65','jpeg:-'],
    {encoding:'buffer',maxBuffer:65536,timeout:30000});
  return {image:{url:file},caption,mimetype:'image/png',jpegThumbnail,
    width:header.readUInt32BE(16),height:header.readUInt32BE(20)};
}
export async function clipboardImage(jid) {
  let types;
  try { ({stdout:types} = await run('wl-paste', ['--list-types'], {timeout:5000,maxBuffer:16384})); }
  catch { throw Error('Clipboard unavailable. Image paste needs wl-paste in a Wayland session.'); }
  const offered = types.trim().split(/\r?\n/);
  const mime = ['image/png','image/jpeg','image/webp','image/bmp'].find(type => offered.includes(type));
  if (!mime) throw Error('No image in the clipboard. Copy an image or take a screenshot first.');
  let data;
  try { ({stdout:data} = await run('wl-paste', ['--no-newline','--type',mime], {encoding:'buffer',maxBuffer:limit,timeout:10000})); }
  catch { throw Error('Could not read clipboard image (maximum 20 MB).'); }
  if (!data.length) throw Error('Clipboard image is empty');
  const name = crypto.randomUUID();
  const source = path.join(attachments, name + '.source');
  const file = path.join(attachments, name + '.png');
  try {
    fs.writeFileSync(source,data,{mode:0o600});
    await run('magick', ['-limit','memory','128MiB','-limit','map','256MiB',source+'[0]',
      '-auto-orient','-strip','PNG32:'+file], {timeout:30000,maxBuffer:65536});
    attachmentPath(file);
    return {type:'attached',jid,path:file,bytes:fs.statSync(file).size};
  } catch (err) {
    fs.rmSync(file,{force:true});
    if (err.code === 'ENOENT') throw Error('ImageMagick is required for image previews (install imagemagick).');
    throw Error('Could not decode the clipboard image, or it exceeds 20 MB.');
  } finally { fs.rmSync(source,{force:true}); }
}
export async function renderImage(file, command) {
  const width = Math.max(32,Math.min(1800,Number(command.width)||800));
  const height = Math.max(32,Math.min(1400,Number(command.height)||600));
  const format = ['kitty','sixel'].includes(command.format) ? command.format : 'external';
  const result = {type:'image',request:command.request,path:file,format};
  if (format === 'external') return result;
  const preview = path.join(mediaDir,'preview-'+crypto.randomUUID()+'.png');
  try {
    await run('magick',['-limit','memory','128MiB','-limit','map','256MiB',file+'[0]',
      '-auto-orient','-thumbnail',`${width}x${height}>`,'-strip','PNG32:'+preview],
      {timeout:30000,maxBuffer:65536});
    const png = fs.readFileSync(preview);
    result.width=png.readUInt32BE(16);result.height=png.readUInt32BE(20);
    if (format === 'kitty') result.data=png.toString('base64');
    else {
      const {stdout} = await run('magick',[preview,'-colors','256','sixel:-'],
        {timeout:30000,encoding:'buffer',maxBuffer:8*1024*1024});
      result.data=stdout.toString('base64');
    }
    return result;
  } catch (err) {
    return {...result,format:'external',error:err.code==='ENOENT'
      ? 'Install ImageMagick for terminal previews; O opens this photo externally.'
      : 'Could not render this photo here; O opens it externally.'};
  } finally { fs.rmSync(preview,{force:true}); }
}
