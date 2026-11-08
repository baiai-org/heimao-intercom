/* Production shared-name UI with local fetch/DOM fakes. */
const fs=require('node:fs'),vm=require('node:vm'),assert=require('node:assert/strict');
const source=fs.readFileSync(require('node:path').join(__dirname,'../firmware/main/device_name.js'),'utf8');
const elements=new Map();
function el(id){if(!elements.has(id))elements.set(id,{textContent:'',value:'',disabled:false,listeners:{},addEventListener(k,f){this.listeners[k]=f}});return elements.get(id)}
let state={device_id:'orl-0123456789ab',custom_name:'车载主台',name_revision:0};
let fail=false,posts=0;
const ctx={document:{getElementById:el,title:''},window:{ORLControl:{identity:Promise.resolve({...state})}},location:{host:'test.local'},TextEncoder,
 fetch:async(url,options={})=>{
   if(options.method==='POST') {
     posts++;const request=JSON.parse(options.body);
     if(fail)return{ok:false,status:500,json:async()=>({ok:false,err:'storage unavailable'})};
     if(request.name_revision!==state.name_revision)return{ok:false,status:409,json:async()=>({ok:false})};
     state={...state,custom_name:request.custom_name,name_revision:state.name_revision+1};
     return{ok:true,status:200,json:async()=>({...state,ok:true})};
   }
   return{ok:true,status:200,json:async()=>({...state})};
 }
};
vm.runInNewContext(source,ctx);
const flush=async()=>{for(let i=0;i<20;i++)await Promise.resolve()};
async function edit(name){el('handsetNameInput').value=name;el('handsetNameInput').listeners.input();await el('handsetNameSave').listeners.click()}
(async()=>{
 await flush();assert.equal(el('handsetName').textContent,'车载主台');assert(ctx.document.title.includes('车载主台'));
 ctx.window.HMDeviceName.accept({...state,ptt:'IDLE',fp:{net_tx:1}});assert(el('handsetUse').textContent.includes('发话中'));
 ctx.window.HMDeviceName.accept({...state,ptt:'IDLE',fp:{rf_rx:1}});assert(el('handsetUse').textContent.includes('接收中'));
 ctx.window.HMDeviceName.accept({...state,ptt:'FAULT'});assert(el('handsetUse').textContent.includes('故障'));
 await edit('  <台> " & \\  ');assert.equal(state.custom_name,'<台> " & \\');assert.equal(el('handsetName').textContent,state.custom_name);
 el('handsetNameInput').value='my pending name';el('handsetNameInput').listeners.input();
 state={...state,custom_name:'网页外改名',name_revision:state.name_revision+1};ctx.window.HMDeviceName.accept({...state,ptt:'IDLE'});
 assert.equal(el('handsetNameInput').value,'my pending name');
 await el('handsetNameSave').listeners.click();assert.equal(state.custom_name,'网页外改名');assert(el('handsetNameResult').textContent.includes('核对'));
 await el('handsetNameSave').listeners.click();assert.equal(state.custom_name,'my pending name');
 fail=true;await edit('failed name');assert.equal(state.custom_name,'my pending name');assert(el('handsetNameResult').textContent.includes('storage'));
 fail=false;await edit('');assert.equal(el('handsetName').textContent,'黑猫智能手台-6789AB');
 const before=posts;await edit('名'.repeat(25));assert.equal(posts,before);
 ctx.window.HMDeviceName.accept({device_id:'other',custom_name:'wrong',name_revision:999});assert.equal(el('handsetName').textContent,'黑猫智能手台-6789AB');
 console.log('PASS: production name display/title, escaped text, dirty edits, revision conflicts, failed save, reset, limits and identity isolation');
})().catch(e=>{console.error(e);process.exitCode=1});
