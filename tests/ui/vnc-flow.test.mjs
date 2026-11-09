                                                           
                                                          
                                                 
import assert from 'node:assert/strict';
import test from 'node:test';
import { openVncChannel } from '../../ui/vnc-flow.mjs';
const uuid = '11111111-1111-1111-1111-111111111111';
const nonce = 'AAAAAAAAAAAAAAAAAAAAAA==';
class Socket {
  constructor(url, protocol) { this.url=url;this.protocol=protocol;this.sent=[];this.bufferedAmount=0;Socket.last=this; }
  send(data) { this.sent.push(typeof data==='string'?data:new Uint8Array(data).slice()); }
  close(code, reason) { assert.ok(code===1000 || (code>=3000 && code<=4999),'Browser client close code');this.closed={code,reason};if(this.onclose)this.onclose({code,reason}); }
  control(data) { this.onmessage({data:JSON.stringify(data)}); }
}
async function ready() {
  const pending=openVncChannel('ws://localhost/vnc?port=5900',uuid,'test-credential',Socket);
  const socket=Socket.last;socket.onopen();
  assert.equal(socket.sent.length,1);
  assert.deepEqual(JSON.parse(socket.sent[0]),{type:'auth',token:'test-credential',uuid});
  socket.control({type:'auth_ok'});
  return {channel:await pending,socket};
}
test('RFB binary arrives before ACK; control never reaches RFB',async()=>{
  const {channel,socket}=await ready();const seen=[];
  channel.onmessage=e=>seen.push([...new Uint8Array(e.data)]);
  socket.onmessage({data:new Uint8Array([0,255,3]).buffer});
  assert.equal(socket.sent.length,1);
  socket.control({type:'checkpoint',nonce,sequence:'1'});
  assert.deepEqual(seen,[[0,255,3]]);
  assert.deepEqual(JSON.parse(socket.sent[1]),{type:'ack',nonce,sequence:'1'});
  channel.close();
});
test('receiver absent retains bounded bytes and withholds ACK',async()=>{
  const {channel,socket}=await ready();
  socket.onmessage({data:new Uint8Array([7]).buffer});
  socket.control({type:'checkpoint',nonce,sequence:'1'});
  assert.equal(socket.sent.length,1);
  let received=0;channel.onmessage=e=>{received+=e.data.byteLength;};await Promise.resolve();
  assert.equal(received,1);assert.equal(socket.sent.length,2);channel.close();
});
test('large input is split into at most 32KiB frames preserving bytes',async()=>{
  const {channel,socket}=await ready();const input=new Uint8Array(100000);input.fill(31);channel.send(input);
  const frames=socket.sent.slice(1);assert.equal(frames.length,4);
  assert.ok(frames.every(f=>f.length<=32768));assert.equal(frames.reduce((n,f)=>n+f.length,0),100000);
  assert.ok(frames.every(f=>f.every(b=>b===31)));channel.close();
});
test('input cap closes before partial application send',async()=>{
  const {channel,socket}=await ready();socket.bufferedAmount=4*1024*1024-1;
  channel.send(new Uint8Array(2));assert.equal(socket.sent.length,1);assert.equal(socket.closed.code,4008);
});
test('receive cap closes a peer before receiver attaches',async()=>{
  const {channel,socket}=await ready();socket.onmessage({data:new ArrayBuffer(256*1024+1)});
  assert.equal(socket.closed.code,4008);assert.equal(channel.readyState,3);
});
test('repeated or cross-session checkpoint never gets credit',async()=>{
  const {channel,socket}=await ready();channel.onmessage=()=>{};
  socket.control({type:'checkpoint',nonce,sequence:'1'});
  const count=socket.sent.length;socket.control({type:'checkpoint',nonce,sequence:'1'});
  assert.equal(socket.sent.length,count);assert.equal(socket.closed.code,4008);
});
test('unexpected text and pre-auth binary fail closed',async()=>{
  const pending=openVncChannel('ws://localhost/vnc?port=5900',uuid,'test-credential',Socket);
  const socket=Socket.last;socket.onopen();socket.onmessage({data:new ArrayBuffer(1)});
  await assert.rejects(pending,/receive limit/);assert.equal(socket.closed.code,4008);
});
test('wrong subprotocol cannot send credentials',async()=>{
  const pending=openVncChannel('ws://localhost/vnc?port=5900',uuid,'test-credential',Socket);
  const socket=Socket.last;socket.protocol='legacy';socket.onopen();
  await assert.rejects(pending,/protocol required/);assert.equal(socket.sent.length,0);
});

                                                             
test('abort pending authentication closes transport without waiting for server',async()=>{
  const cancel=new AbortController();
  const pending=openVncChannel('ws://localhost/vnc?port=5900',uuid,'test-credential',Socket,cancel.signal);
  const socket=Socket.last;socket.onopen();cancel.abort();
  await assert.rejects(pending,/cancelled/);assert.ok(socket.closed);
  socket.control({type:'auth_ok'});assert.equal(socket.sent.length,1);
});
test('pre-aborted request never creates a socket',async()=>{
  const cancel=new AbortController();cancel.abort();const prior=Socket.last;
  await assert.rejects(openVncChannel('ws://localhost/vnc?port=5900',uuid,'test-credential',Socket,cancel.signal),/cancelled/);
  assert.equal(Socket.last,prior);
});
