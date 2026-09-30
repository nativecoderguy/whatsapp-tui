// Keep reply lookup and mention validation independent of the live socket.
export function composeOptions(command, list, loadPhoto) {
  let quoted;
  if (command.quote) {
    const message = list.find(m => m.id === command.quote && m.jid === command.jid);
    if (!message) throw Error('The quoted message is no longer in local history. Choose another message.');
    if (!message.raw && !message.photo && message.jid.endsWith('@g.us') && !message.senderJid)
      throw Error('This older group message needs to sync again before it can be quoted.');
    quoted = message.raw || (message.photo ? loadPhoto(message) : {
      key: {remoteJid: message.jid, id: message.id, fromMe: message.mine,
        ...(message.senderJid ? {participant: message.senderJid} : {})},
      message: {conversation: message.text},
    });
    if (!quoted?.message) throw Error('This message needs to sync again before it can be quoted.');
  }
  const mentions = [...new Set(command.mentions || [])];
  if (mentions.length > 32 || mentions.some(id => typeof id !== 'string' || !/^[^\s@]+@(s\.whatsapp\.net|lid)$/.test(id)))
    throw Error('Invalid mentions');
  return {mentions, options: quoted ? {quoted} : {}};
}
