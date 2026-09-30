export function fuzzyScore(text, query) {
  const needle = [...query.toLocaleLowerCase()];
  if (!needle.length) return 0;
  let score=0, previous=-2, index=0, q=0, before=' ';
  for(const character of text.toLocaleLowerCase()) {
    if(character===needle[q]) {
      score+=10+(previous===index-1?20:0)+(/[\s\p{P}]/u.test(before)?15:0);
      if(previous<0)score-=Math.min(index,50);
      previous=index;
      if(++q===needle.length)return score;
    }
    before=character;index++;
  }
  return -Infinity;
}

export function searchHistory(messages, chats, query) {
  const results=[];
  for(const list of messages.values())for(const message of list) {
    const chatName=chats.get(message.jid)?.name || message.jid;
    const score=fuzzyScore(`${chatName} ${message.mine?'You':message.sender} ${message.text}`,query);
    if(score!==-Infinity)results.push({message,score});
  }
  return results.sort((a,b)=>b.score-a.score || b.message.time-a.message.time)
    .slice(0,200).map(result=>result.message);
}
