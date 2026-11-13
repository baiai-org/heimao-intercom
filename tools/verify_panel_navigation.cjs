const {chromium}=require(process.env.PLAYWRIGHT_MODULE || 'playwright');
const fs=require('node:fs'),path=require('node:path'),assert=require('node:assert/strict');
const main=path.resolve(__dirname,'../firmware/main');
const output=process.env.PANEL_TEST_OUTPUT || __dirname;fs.mkdirSync(output,{recursive:true});
const identity={device_id:'orl-example',api_version:2,custom_name:'桥接测试',name_revision:1};
const originalIdentity=identity.device_id;
const status={...identity,build:{version:'r11-90'},wifi:{connected:true,ip:'192.0.2.1',ssid:'测试网络',state:'connected',rssi:-45,host:'heimao',ip6:''},
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
const channels=[0,1,2].map(i=>({i,name:'测试信道'+(i+1),rx:433.5+i*.0125,tx:433.5+i*.0125,bw:0,sq:3,vol:5,pwr:0,band:'UHF',compatible:true}));
const radio={freq_grid_hz:500,freq_decimals:4,band:'UHF',min:400,max:480,model:'SA818S-U',compatible:true,ready:true,power_applied:0,power_pending:0};
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
   const scripts={'/app-control.js':'app_control.js','/device-name.js':'device_name.js','/rx-guard.js':'rx_guard.js'};
   if(scripts[p])return route.fulfill({contentType:'application/javascript',body:fs.readFileSync(path.join(main,scripts[p]))});
   if(p==='/api/device')json=identity;
   else if(p==='/api/talk'){assert.equal(url.searchParams.get('on'),'0');stops++;if(!refuseStop)status.ptt="IDLE";json={ok:!refuseStop};}
   else if(p==='/api/sdr_ptt'){assert.equal(url.searchParams.get('on'),'0');json={ok:true,sid:Number(url.searchParams.get('sid')),tx:Number(url.searchParams.get('tx')),id:Number(url.searchParams.get('id')),ptt:refuseStop};}
   else if(p==='/api/status')json=status;
   else if(p==='/api/np2p/denied')json={ready:true,count:denied.length,limit:64,entries:denied};
   else if(p==='/api/radio')json=radio;
   else if(p==='/api/live')json={ptt:'IDLE',rf_key:0,rf_tx:0,ic_mode:status.intercom.mode,rssi:80,peak:200,mod_sq:1};
   else if(p==='/api/channels')json=channels;
   else if(['/api/ctcss','/api/dcs'].includes(p))json=[];
   else if(p==='/api/set'){
    const k=url.searchParams.get('k'),v=url.searchParams.get('v');writes.push({k,v});
    if(k==='ix_mode')status.intercom.mode=Number(v);
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
  await page.locator('[data-page=audio]').click();await page.getByRole('button',{name:'尾音',exact:true}).click();assert(await page.locator('#ixTailFile').count());
  await page.getByRole('button',{name:'均衡器',exact:true}).click();assert(await page.locator('#eqReset').isVisible());
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
  assert.equal(await page.locator('.panel-nav').count(),0);
  assert.equal(await page.locator('#npPeerList .np2p-peerrow').count(),16);
  assert.equal(await page.locator('#ftx').inputValue(),'433.5050');
  await page.evaluate(()=>{holdSyncUntil=0;last='';return refresh();});
  assert.equal(await page.locator('#ftx').inputValue(),'433.5050');
  await page.screenshot({animations:'disabled',path:path.join(output,'layout-detail-desktop.png')});
  await page.setViewportSize({width:390,height:844});
  assert((await toggle.boundingBox()).height>=48);assert(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth));
  await page.screenshot({animations:'disabled',path:path.join(output,'layout-detail-mobile.png')});
  await page.locator('#mobNav [data-sec-tab=eq]').click();
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
  assert.deepEqual(errors,[]);
  fs.writeFileSync(path.join(output,'inventory.json'),JSON.stringify({desktop,mobile,errors},null,2));
  console.log(JSON.stringify({desktop,mobile,errors},null,2));
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});

