const {chromium}=require(process.env.PLAYWRIGHT_MODULE || 'playwright');
const fs=require('node:fs'),path=require('node:path'),assert=require('node:assert/strict');
const main=path.resolve(__dirname,'../firmware/main');
const output=process.env.PANEL_TEST_OUTPUT || __dirname;fs.mkdirSync(output,{recursive:true});
const identity={device_id:'orl-example',api_version:2,custom_name:'桥接测试',name_revision:1};
const originalIdentity=identity.device_id;
const eqSlots = [null,null,null]; let eqRevision=0, eqConflict=false;
let failTailUpload=false, tailUploads=0, holdTailUpload=null;
const status={...identity,build:{version:'r11-110'},wifi:{connected:true,ip:'192.0.2.1',ssid:'测试网络',state:'connected',rssi:-45,host:'heimao',mdns_host:'heimao-2',mdns_state:'conflict',ip6:''},
 mode:'MONITOR',ptt:'IDLE',rssi:80,peak:200,mod_sq:true,sql_open:true,eq_spk:[0,0,0,0,0,0],eq_mic:[0,0,0,0,0,0],
 clip:0,ovf:0,underruns:0,gain:96,txgain:36,ntxgain:254,bgtx:16,cvol:80,spk:5,rxnetgain:16,txpre:1,tx_count:0,uptime:120,heap:50000,audio_rate:16000,
 freq:{rx:433.5,tx:433.5,bw:0,sq:3,vol:5,ctcss_tx:'0000',ctcss_rx:'0000',power:0},
 intercom:{mode:1,room:1000,state:'idle',peers:1,tx:0,rx:0,broker:'mqtt.example.test',diag:'',user:'',pass:0},
 bridge:{en:1,chan:1,chan_name:'测试信道',active:1,keyed:0},tail:{id:0,custom_s:0,modes:7},
 rx_guard:{version:1,p2p:{en:1,ms:1234,sources:7,outputs:7},relay:{en:0,ms:2000,sources:7,outputs:7},blocked:7,remaining_ms:1234}};
const denied=[];let failPolicySave=false;
status.np2p={access_version:1,access_ready:true,denied_count:0,denied_limit:64,pub:'192.0.2.10',port:7821,nat:'not-port-preserving',stun_age_s:14,rtt_ms:51,hub:false,peer_limit:16,peer_count:16,
 peers:Array.from({length:16},(_,i)=>({name:'p'+i,ip:i<8?'192.0.2.'+(i+20):'2001:db8::'+i,port:54321+i,state:'linked',rtt_ms:30,rx:100,tx:100,drop:0}))};
let rejectChannel=false,refuseStop=false,stops=0;
let failDenoiseSave=false, denoiseWrites=0, failDenoiseRead=false, holdDenoiseRead=null, loseDenoiseReply=false;
const idleDenoise={state:0,level:0,frames:0,max_us:0,overruns:0,wait_max_us:0,
 input_frames:0,wet_frames:0,dry_frames:0,queue_full:0,recoveries:0};
const denoise={ok:true,levels:0,selection_level:2,active_mode:0,...idleDenoise,
 speaker_stats:{...idleDenoise},handset_stats:{...idleDenoise},bridge_stats:{...idleDenoise},
 i2s:{receive_stalls:0,short_writes:0,errors:0},
 network_stats:[{...idleDenoise},{...idleDenoise},{...idleDenoise}]};
const packLevel=(setting,level)=>level << ((setting-1)*3);
const getLevel=setting=>(denoise.levels >> ((setting-1)*3)) & 7;
const putLevel=(setting,level)=>{denoise.levels=(denoise.levels & ~packLevel(setting,7)) | packLevel(setting,level);};
const settings=Array.from({length:10},(_,i)=>i+1);
const configLevels=(level,paths)=>settings.reduce((values,i)=>values | (paths & (1<<i) ? packLevel(i,level) : 0),0);
denoise.levels=configLevels(2,0x7fe);
const tailState=()=>{
 const {tail,intercom:{mode}}=status;
 return {...tail,mode,enabled:!!(tail.id && (tail.modes & (1<<mode))),
  custom_play_ms:Math.round(tail.custom_s*1000)};
};
const channels=[0,1,2].map(i=>({i,name:'测试信道'+(i+1),rx:433.5+i*.0125,tx:433.5+i*.0125,bw:0,sq:3,vol:5,pwr:0,band:'UHF',compatible:true}));
const radio={freq_grid_hz:500,freq_decimals:4,band:'UHF',min:400,max:480,model:'SA818S-U',compatible:true,ready:true,power_applied:0,power_pending:0};

