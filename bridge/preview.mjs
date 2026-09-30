// Local image previews only: no WhatsApp connection, imports, or sends.
import readline from 'node:readline';
import { clipboardImage, renderImage, attachmentPath, discardAttachment } from './media.mjs';
const emit = value => process.stdout.write(JSON.stringify(value)+'\n');
let latestView,viewJob=Promise.resolve();
readline.createInterface({input:process.stdin}).on('line',async line => {
  let command;
  try {
    command=JSON.parse(line);
    if(command.type==='clipboard') emit(await clipboardImage(command.jid));
    else if(command.type==='view' && command.attachment) {
      latestView=command.request;
      const previous=viewJob;
      viewJob=(async()=> {
        await previous.catch(()=>{});
        if(command.request!==latestView)return;
        const image=await renderImage(attachmentPath(command.attachment),command);
        if(command.request===latestView)emit(image);
      })();
      await viewJob;
    }
    else if(command.type==='discard') discardAttachment(command.path);
  } catch(err) {emit({type:'error',text:err.message,request:command?.request});}
});
process.stdin.on('end',()=>process.exit(0));
