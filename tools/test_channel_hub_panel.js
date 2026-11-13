/* Production render/command functions, with a deterministic DOM/HTTP substitute. */
const fs=require('node:fs'),path=require('node:path'),vm=require('node:vm'),assert=require('node:assert/strict');
const html=fs.readFileSync(path.join(__dirname,'../firmware/main/panel_index.html'),'utf8');
const source=[...html.matchAll(/<script\b[^>]*>([\s\S]*?)<\/script>/g)].map(m=>m[1]).join('\n');
function part(start,end){const a=source.indexOf(start),b=source.indexOf(end,a+start.length);assert(a>=0&&b>a,start);return source.slice(a,b);}
const els=new Map([...html.matchAll(/\bid="([^"]+)"/g)].map(m=>[m[1],{
 value:'',textContent:'',innerHTML:'',disabled:false,style:{},dataset:{},classList:{toggle(){}},addEventListener(){},setAttribute(){},removeAttribute(){}
}]));
const logs=[],requests=[],timers=[];let respond;
const context=vm.createContext({window:{},document:{activeElement:null,addEventListener(){}},$:id=>{assert(els.has(id),id);return els.get(id);},
 rxGuardViews:[],ensureRxGuardViews(){},log:(...v)=>logs.push(v),last:'',refresh(){},
 fetch:(url,options)=>{requests.push(url);return new Promise((resolve,reject)=>{
  respond=j=>resolve({json:async()=>j});options?.signal?.addEventListener('abort',()=>{const e=Error('timeout');e.name='AbortError';reject(e);});
 });},AbortController,clearTimeout(){},
 setTimeout:fn=>timers.push(fn),escHtml:v=>String(v).replaceAll('&','&amp;').replaceAll('<','&lt;').replaceAll('>','&gt;').replaceAll('"','&quot;')});
vm.runInContext(part('const panelDirty = new Map();',"$('spkVol').addEventListener"),context);
vm.runInContext(part('function radioDisplayFrequency(', 'function radioFrequency('),context);
vm.runInContext(part('function syncBridge(','function brToggle('),context);
vm.runInContext(part('function renderNp2p(','const CH_MAX'),context);
const status={wifi:{connected:true},intercom:{room:1000,mode:1},freq:{rx:433.5,tx:433.5},bridge:{en:1,chan:1,active:1}};
(async()=>{
 const first=context.cmd('bridge_chan','2');
 context.syncBridge(status);assert.equal(els.get('brChan1').disabled,true);
 const second=await context.cmd('bridge_chan','3');assert.equal(second.ok,false);assert.equal(requests.length,1);
 respond({ok:false,err:'发射或尾音尚未结束，请松开 PTT 后重试'});await first;
 context.document.activeElement=els.get('brChan1');els.get('brChan1').value='2';
 context.syncBridge(status);assert.equal(els.get('brChan1').value,'1');assert.equal(els.get('brChan1').disabled,false);
 assert(logs.some(v=>v[0].includes('发射或尾音尚未结束')));assert.match(els.get('brS1').textContent,/433.5000/);
 status.np2p={pub:'0.0.0.0',port:0,hub:false,hub_role:'member',hub_conflict:false,stun_age_s:-1,peer_limit:16,peer_count:2,peers:[
  {name:'A',ip:'2001:db8::1',port:54321,state:'linked',hub:true},
  {name:'C',ip:'',port:0,state:'linked',route:'hub',via:'A<script>'}
 ]};
 context.renderNp2p(status);assert.equal(els.get('npPub').textContent,'尚未取得公网 IPv4');
 assert(!els.get('npPeerList').innerHTML.includes('0.0.0.0'));
 assert.match(els.get('npPeerList').innerHTML,/\[2001:db8::1\]:54321/);
 assert.match(els.get('npPeerList').innerHTML,/经 A&lt;script&gt; 转发/);
 assert(!els.get('npPeerList').innerHTML.includes('data-np-knock="1"'));
 assert.match(els.get('npState1').textContent,/直连 1\/1 · 转发成员 1/);
 status.np2p.pub='192.0.2.1';status.np2p.port=1234;status.np2p.stun_age_s=40;
 status.np2p.hub=true;status.np2p.hub_conflict=true;
 context.renderNp2p(status);assert.equal(els.get('npPub').textContent,'192.0.2.1:1234');
 assert.match(els.get('npNat').textContent,/地址可能过期/);assert.match(els.get('npDiag1').textContent,/只保留一台/);
 context.renderNp2p({});assert.match(els.get('npPub').textContent,/暂不可用/);
 const hanging=context.cmd('bridge_chan','2');timers.at(-1)();await hanging;
 context.syncBridge(status);assert.equal(els.get('brChan1').disabled,false);
 assert(logs.some(v=>v[0].includes('切换请求超时')));
 console.log('PASS: serialized channel command, truthful rejection/reset while focused, actual RF display; unresolved/stale/missing public IP, IPv6, indirect member path/no-reconnect, escaped hub name, role/conflict and direct-only counts');
})().catch(e=>{console.error(e);process.exitCode=1;});