async function assertEqSlotLayout(page, columns, filled) {
 const list=page.locator('#eqPresetList');
 await list.waitFor();
 await page.waitForFunction(expected=>{
  const slots=[...document.querySelectorAll('#eqPresetList .eq-user-slot')];
  return slots.length===3 && slots.filter(slot=>!slot.querySelector('.eq-user-more').hidden).length===expected &&
      slots.every(slot=>slot.querySelector('strong').textContent.includes(' · '));
 },filled);
 const {bounds,slots}=await list.evaluate(element=>({
  bounds:element.getBoundingClientRect().toJSON(),
  slots:[...element.children].map(slot=>({
   ...slot.getBoundingClientRect().toJSON(),
   nameWidth:slot.querySelector('strong').getBoundingClientRect().width,
   actionsRight:slot.querySelector('.eq-user-actions').getBoundingClientRect().right
  }))
 }));
 for(let index=0;index<slots.length;index++){
  const slot=slots[index];
  assert(slot.nameWidth>0 && slot.actionsRight<=slot.right+1, 'A slot must keep its name and actions inside the card');
  if(columns===3){
   assert(Math.abs(slot.top-slots[0].top)<1 && Math.abs(slot.width-slots[0].width)<1);
   if(index>0)assert(slot.left>=slots[index-1].right);
  }else{
   assert(Math.abs(slot.left-bounds.left)<1 && Math.abs(slot.width-bounds.width)<1);
   if(index>0)assert(slot.top>=slots[index-1].bottom);
  }
 }
 assert(Math.abs(slots[0].left-bounds.left)<1 && Math.abs(slots[2].right-bounds.right)<1);
}

