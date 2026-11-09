                                                                  
                                                                    
                                                         
import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';
import test from 'node:test';
const source=fs.readFileSync(new URL('../../ui/modules/vm-console.js',import.meta.url),'utf8');
const first='11111111-1111-1111-1111-111111111111',second='22222222-2222-2222-2222-222222222222';
function fixture(deferred=false){
 const nodes=new Map(),scripts=[],sessions=[],channels=[],observers=[];
 class Element{
  constructor(tag,props={}){this.tag=tag;this.style={};this.dataset={};this.isConnected=true;this.children=[];Object.assign(this,props);if(this.id)nodes.set(this.id,this);}
  appendChild(node){this.children.push(node);return node;}
  replaceChildren(...nodes){this.children=nodes;}
  remove(){this.isConnected=false;if(nodes.get(this.id)===this)nodes.delete(this.id);}
 }
 new Element('div',{id:'vnc-frame'});
 const el=(tag,props,...children)=>{const n=new Element(tag,props||{});children.forEach(x=>n.appendChild(x));return n;};
 const clearEl=node=>{function remove(n){if(n instanceof Element){n.children.forEach(remove);n.remove();}}node.children.forEach(remove);node.children=[];};
 const document={getElementById:id=>nodes.get(id)||null,createElement:tag=>new Element(tag),addEventListener(){},querySelector:()=>scripts.find(s=>s.isConnected&&s.dataset.vncLoader)||null,body:new Element('body'),head:{appendChild(s){scripts.push(s);nodes.set(s.id,s);}}};
 class RFB{constructor(container,channel){this.container=container;this.channel=channel;sessions.push(this);}disconnect(){this.closed=true;this.channel.close();}addEventListener(){}}
 class Observer{constructor(callback){this.callback=callback;observers.push(this);}observe(){}disconnect(){this.closed=true;}}
 const context={document,location:{protocol:'https:',host:'example'},console,MutationObserver:Observer,AbortController,WebSocket:class{},connectWS(){},t:x=>x,escapeHtml:String,escapeAttr:String,window:{authToken:'fixture',PCV:{uxlib:{el,clearEl}}},MockRFB:RFB,openVncChannel:(url,uuid,token,WS,signal)=>{
  const channel={url,uuid,signal,close(){this.closed=true;}};channels.push(channel);
  return deferred?new Promise(resolve=>{channel.resolve=()=>resolve(channel);}):Promise.resolve(channel);
 }};
 vm.createContext(context);vm.runInContext(source,context);
 const evaluate=s=>vm.runInContext('(async()=>{const _mod=MockRFB;'+s.textContent.replace(/^import .*;\n/gm,'')+'})()',context);
 return{context,nodes,scripts,sessions,channels,observers,evaluate,open:uuid=>context.window.openNoVNC('localhost',5900,uuid)};
}
test('delayed imports open only the latest UUID on its original container',async()=>{
 const f=fixture();f.open(first);f.open(second);
 for(const script of f.scripts)await f.evaluate(script);
 assert.equal(f.sessions.length,1);assert.equal(f.sessions[0].channel.uuid,second);
 assert.equal(f.sessions[0].container,f.nodes.get('vnc-container'));
 assert.equal(f.context.window._vncRfb,f.sessions[0]);
});
test('late authentication cannot attach to a replacement view even if adapter resolves',async()=>{
 const f=fixture(true);f.open(first);const old=f.evaluate(f.scripts[0]);await Promise.resolve();
 assert.equal(f.channels.length,1);const oldContainer=f.nodes.get('vnc-container');
 f.open(second);assert.ok(f.channels[0].signal.aborted);const next=f.evaluate(f.scripts[1]);
 f.channels[0].resolve();await old;assert.ok(f.channels[0].closed);assert.equal(f.sessions.length,0);
 f.channels[1].resolve();await next;assert.equal(f.sessions.length,1);
 assert.notEqual(f.sessions[0].container,oldContainer);assert.equal(f.sessions[0].channel.uuid,second);
});
test('route departure aborts pending authentication before an RFB exists',async()=>{
 const f=fixture(true);f.open(first);const pending=f.evaluate(f.scripts[0]);await Promise.resolve();
 f.nodes.get('vnc-container').remove();for(const observer of f.observers)observer.callback();
 assert.ok(f.channels[0].signal.aborted);f.channels[0].resolve();await pending;
 assert.equal(f.sessions.length,0);assert.ok(f.channels[0].closed);
});
