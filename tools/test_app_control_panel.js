/* Execute the embedded lease client. No board, audio or RF is accessed. */
const assert=require('node:assert/strict'),fs=require('node:fs'),vm=require('node:vm');
const {webcrypto}=require('node:crypto');
const calls=[],intervals=[],listeners={},events=[];
let occupied=false,denyNext=false,claimCount=0,current='';
function element(){return {style:{},dataset:{},children:[],append(...x){this.children.push(...x)},textContent:'',hidden:false};}
const body=element();body.prepend=x=>{body.children.unshift(x)};
const context={URL,Headers,crypto:webcrypto,Event:class{constructor(type){this.type=type}},console,
 location:{href:'http://192.0.2.13/',origin:'http://192.0.2.13'},
 document:{createElement:element,body},setInterval:fn=>intervals.push(fn)};
context.window=context;context.addEventListener=(name,fn)=>{listeners[name]=fn};
context.dispatchEvent=e=>events.push(e.type);context.confirm=()=>true;
context.WebSocket=class{constructor(url){this.url=url}};
context.fetch=async(url,options={})=>{
 const path=new URL(url,context.location.href).pathname;calls.push({path,url,options});
 let data={ok:true},status=200;
 if(path==='/api/device')data={device_id:'orl-test',api_version:2};
 else if(path==='/api/control'&&options.method==='POST'){
  const request=JSON.parse(options.body);
  if(request.op==='claim'&&occupied){data={ok:false,held:true,mine:false,label:'Other phone'};status=409}
  else if(request.op==='claim'||request.op==='takeover'){
   ++claimCount;current='a'.repeat(64);data={ok:true,device_id:'orl-test',token:current,generation:claimCount};occupied=true;
  }else if(request.op==='release'){occupied=false;current='';data={ok:true,device_id:'orl-test'}}
  else data={ok:true,device_id:'orl-test'};
 }else if(path==='/api/control')data={held:occupied,label:'Other phone'};
 else if(denyNext){denyNext=false;status=423;data={ok:false,err:'expired'};}
 return {ok:status<400,status,json:async()=>data};
};
vm.createContext(context);
vm.runInContext(fs.readFileSync(require('node:path').join(__dirname,'../firmware/main/app_control.js'),'utf8'),context);
(async()=>{
 await context.ORLControl.identity;assert.equal(claimCount,0);
 await context.fetch('/api/status');await intervals[0]();assert.equal(claimCount,0);
 await Promise.all([context.fetch('/api/set?k=invalid&v=0'),context.fetch('/api/channel?op=invalid')]);
 assert.equal(claimCount,1,'concurrent actions share one acquisition');
 const writes=calls.filter(c=>c.path==='/api/set'||c.path==='/api/channel');
 for(const c of writes){assert.equal(c.options.headers.get('X-ORL-Control'),current);assert.equal(c.options.headers.get('X-ORL-Device'),'orl-test');}
 const ws=new context.WebSocket('ws://192.0.2.13/ws/sdr?sid=1');
 assert.equal(new URL(ws.url).searchParams.get('control'),current);
 const prior=calls.length;denyNext=true;await context.fetch('/api/set?k=invalid&v=1');
 assert.equal(calls.length,prior+1,'a rejected write must never be replayed');
 assert.equal(context.ORLControl.token,'');assert(events.includes('orl-control-lost'));
 await context.fetch('/api/sdr?on=0&sid=1');assert.equal(claimCount,1,'late release must not acquire a fresh lease');
 await assert.rejects(context.fetch('/api/set?k=invalid&v=2'),/Other phone/);
 assert.equal(claimCount,1);
 const bar=body.children[0],acquire=bar.children.find(x=>x.textContent==='请求接管');
 assert(acquire);await acquire.onclick();assert.equal(claimCount,2,'only explicit takeover can replace the owner');
 const headers={};await context.ORLControl.authorizeXhr({setRequestHeader:(k,v)=>headers[k]=v});
 assert.equal(headers['X-ORL-Control'],current);
 listeners.pagehide();await new Promise(resolve=>setImmediate(resolve));
 assert.equal(JSON.parse(calls.at(-1).options.body).op,'release');assert.equal(calls.at(-1).options.keepalive,true);
 console.log('PASS: embedded control client read-only startup, single acquisition, request headers, WS lease, no write replay, stale release, explicit takeover, OTA authorization and page cleanup');
})().catch(e=>{console.error(e);process.exitCode=1});