(async()=>{
 const browser=await chromium.launch({executablePath:process.env.CHROME_PATH || 'C:/Program Files/Google/Chrome/Application/chrome.exe',headless:true});
 try{
  const page=await browser.newPage({viewport:{width:1280,height:1000}}),errors=[],writes=[];
  page.on('pageerror',e=>errors.push(e.message));
  page.on('dialog',d=>d.accept());
  await page.route('**/*',async route=>{
   const url=new URL(route.request().url());
   if(url.hostname!=='heimao.test')return route.abort();
   const p=url.pathname;let json;
   if(p==='/')return route.fulfill({contentType:'text/html; charset=utf-8',body:fs.readFileSync(path.join(main,'panel_index.html'))});
   if(p==='/setup')return route.fulfill({contentType:'text/html; charset=utf-8',body:fs.readFileSync(path.join(main,'setup.html'))});
   const scripts={'/app-control.js':'app_control.js','/device-name.js':'device_name.js','/rx-guard.js':'rx_guard.js','/rf-denoise.js':'rf_denoise.js','/eq-presets.js':'eq_presets.js'};
   if(scripts[p])return route.fulfill({contentType:'application/javascript',body:fs.readFileSync(path.join(main,scripts[p]))});
   if(p==='/api/device')json=identity;
   else if(p==='/api/wifi')json={...status.wifi,profiles:[],scan_running:false,offline_ms:0};
   else if(p==='/api/scan')json=[];
   else if(p==='/api/talk'){assert.equal(url.searchParams.get('on'),'0');stops++;if(!refuseStop)status.ptt="IDLE";json={ok:!refuseStop};}
   else if(p==='/api/sdr_ptt'){assert.equal(url.searchParams.get('on'),'0');json={ok:true,sid:Number(url.searchParams.get('sid')),tx:Number(url.searchParams.get('tx')),id:Number(url.searchParams.get('id')),ptt:refuseStop};}
    else if(p==='/api/status')json={...status,tail:tailState()};
    else if(p==='/api/tail_upload'){
     assert.equal(url.searchParams.get('activate'),'1');
     assert.equal(route.request().method(),'POST');
     assert.equal(route.request().headers()['x-orl-device'],identity.device_id);
     assert.equal(route.request().postDataBuffer().toString(),'test audio');
     tailUploads++;
     if(holdTailUpload)await holdTailUpload;
     if(failTailUpload)json={ok:false,err:'存储失败'};
     else {
      status.tail.id=8;status.tail.custom_s=0.8;
      json={ok:true,device_id:identity.device_id,tail:tailState()};
     }
    }
   else if(p==='/api/eq_presets'){
    if(route.request().method()==='POST'){
     assert.equal(route.request().headers()['x-orl-device'],identity.device_id);
     const body=route.request().postDataJSON();
     assert.equal(body.boot_id,'1234abcd');
     const index=body.slot-1;
     if(eqConflict){eqConflict=false;eqRevision++;}
     if(body.revision!==eqRevision)return route.fulfill({status:409,json:{ok:false,err:'版本冲突'}});
     if(body.op==='save'){eqSlots[index]={name:body.name,spk:body.spk,mic:body.mic};eqRevision++;}
     else if(body.op==='rename'){eqSlots[index].name=body.name;eqRevision++;}
     else if(body.op==='delete'){eqSlots[index]=null;eqRevision++;}
     else if(body.op==='apply'){
      assert.deepEqual(body.expected_spk,status.eq_spk);
      assert.deepEqual(body.expected_mic,status.eq_mic);
      status.eq_spk=eqSlots[index].spk;status.eq_mic=eqSlots[index].mic;
     }else throw Error('Unexpected EQ mutation '+body.op);
    }
    json={ok:true,device_id:identity.device_id,boot_id:'1234abcd',revision:eqRevision,slots:eqSlots,
     active_spk:status.eq_spk,active_mic:status.eq_mic};
   }
   else if(p==='/api/rf_denoise'){
    if(route.request().method()==='POST'){
     assert.equal(route.request().headers()['x-orl-device'],identity.device_id);
     const {selection_level,paths,expected_levels,expected_selection_level}=route.request().postDataJSON();
     assert.equal(paths & ~0x7fe,0);
     assert(selection_level>=1 && selection_level<=4);
     denoiseWrites++;
     if(failDenoiseSave)return route.fulfill({status:503,json:{ok:false}});
     const levels=configLevels(selection_level,paths);
     if((denoise.levels!==levels || denoise.selection_level!==selection_level) &&
         (denoise.levels!==expected_levels || denoise.selection_level!==expected_selection_level)) {
      return route.fulfill({status:409,json:{ok:false}});
     }
     Object.assign(denoise,{levels,selection_level});
     if(loseDenoiseReply)return route.abort('timedout');
    }
    json={...denoise,flags:settings.reduce((bits,i)=>bits | (getLevel(i)?1<<i:0),0),
     mode:status.intercom.mode,rf_tx:!!status.rf_tx,bridge_active:!!status.bridge.active,
     bridge_keyed:!!status.bridge.keyed,device_id:identity.device_id};
    if(route.request().method()==='GET'){
     if(failDenoiseRead)return route.fulfill({status:503,json:{ok:false}});
     if(holdDenoiseRead)await holdDenoiseRead;
    }
   }
   else if(p==='/api/np2p/denied')json={ready:true,count:denied.length,limit:64,entries:denied};
   else if(p==='/api/radio')json=radio;
   else if(p==='/api/live')json={ptt:'IDLE',rf_key:0,rf_tx:0,ic_mode:status.intercom.mode,rssi:80,peak:200,mod_sq:1};
   else if(p==='/api/channels')json=channels;
   else if(['/api/ctcss','/api/dcs'].includes(p))json=[];
   else if(p==='/api/set'){
     const {k,v}=route.request().method()==='POST' ? route.request().postDataJSON() :
      {k:url.searchParams.get('k'),v:url.searchParams.get('v')};
     writes.push({k,v});
     if(k==='tail' || k==='tail_modes'){
      assert.equal(route.request().headers()['x-orl-device'],identity.device_id);
      if(k==='tail')status.tail.id=Number(v);
      else status.tail.modes=Number(v);
      return route.fulfill({json:{ok:true,device_id:identity.device_id,tail:tailState()}});
     }else if(k==='ix_mode')status.intercom.mode=Number(v);
    else if(k==='spk_vol')status.spk=Number(v);
    else if(k==='freq_pair'){
     if(rejectChannel)return route.fulfill({contentType:'application/json',body:JSON.stringify({ok:false,err:'模块拒绝频率或响应超时，请重新提交可用频率'})});
     [status.freq.tx,status.freq.rx]=v.split(',').map(Number);
    }
    else if(k==='bridge_chan'){
     if(rejectChannel)return route.fulfill({contentType:'application/json',body:JSON.stringify({ok:false,err:'发射或尾音尚未结束，请松开 PTT 后重试'})});
     status.bridge.chan=Number(v);status.bridge.chan_name=channels[Number(v)-1].name;
     status.freq.rx=status.freq.tx=channels[Number(v)-1].rx;
    }
    else if(['rx_guard_p2p','rx_guard_relay'].includes(k)){
     const [en,ms,sources,outputs]=v.split(',').map(Number);status.rx_guard[k==='rx_guard_p2p'?'p2p':'relay']={en,ms,sources,outputs};
    }else if(k==='np2p_unblock'){
     const index=denied.findIndex(p=>p.id===v);if(index>=0)denied.splice(index,1);status.np2p.denied_count=denied.length;
    }else if(k==='np2p_peer_del'){
     if(failPolicySave)return route.fulfill({contentType:'application/json',body:JSON.stringify({ok:false,err:'删除与拒绝记录保存失败，未完成删除，请重试'})});
     const peer=status.np2p.peers[Number(v)];denied.push({...peer,id:'m-020304050607',identity:true});status.np2p.denied_count=denied.length;
     status.np2p.peers.splice(Number(v),1);status.np2p.peer_count=status.np2p.peers.length;
    }else if(k==='np2p_peer_add'||k==='np2p_peer_restore'){
     if(status.np2p.peers.length>=16)return route.fulfill({contentType:'application/json',body:JSON.stringify({ok:false,err:'对端已满（最多16个），请先删除不再使用的对端'})});
     status.np2p.peers.push({name:'extra',ip:'192.0.2.99',port:54321,state:'linked',rtt_ms:30,rx:0,tx:0,drop:0});status.np2p.peer_count=status.np2p.peers.length;
    }else throw Error('Unexpected mutation '+k);
    json={ok:true};
   }else if(p.endsWith('.png'))return route.fulfill({contentType:'image/png',body:fs.readFileSync(path.join(main,'brand_icon.png'))});
   else return route.fulfill({status:404,body:'not mocked'});
   return route.fulfill({contentType:'application/json',body:JSON.stringify(json)});
  });
  await page.goto('http://heimao.test/');
  await page.waitForFunction(()=>window._st&&document.querySelector('#rxGuard1 [data-rg="form"]').hidden===false);
  assert.deepEqual(await page.locator('#lanPill a').evaluateAll(links=>links.map(a=>a.href)),
    ['https://192.0.2.1/','https://heimao-2.local/']);

  const inventory=async()=>page.evaluate(()=>({
    width:innerWidth,height:innerHeight,pageHeight:document.documentElement.scrollHeight,
    inputs:document.querySelectorAll('input,select,textarea').length,buttons:document.querySelectorAll('button').length,
    groups:[...document.querySelectorAll('.hero-deck,.mobile-nav,.rack-grid>.card,.bottom-grid>.card')].map(e=>({name:e.querySelector('h2')?.innerText||e.className,y:Math.round(e.getBoundingClientRect().top+scrollY),height:Math.round(e.getBoundingClientRect().height)}))
  }));
  await page.setViewportSize({width:1440,height:900});
  assert.equal(await page.locator('[data-page-view]:visible').count(),1);
  assert.equal(await page.locator('[data-page-view=talk]').isVisible(),true);
  for(const id of ['network','bridge','radio','audio','device','talk']){
    await page.locator('.panel-nav [data-page='+id+']').click();
    assert.equal(await page.locator('[data-page-view='+id+']').isVisible(),true);
    assert.equal(await page.locator('[data-page-view]:visible').count(),1);
  }
  assert.equal(writes.length,0);
  await page.locator('[data-page=radio]').click();await page.locator('#ftx').fill('433.5025');
  await page.locator('[data-page=network]').click();await page.evaluate(()=>{holdSyncUntil=0;last='';return refresh();});
  await page.locator('[data-page=radio]').click();assert.equal(await page.locator('#ftx').inputValue(),'433.5025');
  await page.locator('[data-page=talk]').click();
  await page.locator('[data-page=radio]').click();await page.locator('#fset').click();
  await page.waitForFunction(()=>!panelDirty.has('ftx'));assert.equal(status.freq.tx,433.5025);
  await page.locator('[data-page=talk]').click();
  status.ptt='TX';await page.evaluate(()=>{SDR.on=true;txOn=true;txEnd=Date.now()+60000;talkToken=123;});refuseStop=true;
  await page.locator('[data-page=network]').click();assert(await page.locator('[data-page-view=talk]').isVisible());assert.equal(stops,1);
  await page.waitForFunction(()=>document.getElementById('panelNotice').textContent.includes('未确认'));refuseStop=false;await page.locator('[data-page=network]').click();await page.waitForFunction(()=>!document.querySelector('[data-page-view=network]').hidden);assert.equal(stops,2);
  assert(await page.evaluate(()=>SDR.on));await page.evaluate(()=>{SDR.on=false;});
  await page.locator('[data-page=talk]').click();await page.locator('[data-source=browser]').click();
  assert(await page.locator('#sdrPtt').isVisible());
  await page.evaluate(()=>{SDR.http=true;SDR.on=true;SDR.sid=99;SDR.activeTx=3;SDR.ptt=SDR.keyed=true;});refuseStop=true;
  await page.locator('[data-page=network]').click();
  await page.waitForFunction(()=>document.getElementById('panelNotice').textContent.includes('未确认停止浏览器'));
  assert(await page.locator('[data-page-view=talk]').isVisible());
  const retry=page.getByRole('button',{name:'重试停止浏览器发射',exact:true});await retry.click();
  await page.waitForFunction(()=>document.getElementById('panelNotice').textContent.includes('停止尚未确认'));
  assert(await retry.isVisible());assert.equal(await page.evaluate(()=>SDR.releasePending),false);
  refuseStop=false;await retry.click();await page.waitForFunction(()=>document.getElementById('panelNotice').textContent.includes('已确认停止'));
  await page.locator('[data-source=handheld]').click();await page.evaluate(()=>{SDR.on=false;SDR.http=false;});
  await page.locator('[data-page=bridge]').click();assert(await page.locator('#brT1').isVisible());assert(await page.locator('#rxGuard1').isVisible());
  for(const mode of [0,2,1]){status.intercom.mode=mode;await page.evaluate(()=>{holdSyncUntil=0;last='';return refresh();});
   assert.equal(await page.locator('#brT1').isVisible(),mode===1);assert.equal(await page.locator('#brT2').isVisible(),mode===2);}
  await page.locator('[data-page=audio]').click();
  const card=page.locator('#rfDenoiseCard');
  assert(await card.isVisible());
  assert(await page.locator('#eqReset').evaluate(el=>{
   const card=document.getElementById('rfDenoiseCard');
   return el.closest('.card').contains(card) && card.nextElementSibling.matches('.eq-Curve-box') &&
    card.classList.contains('subcard');
  }));
  await page.getByRole('button',{name:'尾音',exact:true}).click();
   const tailType=page.locator('#ixTailSel'), tailStatus=page.locator('#ixTailState');
   await tailType.selectOption('1');
   await page.waitForFunction(()=>document.querySelector('#ixTailState').textContent.includes('当前模式已启用'));
   await page.locator('#ixTailM1').click();
   await page.waitForFunction(()=>document.querySelector('#ixTailState').textContent.includes('当前模式未启用'));
   assert(!await page.locator('#ixTailM1').isChecked());
   assert.equal(status.tail.modes,5);
   await page.locator('#ixTailFile').setInputFiles({name:'tail.wav',mimeType:'audio/wav',buffer:Buffer.from('test audio')});
   // The selector stays on the board-confirmed tone during a delayed upload.
   let finishTailUpload;
   holdTailUpload=new Promise(resolve=>{finishTailUpload=resolve;});
   const upload=page.waitForRequest(req=>req.url().includes('/api/tail_upload?'));
   await page.getByRole('button',{name:'上传并启用',exact:true}).click();
   await upload;
   assert.equal(await tailType.inputValue(),'1');
   assert((await tailStatus.textContent()).includes('正在确认'));
   holdTailUpload=null;finishTailUpload();
   await page.waitForFunction(()=>document.querySelector('#ixTailSel').value==='8');
   assert((await tailStatus.textContent()).includes('当前模式未启用'));
   assert.equal(status.tail.modes,5);
   assert.equal(writes.filter(write=>write.k==='tail').length,1);
   await tailType.selectOption('1');
   await page.waitForFunction(()=>document.querySelector('#ixTailSel').value==='1');
   failTailUpload=true;
   await page.locator('#ixTailFile').setInputFiles({name:'next.wav',mimeType:'audio/wav',buffer:Buffer.from('test audio')});
   await page.getByRole('button',{name:'上传并启用',exact:true}).click();
   await page.waitForFunction(()=>document.querySelector('#ixTailState').textContent.includes('存储失败'));
   assert.equal(await tailType.inputValue(),'1');
   assert.equal(tailUploads,2);
   failTailUpload=false;
  await page.getByRole('button',{name:'调音台 EQ',exact:true}).click();
  const input=setting=>card.locator('[data-path="'+setting+'"]');
  const save=card.locator('[data-dn=save]'), undo=card.locator('[data-dn=undo]');
  const level=card.locator('[data-dn=level]');
  const saved=()=>page.waitForFunction(()=>document.querySelector('#rfDenoiseStatus').textContent.includes('已保存'));
  assert.equal(await card.locator('select:visible').count(),1);
  assert.equal(await card.locator('input:checked').count(),10);
  assert.deepEqual(await level.locator('option').allTextContents(),['轻度','标准','强力','极强']);
  assert.equal(await level.inputValue(),'2');
  assert(await save.isDisabled());
  let finishOldGet;
  holdDenoiseRead=new Promise(resolve=>{finishOldGet=resolve;});
  await page.waitForRequest(req=>req.url().endsWith('/api/rf_denoise')&&req.method()==='GET');
  await level.selectOption('4');
  await input(5).uncheck();
  await input(10).uncheck();
  assert(await card.locator('[data-dn=levelNote]').isVisible());
  assert.equal(denoiseWrites,0);
  await save.click();await saved();
  holdDenoiseRead=null;finishOldGet();
  assert.deepEqual(settings.map(getLevel),[4,4,4,4,0,4,4,4,4,0]);
  assert.equal(await level.inputValue(),'4');
  assert(!await input(5).isChecked()&&!await input(10).isChecked());
  Object.assign(denoise.network_stats[2],{state:2,level:3,frames:38,max_us:10099,overruns:1,
   wait_max_us:6120,input_frames:40,wet_frames:37,dry_frames:3,queue_full:0,recoveries:1});
  await card.getByText('运行详情',{exact:true}).click();
  await card.getByText(/网络来话 → 模拟桥接.*已保存 极强.*当前话段 强力.*输出降噪 37.*原声 3.*恢复 1.*排队最长 6120/).waitFor();
  assert(await card.getByText(/I2S 累计 · 接收欠载 0/).isVisible());
  await card.getByText('运行详情',{exact:true}).click();
  await card.screenshot({path:path.join(output,'denoise-desktop.png')});

  await level.selectOption('1');
  await input(10).check();
  status.intercom.mode=2;
  await page.locator('#eqTabMic').click();
  assert.equal(await card.locator('input:visible').count(),10);
  assert.equal(await level.inputValue(),'1');
  failDenoiseSave=true;
  await save.click();
  await page.waitForFunction(()=>document.querySelector('#rfDenoiseStatus').textContent.includes('核对后再保存'));
  assert.equal(await level.inputValue(),'1');
  assert(await input(10).isChecked());
  assert.equal(getLevel(10),0);
  failDenoiseSave=false;
  loseDenoiseReply=true;
  const writeCount=denoiseWrites;
  await save.click();await saved();loseDenoiseReply=false;
  assert.equal(denoiseWrites,writeCount+1);
  assert.deepEqual(settings.map(getLevel),[1,1,1,1,0,1,1,1,1,1]);

  await input(1).uncheck();
  await level.selectOption('3');
  putLevel(9,2); // Another client changed a route while this draft was being edited.
  await save.click();
  await page.waitForFunction(()=>document.querySelector('#rfDenoiseStatus').textContent.includes('请核对后再保存'));
  assert.equal(await level.inputValue(),'3');
  assert(!await input(1).isChecked());
  assert.equal(getLevel(9),2);
  await undo.click();
  assert(await card.locator('[data-dn=mixed]').isVisible());
  assert(await input(1).isChecked());
  await save.click();await saved();
  assert.equal(getLevel(9),1);

  await level.selectOption('2');
  for(const setting of settings)await input(setting).uncheck();
  await page.locator('[data-page=device]').click();
  await page.locator('[data-page=audio]').click();
  assert.equal(await card.locator('input:checked').count(),0);
  await save.click();await saved();
  assert.equal(denoise.levels,0);
  assert.equal(denoise.selection_level,2);
  failDenoiseRead=true;
  await page.waitForFunction(()=>document.querySelector('[data-dn=connection]').textContent.includes('暂未更新'));
  assert.equal(await level.inputValue(),'2');
  assert(!await level.isDisabled());
  failDenoiseRead=false;
  await card.locator('[data-dn=retry]').click();
  await page.setViewportSize({width:390,height:844});
  assert(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth));
  assert.equal(await card.locator('.denoise-options').first().evaluate(el=>getComputedStyle(el).gridTemplateColumns.split(' ').length),1);
  await card.evaluate(el=>el.scrollIntoView({block:'center'}));
  await card.screenshot({path:path.join(output,'denoise-mobile.png')});
  await page.setViewportSize({width:1440,height:900});
  status.intercom.mode=1;
  await page.locator('#eqTabSpk').click();
  await page.locator('[data-page=device]').click();assert(await page.locator('#fpk0').isVisible());assert(await page.locator('#handsetNameInput').isVisible());
  await page.locator('[data-page=talk]').click();
  await page.mouse.move(0,0);assert.equal(await page.locator('[data-page=talk]').getAttribute('aria-current'),'page');
  assert.deepEqual(errors,[]);
  const desktop=await inventory();
  await page.screenshot({animations:'disabled',path:path.join(output,'current-desktop.png')});
  await page.screenshot({animations:'disabled',path:path.join(output,'current-desktop-full.png'),fullPage:true});
  await page.setViewportSize({width:390,height:844});
  const mobile=await inventory();
  await page.screenshot({animations:'disabled',path:path.join(output,'current-mobile.png')});
  assert(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth));
  await page.locator('[data-page=bridge]').click();await page.screenshot({animations:'disabled',path:path.join(output,'current-mobile-bridge.png'),fullPage:true});
  assert(await page.locator('.panel-more').isVisible());await page.locator('.panel-more').click();
  await page.locator('[data-page=device]').click();assert(await page.locator('#otaBtn').isVisible());
  await page.locator('[data-page=talk]').click();
  await page.screenshot({animations:'disabled',path:path.join(output,'current-mobile-nav.png')});
  await page.setViewportSize({width:1440,height:1000});
  const toggle=page.locator('#panelLayoutToggle');
  assert.equal(await page.locator('.telemetry-strip > :first-child').getAttribute('id'),'panelLayoutToggle');
  assert((await toggle.boundingBox()).height>=48);
  await page.evaluate(()=>{window.controlRefs=[...document.querySelectorAll('input,select,textarea')];SDR.on=true;});
  await page.locator('[data-page=radio]').click();await page.locator('#ftx').fill('433.5050');
  await page.locator('#ixTailFile').setInputFiles({name:'draft.wav',mimeType:'audio/wav',buffer:Buffer.from('pending audio')});
  status.ptt='TX';refuseStop=true;await page.evaluate(()=>{txOn=true;txEnd=Date.now()+60000;talkToken=124;});
  await toggle.click();await page.waitForFunction(()=>document.getElementById('panelNotice').textContent.includes('未确认'));
  assert.equal(await page.locator('body').getAttribute('data-panel-layout'),'simple');
  refuseStop=false;await toggle.click();await page.waitForFunction(()=>document.body.dataset.panelLayout==='detail');
  assert(await page.locator('#ftx').isVisible());assert(await page.locator('#eqReset').isVisible());assert(await page.locator('#brT1').isVisible());
  assert(await page.locator('#rfDenoiseCard').isVisible());
  assert.equal(await page.locator('.panel-nav').count(),0);
  assert.equal(await page.locator('#npPeerList .np2p-peerrow').count(),16);
  assert.equal(await page.locator('#ftx').inputValue(),'433.5050');
  await page.evaluate(()=>{holdSyncUntil=0;last='';return refresh();});
  assert.equal(await page.locator('#ftx').inputValue(),'433.5050');
  await page.screenshot({animations:'disabled',path:path.join(output,'layout-detail-desktop.png')});
  await assertEqSlotLayout(page,3,0);
  await page.setViewportSize({width:390,height:844});
  assert((await toggle.boundingBox()).height>=48);assert(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth));
  await page.screenshot({animations:'disabled',path:path.join(output,'layout-detail-mobile.png')});
  await page.locator('#mobNav [data-sec-tab=eq]').click();
  await assertEqSlotLayout(page,1,0);
  for(let i=0;i<4;i++){
   await toggle.click();await page.waitForFunction(()=>document.body.dataset.panelLayout==='simple');
   assert(await page.locator('#ftx').isVisible());
   assert(await page.evaluate(()=>controlRefs.every(el=>el.isConnected)&&new Set(controlRefs).size===document.querySelectorAll('input,select,textarea').length));
   assert.equal(await page.locator('#ixTailFile').evaluate(el=>el.files[0].name),'draft.wav');
   assert(await page.evaluate(()=>SDR.on));
   await toggle.click();await page.waitForFunction(()=>document.body.dataset.panelLayout==='detail');
  }
  await page.reload();await page.waitForFunction(()=>document.body.dataset.panelLayout==='detail');
  status.device_id='orl-other';identity.device_id=status.device_id;await page.reload();await page.waitForFunction(()=>window._st?.device_id==='orl-other');
  assert.equal(await page.locator('body').getAttribute('data-panel-layout'),'simple');
  status.device_id=identity.device_id=originalIdentity;await page.reload();await page.waitForFunction(()=>document.body.dataset.panelLayout==='detail');
  await toggle.click();await page.waitForFunction(()=>document.body.dataset.panelLayout==='simple');
  await page.screenshot({animations:'disabled',path:path.join(output,'layout-simple-mobile.png')});
  Object.assign(status.intercom,{mode:2,relay_link:'connected',relay_reason:'',peers:27});
  await page.waitForFunction(()=>document.querySelector('#relayTalkState').textContent.includes('已连接'));
  await page.locator('[data-page=network]').click();
  assert.equal(await page.locator('#ixSeg .relay-state').count(),0);
  assert.equal(await page.locator('#relayStageState').getAttribute('data-kind'),'connected');
  assert(!(await page.locator('.mode-stage[data-modes="2"]').innerText()).includes('27'));
  Object.assign(status.intercom,{relay_link:'error',relay_reason:'authentication'});
  await page.waitForFunction(()=>document.querySelector('#relayStageState').textContent.includes('用户名'));
  assert.equal(await page.locator('#relayStageState').getAttribute('data-kind'),'down');
  Object.assign(status.intercom,{relay_link:'reconnecting',relay_reason:''});
  await page.waitForFunction(()=>document.querySelector('#relayStageState').textContent.includes('正在重连'));
  await page.locator('[data-page=talk]').click();
  assert(await page.locator('#relayTalkState').isVisible());
  status.bridge.keyed=1;
  await page.waitForFunction(()=>!document.querySelector('#speakerBridgeState').hidden);
  await page.locator('#spkVol').evaluate(el=>{
   el.value='4';
   el.dispatchEvent(new Event('input',{bubbles:true}));
   el.dispatchEvent(new Event('change',{bubbles:true}));
  });
  await page.waitForFunction(()=>document.querySelector('#speakerSaveState').textContent.includes('已确认'));
  assert.equal(status.spk,4);status.bridge.keyed=0;
  await page.locator('.panel-more').click();
  await page.locator('[data-page=device]').click();
  assert(await page.locator('#panelClock').evaluate(el=>el.nextElementSibling.classList.contains('name-plate')));
  assert.equal(await page.locator('#panelClockZone').inputValue(),'480');
  await page.locator('#panelClockZone').selectOption('345');
  assert.equal(await page.evaluate(()=>{
   const nativeNow=Date.now;Date.now=()=>Date.UTC(2026,10,19,18,15,1);
   renderClock();Date.now=nativeNow;
   return document.querySelector('#panelClockText').textContent;
  }),'UTC+05:45 00:00:01');
  await page.locator('#panelClock').screenshot({path:path.join(output,'clock-mobile.png')});
  await page.locator('.panel-more').click();
  await page.locator('[data-page=audio]').click();
  await page.setViewportSize({width:320,height:812});
  await assertEqSlotLayout(page,1,0);
  const slot=page.locator('#eqPresetList [data-slot="1"]');
  const more=slot.getByRole('button',{name:'槽位 1 更多操作'});
  await slot.getByRole('button',{name:'保存',exact:true}).click();
  await page.locator('#eqPresetName').fill('车载');
  await page.locator('#eqPresetSubmit').click();
  await page.waitForFunction(()=>document.querySelector('#eqPresetFeedback').textContent.includes('已保存'));
  await assertEqSlotLayout(page,1,1);
  await page.setViewportSize({width:1440,height:900});
  await assertEqSlotLayout(page,3,1);
  await page.setViewportSize({width:320,height:812});
  const longName='夜间通联'.repeat(6);
  eqConflict=true;
  await more.click();
  await slot.getByRole('button',{name:'重命名'}).click();
  await page.locator('#eqPresetName').fill(longName);
  await page.locator('#eqPresetSubmit').click();
  await page.waitForFunction(()=>document.querySelector('#eqPresetFeedback').textContent.includes('已变化'));
  assert.equal(await page.locator('#eqPresetName').inputValue(),longName);
  await page.locator('#eqPresetSubmit').click();
  await page.waitForFunction(()=>document.querySelector('#eqPresetFeedback').textContent.includes('已重命名'));
  assert.equal(eqSlots[0].name,longName);
  assert(await slot.evaluate(row=>{
   const title=row.querySelector('strong'),actions=row.querySelector('.eq-user-actions');
   const a=title.getBoundingClientRect(),b=actions.getBoundingClientRect();
   return title.scrollWidth>title.clientWidth && getComputedStyle(title).textOverflow==='ellipsis' &&
       Math.abs(a.y+a.height/2-b.y-b.height/2)<1 && b.right<=row.getBoundingClientRect().right;
  }));
  await more.click();
  assert.equal(await slot.locator('.eq-user-menu span').innerText(),longName);
  await page.keyboard.press('Escape');
  assert(await slot.getByRole('button',{name:'重命名'}).isHidden());
  assert.equal(await more.getAttribute('aria-expanded'),'false');
  await slot.getByRole('button',{name:'应用'}).click();
  await page.waitForFunction(()=>document.querySelector('#eqPresetFeedback').textContent.includes('已应用'));
  await more.click();
  await slot.getByRole('button',{name:'覆盖'}).click();
  assert.equal(await page.locator('#eqPresetName').inputValue(),longName);
  await page.locator('#eqPresetCancel').click();
  await more.click();
  await slot.getByRole('button',{name:'删除'}).click();
  await page.waitForFunction(()=>document.querySelector('#eqPresetFeedback').textContent==='已删除');
  assert.equal(eqSlots[0],null);
  await page.setViewportSize({width:1440,height:900});
  await assertEqSlotLayout(page,3,0);
  for(const [index,name] of [longName,'车载','日常'].entries()){
   const row=page.locator('#eqPresetList').locator('.eq-user-slot').nth(index);
   await row.getByRole('button',{name:'保存',exact:true}).click();
   await page.locator('#eqPresetName').fill(name);
   await page.locator('#eqPresetSubmit').click();
   await row.locator('strong').filter({hasText:name}).waitFor();
   assert.equal(eqSlots[index].name,name);
  }
  await assertEqSlotLayout(page,3,3);
  await slot.getByRole('button',{name:'应用'}).click();
  await page.waitForFunction(()=>document.querySelector('#eqPresetFeedback').textContent.includes('已应用'));
  await more.click();
  assert.equal(await slot.locator('.eq-user-menu span').innerText(),longName);
  await page.keyboard.press('Escape');
  await page.locator('#eqUserPresets').screenshot({path:path.join(output,'eq-slots-desktop.png')});
  await page.setViewportSize({width:761,height:850});
  await assertEqSlotLayout(page,3,3);
  await page.setViewportSize({width:760,height:850});
  await assertEqSlotLayout(page,1,3);
  const touch=await page.context().newCDPSession(page);
  await page.setViewportSize({width:844,height:390});
  await touch.send('Emulation.setTouchEmulationEnabled',{enabled:true});
  await page.waitForFunction(()=>matchMedia('(hover: none) and (pointer: coarse)').matches);
  await assertEqSlotLayout(page,1,3);
  await touch.send('Emulation.setTouchEmulationEnabled',{enabled:false});
  await touch.detach();
  await page.setViewportSize({width:320,height:812});
  await assertEqSlotLayout(page,1,3);
  await page.locator('#eqUserPresets').screenshot({path:path.join(output,'eq-slots-mobile.png')});
  await page.reload();
  await page.waitForFunction(()=>document.querySelector('#panelClockZone').value==='345');
  await page.locator('#panelLayoutToggle').click();
  await page.waitForFunction(()=>document.body.dataset.panelLayout==='detail');
  assert(await page.locator('#panelClock').evaluate(el=>el.nextElementSibling.classList.contains('name-plate')));
  assert.equal(await page.locator('#ixSeg .relay-state').count(),0);
  await page.locator('#mobNav [data-sec-tab=eq]').click();
  await assertEqSlotLayout(page,1,3);
  await page.setViewportSize({width:1440,height:900});
  await assertEqSlotLayout(page,3,3);
  await page.locator('#eqUserPresets').screenshot({path:path.join(output,'eq-slots-detail-desktop.png')});
  await page.goto('http://heimao.test/setup');
  await page.waitForFunction(()=>document.getElementById('state').textContent.includes('重名'));
  assert.equal(await page.locator('#openHost').getAttribute('href'),'https://heimao-2.local/');
  assert.equal(await page.locator('#openIp').getAttribute('href'),'https://192.0.2.1/');
  Object.assign(status.wifi,{mdns_host:'',mdns_state:'error',ip:'192.0.2.2'});
  await page.waitForFunction(()=>document.getElementById('state').textContent.includes('域名发布异常'));
  assert(await page.locator('#openHost').isHidden());
  assert.equal(await page.locator('#openIp').getAttribute('href'),'https://192.0.2.2/');
  assert.deepEqual(errors,[]);
  const layoutReport={desktop,mobile,eqSlots:{desktopColumns:3,mobileColumns:1,boundary:760,
   filledCounts:[0,1,3],detail:true,touchLandscape:true},errors};
  fs.writeFileSync(path.join(output,'inventory.json'),JSON.stringify(layoutReport,null,2));
  console.log(JSON.stringify(layoutReport,null,2));
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});

