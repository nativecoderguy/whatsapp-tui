import makeWASocket, { useMultiFileAuthState, DisconnectReason, Browsers,
  fetchLatestBaileysVersion, normalizeMessageContent, downloadMediaMessage, BufferJSON } from '@whiskeysockets/baileys';
import pino from 'pino';
import fs from 'node:fs';
import path from 'node:path';
import readline from 'node:readline';
import {mediaDir, mediaKey, clipboardImage, renderImage, attachmentPath, discardAttachment, photoContent, limit} from './media.mjs';

process.umask(0o077);
const dir = path.join(process.env.XDG_STATE_HOME || path.join(process.env.HOME, '.local/state'), 'whatsapp-tui');
fs.mkdirSync(dir, { recursive: true, mode: 0o700 });
const emit = value => process.stdout.write(JSON.stringify(value) + '\n');
const logger = pino({ level: 'silent' });
const chats = new Map();
const messages = new Map();
const contactNames = new Map();
let socket, connected = false, stopping = false, retry = 0, reconnectTimer, saveTimer;
let latestView, viewJob=Promise.resolve();
let thumbnailJob=Promise.resolve();
const imageDownloads=new Map();
const cache = path.join(dir, 'history.json');
try {
  const saved = JSON.parse(fs.readFileSync(cache, 'utf8'));
  for (const chat of saved.chats || []) chats.set(chat.id, chat);
  for (const [jid, list] of Object.entries(saved.messages || {})) messages.set(jid, list);
} catch { /* First start or interrupted cache write. */ }
function save() {
  clearTimeout(saveTimer);
  saveTimer = setTimeout(() => {
    try {
      const tmp = cache + '.tmp';
      fs.writeFileSync(tmp, JSON.stringify({ chats: [...chats.values()], messages: Object.fromEntries(messages) }), { mode: 0o600 });
      fs.renameSync(tmp, cache);
    } catch { emit({ type: 'error', text: 'Could not save local chat history.' }); }
  }, 700);
}
function validJid(id) { return typeof id === 'string' && /@(s\.whatsapp\.net|g\.us|lid)$/.test(id); }
function chatEvent(chat) { emit({ type: 'chat', ...chat }); }
function updateChat(raw) {
  if (!validJid(raw.id)) return;
  const old = chats.get(raw.id) || { id: raw.id, name: contactNames.get(raw.id) || raw.id.split('@')[0], unread: 0, time: 0, preview: '' };
  const chat = { ...old, name: raw.name || raw.subject || old.name,
    unread: raw.unreadCount == null ? old.unread : Math.max(0, raw.unreadCount),
    time: Number(raw.conversationTimestamp || old.time) };
  chats.set(chat.id, chat); chatEvent(chat); save();
}
function updateContact(contact) {
  const name = contact.name || contact.notify;
  if (!validJid(contact.id) || !name) return;
  contactNames.set(contact.id, name);
  if (chats.has(contact.id)) updateChat({id:contact.id, name});
}
function unpack(raw, notify = false) {
  const jid = raw.key?.remoteJid;
  if (!validJid(jid) || !raw.message) return;
  const content = normalizeMessageContent(raw.message) || raw.message;
  if (content.protocolMessage || content.senderKeyDistributionMessage || content.reactionMessage) return;
  let body = content.conversation || content.extendedTextMessage?.text;
  if (!body) {
    const types = { imageMessage: 'Photo', videoMessage: 'Video', audioMessage: 'Voice message',
      stickerMessage: 'Sticker', documentMessage: 'Document', contactMessage: 'Contact',
      locationMessage: 'Location', pollCreationMessage: 'Poll' };
    for (const [key, label] of Object.entries(types)) {
      if (content[key]) { body = '[' + label + '] ' + (content[key].caption || content[key].fileName || ''); break; }
    }
  }
  if (!body) return;
  const list = messages.get(jid) || [];
  const existing = list.find(item => item.id === raw.key.id);
  const wrapper = raw.message.ephemeralMessage?.message || raw.message;
  const photo = !!content.imageMessage && !wrapper.viewOnceMessage && !wrapper.viewOnceMessageV2 && !wrapper.viewOnceMessageV2Extension && !content.imageMessage.viewOnce;
  if (photo) {
    const meta = path.join(mediaDir,mediaKey(jid,raw.key.id)+'.json');
    try { fs.writeFileSync(meta,JSON.stringify(raw,BufferJSON.replacer),{mode:0o600}); }
    catch { emit({type:'notice',text:'Could not save photo metadata'}); }
    body='[Photo · v to view]' + (content.imageMessage.caption ? '\n'+content.imageMessage.caption : '');
  }
  if (content.imageMessage && !photo) body='[View once photo · open on your phone]';
  if(existing) {
    if(photo && !existing.photo) {existing.photo=true;existing.text=body;emit({type:'message',...existing});save();}
    return;
  }
  const msg = { id: raw.key.id, jid, text: body, mine: !!raw.key.fromMe, photo,
    sender: raw.pushName || raw.key.participant?.split('@')[0] || '', time: Number(raw.messageTimestamp || Date.now()/1000),
    status: raw.key.fromMe ? 'sent' : '' };
  list.push(msg); list.sort((a,b) => a.time-b.time);
  messages.set(jid, list.slice(-500));
  const old = chats.get(jid) || { id: jid, name: contactNames.get(jid) || raw.pushName || jid.split('@')[0], unread: 0, time: 0, preview: '' };
  if (msg.time >= old.time) { old.time = msg.time; old.preview = body; }
  if (notify && !msg.mine) old.unread++;
  chats.set(jid, old); chatEvent(old);
  emit({ type: 'message', ...msg }); save();
}
function showHistory(jid) { emit({ type: 'history', jid, messages: messages.get(jid) || [] }); }
const { state, saveCreds } = await useMultiFileAuthState(path.join(dir, 'auth'));
let version;
try { ({ version } = await fetchLatestBaileysVersion({ signal: AbortSignal.timeout(15000) })); }
catch { /* Baileys' bundled version is the fallback. */ }
async function connect() {
  if (stopping) return;
  connected = false;
  emit({ type: 'status', text: 'Connecting', online: false });
  socket = makeWASocket({ auth: state, logger, ...(version ? {version} : {}),
    browser: Browsers.ubuntu('Omarchy TUI'), syncFullHistory: true,
    markOnlineOnConnect: false, getMessage: async () => undefined });
  socket.ev.on('creds.update', () => saveCreds().catch(err => emit({type:'error',text:err.message})));
  socket.ev.on('connection.update', event => {
    if (event.qr) emit({ type: 'qr', text: event.qr });
    if (event.connection === 'open') {
      connected = true; retry = 0;
      emit({ type: 'status', text: 'Connected', online: true });
      for (const chat of chats.values()) chatEvent(chat);
    }
    if (event.connection === 'close' && !stopping) {
      connected = false;
      emit({ type: 'clear-qr' });
      const code = event.lastDisconnect?.error?.output?.statusCode;
      if (code === DisconnectReason.loggedOut) {
        emit({ type: 'status', text: 'Signed out. Run whatsapp-tui --reset-session to pair again.', online: false });
      } else {
        const delay = Math.min(30000, 1500 * 2 ** Math.min(retry++, 5));
        emit({ type: 'status', text: `Reconnecting in ${Math.round(delay/1000)}s`, online: false });
        clearTimeout(reconnectTimer); reconnectTimer = setTimeout(() => connect().catch(fatal), delay);
      }
    }
  });
  socket.ev.on('messaging-history.set', ({ chats: historyChats, contacts, messages: history }) => {
    contacts.forEach(updateContact);
    historyChats.forEach(updateChat);
    history.forEach(m => unpack(m));
    emit({ type: 'notice', text: 'Chat history synced' });
  });
  socket.ev.on('chats.upsert', items => items.forEach(updateChat));
  socket.ev.on('chats.update', items => items.forEach(updateChat));
  socket.ev.on('contacts.upsert', items => items.forEach(updateContact));
  socket.ev.on('contacts.update', items => items.forEach(updateContact));
  socket.ev.on('messages.upsert', ({messages: items, type}) => items.forEach(m => unpack(m, type === 'notify')));
  socket.ev.on('messages.update', items => {
    for (const {key, update} of items) {
      const msg = messages.get(key.remoteJid)?.find(m => m.id === key.id);
      if (msg && update.status != null) {
        msg.status = update.status >= 4 ? 'read' : update.status >= 3 ? 'delivered' : 'sent';
        emit({type:'message', ...msg}); save();
      }
    }
  });
}
function fatal(err) { emit({ type: 'error', text: err.message || String(err) }); }
async function downloadImage(jid,id) {
  if (!validJid(jid) || typeof id !== 'string') throw Error('Invalid photo');
  const key=mediaKey(jid,id), file=path.join(mediaDir,key+'.image');
  if (fs.existsSync(file)) return file;
  const metadata=path.join(mediaDir,key+'.json');
  if (!fs.existsSync(metadata)) throw Error('Photo metadata unavailable. New photos will support viewing; older messages may need to sync again.');
  if (!connected) throw Error('Connect to WhatsApp to download this photo.');
  const raw=JSON.parse(fs.readFileSync(metadata,'utf8'),BufferJSON.reviver);
  const content=normalizeMessageContent(raw.message);
  if (!content?.imageMessage || Number(content.imageMessage.fileLength||0)>limit) throw Error('Photo exceeds the 20 MB limit.');
  const stream=await downloadMediaMessage(raw,'stream',{}, {logger,reuploadRequest:socket.updateMediaMessage});
  const temporary=file+'.'+process.pid+'.tmp';
  const fd=fs.openSync(temporary,'w',0o600);let bytes=0;
  try {
    for await (const chunk of stream) {
      bytes+=chunk.length;
      if(bytes>limit) throw Error('Photo exceeds the 20 MB limit');
      fs.writeFileSync(fd,chunk);
    }
    fs.closeSync(fd);fs.renameSync(temporary,file);return file;
  } catch(err) {
    try {fs.closeSync(fd);} catch {}
    fs.rmSync(temporary,{force:true});stream.destroy();throw err;
  }
}
function getImage(jid,id) {
  const key=mediaKey(jid,id);
  if(imageDownloads.has(key))return imageDownloads.get(key);
  const job=downloadImage(jid,id).finally(()=>imageDownloads.delete(key));
  imageDownloads.set(key,job);return job;
}
readline.createInterface({ input: process.stdin }).on('line', async line => {
  let command;
  try {
    command = JSON.parse(line);
    if (command.type === 'history' && validJid(command.jid)) showHistory(command.jid);
    if (command.type === 'clipboard') {
      if(!validJid(command.jid)) throw Error('Select a chat before attaching a photo');
      emit(await clipboardImage(command.jid));
    }
    if (command.type === 'discard') discardAttachment(command.path);
    if (command.type === 'thumbnail') {
      const previous=thumbnailJob;
      thumbnailJob=(async()=> {
        await previous.catch(()=>{});
        const file=await getImage(command.jid,command.id);
        const image=await renderImage(file,command);
        emit({...image,type:'inline-image'});
      })();
      await thumbnailJob;
    }
    if (command.type === 'view') {
      latestView=command.request;
      const previous=viewJob;
      viewJob=(async()=> {
        await previous.catch(()=>{});
        if(command.request!==latestView)return;
        const file=command.attachment ? attachmentPath(command.attachment) : await getImage(command.jid,command.id);
        if(command.request!==latestView)return;
        const image=await renderImage(file,command);
        if(command.request===latestView)emit(image);
      })();
      await viewJob;
    }
    if (command.type === 'send' || command.type === 'send-image') {
      if (!connected) throw Error('Offline. Your message has not been sent.');
      const image=command.type==='send-image';
      if (!validJid(command.jid) || typeof command.text !== 'string' || (!image && !command.text.trim()) || command.text.length > 4096) throw Error('Invalid message');
      const file=image?attachmentPath(command.path):undefined;
      const payload=image?await photoContent(file,command.text):{text:command.text};
      const sent = await socket.sendMessage(command.jid,payload);
      if (!sent) throw Error('WhatsApp did not accept the message');
      if(image) {
        try {fs.copyFileSync(file,path.join(mediaDir,mediaKey(command.jid,sent.key.id)+'.image'));}
        catch {emit({type:'notice',text:'Photo sent; could not keep a local image copy.'});}
      }
      unpack(sent);
      emit({type:'sent', token:command.token});
      if(image) { try {discardAttachment(file);} catch {} }
    }
  } catch (err) { emit({ type: 'error', text: err.message, token:command?.token,request:command?.request,scope:command?.type==='thumbnail'?'inline':undefined }); }
});
function stop() { stopping = true; clearTimeout(reconnectTimer); socket?.end(undefined); process.exit(0); }
process.on('SIGTERM', stop); process.on('SIGINT', stop);
process.stdin.on('end', stop);
process.on('unhandledRejection', fatal);
for (const chat of chats.values()) chatEvent(chat);
await connect();
