const {chromium}=require(process.env.PLAYWRIGHT_MODULE||'playwright');
const fs=require('node:fs'),path=require('node:path'),assert=require('node:assert/strict');
const main=path.resolve(__dirname,'../firmware/main'),out=process.env.PANEL_TEST_OUTPUT||path.resolve(__dirname,'../.tmp/maintenance-panel');fs.mkdirSync(out,{recursive:true});
const identity={device_id:'orl-example',api_version:2,custom_name:'维护测试',name_revision:1,capabilities:{maintenance:2,peer_labels:1}};
const status={...identity,build:{version:'r11-107'},wifi:{connected:true,ip:'192.0.2.1',ssid:'测试网络',state:'connected',rssi:-45,host:'heimao',ip6:''},
 mode:'MONITOR',ptt:'IDLE',rssi:80,peak:200,mod_sq:false,sql_open:false,eq_spk:[0,0,0,0,0,0],eq_mic:[0,0,0,0,0,0],
 clip:0,ovf:0,underruns:0,gain:96,txgain:36,ntxgain:254,bgtx:16,cvol:80,spk:5,rxnetgain:16,txpre:1,tx_count:0,uptime:120,heap:50000,audio_rate:16000,
 freq:{rx:433.5,tx:433.5,bw:0,sq:3,vol:5,ctcss_tx:'0000',ctcss_rx:'0000',power:0},
 intercom:{mode:1,room:1000,state:'idle',peers:1,tx:0,rx:0,broker:'mqtt.example.test',diag:'',user:'',pass:0},
 bridge:{en:0,chan:0,chan_name:'',active:0,keyed:0},tail:{id:0,custom_s:0,modes:7},
 rx_guard:{version:1,p2p:{en:1,ms:1234,sources:7,outputs:7},relay:{en:0,ms:2000,sources:7,outputs:7},blocked:0,remaining_ms:0},
 np2p:{access_version:1,access_ready:true,denied_count:0,denied_limit:64,pub:'192.0.2.10',port:7821,nat:'port-preserving',stun_age_s:14,rtt_ms:51,hub:false,peer_limit:16,peer_count:16,labels_version:1,labels_ready:true,label_revision:0,
 peers:Array.from({length:16},(_,i)=>({id:'m-0011223344'+i.toString(16).padStart(2,'0'),name:'中文测试对端'+i,custom_label:false,ip:i<8?'192.0.2.'+(i+20):'2001:db8::'+i,port:54321+i,state:'linked',rtt_ms:30,rx:100,tx:100,drop:0}))}};
const maint={...identity,ok:true,version:2,ready:true,revision:0,boot_id:'boot-a',cleanup_enabled:true,restart_enabled:false,restart_days:0,restart_minute:240,timezone:'Asia/Shanghai',clock_state:'waiting',last_sync_utc:0,next_restart_utc:0,restart_waiting:false,reboot_pending:false,idle_s:120,busy:'idle',cleanup_runs:0,cleanup_delays:0,released_buffer_bytes:0,released_decoders:0,internal_free:50000,internal_largest:32000,psram_free:6000000,result:'not_run',last_reboot:'other'};
(async()=>{const browser=await chromium.launch({headless:true,executablePath:process.env.CHROME_PATH||'C:/Program Files/Google/Chrome/Application/chrome.exe'});const results=[];
try{for(const mobile of [false,true]){
 const page=await browser.newPage({viewport:mobile?{width:390,height:844}:{width:1280,height:1000}}),state=structuredClone(status),m=structuredClone(maint),writes=[],errors=[];
 const update={...identity,ok:true,boot_id:"boot-a",state:"ready",version:"r11-109",notes:"测试说明",sha256:"a".repeat(64),install:true};
 let failSave=false,failLabel=false,busyCleanup=false,failFactory=false,confirmAccept=true,rebootReads=0;
 page.on('pageerror',e=>errors.push(e.message));page.on('dialog',d=>confirmAccept?d.accept():d.dismiss());
 await page.route('**/*',async route=>{
  const url=new URL(route.request().url()),p=url.pathname;if(url.hostname!=='heimao.test')return route.abort();
  if(p==='/')return route.fulfill({contentType:'text/html; charset=utf-8',body:fs.readFileSync(path.join(main,'panel_index.html'))});
  const scripts={'/app-control.js':'app_control.js','/device-name.js':'device_name.js','/rx-guard.js':'rx_guard.js','/maintenance.js':'maintenance.js','/rf-denoise.js':'rf_denoise.js'};
  if(scripts[p])return route.fulfill({contentType:'application/javascript',body:fs.readFileSync(path.join(main,scripts[p]))});
  let j,code=200;
  if(p==='/api/device')j=identity;
  else if(p==='/api/status')j=state;
  else if(p==='/api/live')j={ptt:'IDLE',rf_key:0,ic_mode:1,rssi:80,peak:200,mod_sq:0};
  else if(p==='/api/radio')j={freq_grid_hz:500,freq_decimals:4,band:'UHF',min:400,max:480,model:'SA818S-U',compatible:true,ready:true};
  else if(['/api/ctcss','/api/dcs','/api/channels'].includes(p))j=[];
  else if(p==='/api/rf_denoise')j={ok:false,err:'此测试仅模拟设备维护'};
  else if(p==='/api/np2p/denied')j={ready:true,count:0,limit:64,entries:[]};
  else if(p==='/api/online_update'){
   if(route.request().method()==='POST'){
    assert.equal(route.request().headers()['x-orl-device'],identity.device_id);
    const b=route.request().postDataJSON();writes.push({path:p,...b});
    assert.equal(b.boot_id,update.boot_id);assert.equal(b.sha256,update.sha256);
    if(b.op==='install')Object.assign(update,{install:false,state:"install_wait"});
    else assert.equal(b.op,"check");
   }
   j=update;
  }
  else if(p==='/api/maintenance'){
   if(route.request().method()==='POST'){
    assert.equal(route.request().headers()['x-orl-device'],identity.device_id);const b=route.request().postDataJSON();writes.push({path:p,...b});
    if(b.op==='set'){
     if(failSave||b.revision!==m.revision){code=409;j={ok:false,err:'版本冲突或保存失败'};}
     else{Object.assign(m,{revision:m.revision+1,cleanup_enabled:b.cleanup_enabled,restart_enabled:b.restart_enabled,restart_days:b.restart_days,restart_minute:b.restart_minute,clock_state:'synced',last_sync_utc:1795075200,next_restart_utc:1795120200});j=m;}
    }else if(b.op==='cleanup'){
     if(busyCleanup){code=409;j={ok:false,err:'正在通联，等待连续空闲60秒'};}
     else{Object.assign(m,{cleanup_runs:m.cleanup_runs+1,last_cleanup_uptime_s:120,released_buffer_bytes:10624,released_decoders:1,result:'reclaimed'});j=m;}
    }else if(b.op==='reboot'){assert.equal(b.boot_id,m.boot_id);m.reboot_pending=true;rebootReads=1;j=m;}
    else if(b.op==='factory_reset'){
     assert.equal(b.boot_id,m.boot_id);
     if(failFactory){code=409;j={ok:false,err:'正在写入'};}
     else {m.reboot_pending=true;j=m;}
    }else throw Error('Unexpected maintenance mutation');
   }else{
    if(rebootReads){if(++rebootReads>=3)Object.assign(m,{boot_id:'boot-b',reboot_pending:false,last_reboot:'manual'});}
    j=m;
   }
  }else if(p==='/api/np2p/peer'){
   assert.equal(route.request().headers()['x-orl-device'],identity.device_id);const b=route.request().postDataJSON();writes.push({path:p,...b});
   if(failLabel||b.revision!==state.np2p.label_revision){code=409;j={ok:false,err:'名称保存失败，草稿已保留'};}
   else if(b.op==='rename'){
    const peer=state.np2p.peers.find(x=>x.id===b.id);assert(peer);peer.name=b.name||'默认名称';peer.custom_label=!!b.name;state.np2p.label_revision++;j={ok:true};
   }else if(b.op==='add'){state.np2p.label_revision++;j={ok:true};}
   else throw Error('Unexpected label mutation');
  }else if(p.endsWith('.png'))return route.fulfill({contentType:'image/png',body:fs.readFileSync(path.join(main,'brand_icon.png'))});
  else if(p.startsWith('/api/'))throw Error('Unexpected API '+p);
  else return route.fulfill({status:404,body:''});
  return route.fulfill({status:code,contentType:'application/json',body:JSON.stringify(j)});
 });
 await page.goto('http://heimao.test/');await page.waitForFunction(()=>window._st&&document.querySelector('#maintenanceSave')&&!document.querySelector('#maintenanceSave').disabled);
 const online=page.locator('#onlineUpdate');
 await online.waitFor({state:'attached'});
 await page.waitForFunction(()=>document.querySelector('#onlineUpdateStatus').textContent.includes('新版已下载'));
 assert.equal(writes.length,0);assert.equal(await page.locator('#maintenanceReboot').count(),1);assert.equal(await page.locator('[data-np-rename]').count(),16);
 // Use the real navigation; the device tab is under More on mobile.
 async function devicePage(){
  if(await page.locator('[data-page="device"]').isVisible())await page.locator('[data-page="device"]').click();
  else{await page.getByRole('button',{name:'更多',exact:true}).click();await page.locator('[data-page="device"]').click();}
 }
 await devicePage();
 assert.match(await page.locator('#maintenanceClock').textContent(),/等待首次校时/);
 await page.locator('#maintenanceTime').fill('04:30');await page.locator('#maintenanceRestart').check();
 await page.locator('#maintenanceSave').click();assert.equal(writes.length,0);
 assert.match(await page.locator('#maintenanceMessage').textContent(),/至少选择一天/);
 await page.getByLabel('周一',{exact:true}).check();
 await page.getByLabel('周三',{exact:true}).check();
 await page.getByLabel('周五',{exact:true}).check();
 // Both trees reuse the same form, preserving drafts and identity.
 for(let i=0;i<4;i++){await page.locator('#panelLayoutToggle').click();assert.equal(await page.locator('#maintenanceTime').inputValue(),'04:30');assert.equal(await page.locator('#maintenanceForm').count(),1);}
 await devicePage();failSave=true;await page.locator('#maintenanceSave').click();await page.waitForFunction(()=>document.querySelector('#maintenanceMessage').textContent.includes('草稿已保留'));
 assert.equal(await page.locator('#maintenanceTime').inputValue(),'04:30');assert.equal(m.restart_enabled,false);
 failSave=false;await page.locator('#maintenanceSave').click();await page.waitForFunction(()=>document.querySelector('#maintenanceMessage').textContent.includes('已确认并保存'));assert.equal(m.restart_days,21);assert.equal(m.restart_minute,270);assert.equal(m.restart_enabled,true);
 assert.match(await page.locator('#maintenanceClock').textContent(),/已校时/);
 assert.match(await page.locator('#maintenanceState').textContent(),/下次重启/);
 busyCleanup=true;await page.locator('#maintenanceCleanNow').click();await page.waitForFunction(()=>document.querySelector('#maintenanceMessage').textContent.includes('未执行回收'));assert.equal(m.cleanup_runs,0);
 busyCleanup=false;await page.locator('#maintenanceCleanNow').click();await page.waitForFunction(()=>document.querySelector('#maintenanceStats').textContent.includes('10624'));
 // Jump to the network tab via real nav or detailed layout and preserve a rename draft through polling.
 if(!await page.locator('[data-np-rename]').first().isVisible()){
  if(await page.locator('[data-page="network"]').isVisible())await page.locator('[data-page="network"]').click();
  else{await page.getByRole('button',{name:'更多',exact:true}).click();await page.locator('[data-page="network"]').click();}
 }
 const selected=state.np2p.peers[0].id;
 await page.locator('[data-np-rename]').first().click();await page.locator('#peerLabelInput').fill('山顶 <>&" 互联');
 state.np2p.peers.reverse();await page.evaluate(()=>refresh());assert.equal(await page.locator('#peerLabelInput').inputValue(),'山顶 <>&" 互联');
 failLabel=true;await page.locator('#peerLabelSave').click();await page.waitForFunction(()=>document.querySelector('#peerLabelError').textContent.includes('保存失败'));
 assert.equal(await page.locator('#peerLabelInput').inputValue(),'山顶 <>&" 互联');assert.equal(writes.at(-1).id,selected);
 failLabel=false;await page.locator('#peerLabelSave').click();await page.waitForFunction(()=>!document.querySelector('#peerLabelDialog'));
 assert.equal(state.np2p.peers.find(x=>x.id===selected).name,'山顶 <>&" 互联');assert.equal(await page.locator('#npPeerList script').count(),0);
 await devicePage();
 assert.equal(await online.isVisible(),true);
 assert.equal(await page.locator('#onlineUpdateNotes').textContent(),'测试说明');
 await page.locator('#onlineUpdateLater').click();
 assert.match(await page.locator('#onlineUpdateStatus').textContent(),/稍后安装/);
 assert.equal(writes.filter(x=>x.path==='/api/online_update').length,0);
 await page.locator('#onlineUpdateCheck').click();
 await page.waitForFunction(()=>!document.querySelector('#onlineUpdateCheck').disabled);
 assert.equal(writes.filter(x=>x.op==='check').length,1);
 confirmAccept=false;await page.locator('#onlineUpdateInstall').click();
 assert.equal(writes.filter(x=>x.op==='install').length,0);
 confirmAccept=true;await page.locator('#onlineUpdateInstall').click();
 await page.waitForFunction(()=>document.querySelector('#onlineUpdateStatus').textContent.includes('已安排安装'));
 assert.equal(writes.filter(x=>x.op==='install').length,1);
 await page.screenshot({path:path.join(out,mobile?'maintenance-mobile.png':'maintenance-desktop.png'),fullPage:true});
 await page.locator('#maintenanceReboot').click();await page.waitForFunction(()=>document.querySelector('#maintenanceMessage').textContent.includes('已重新上线'),{timeout:15000});
 assert.equal(writes.filter(x=>x.op==='reboot').length,1);assert.equal(m.boot_id,'boot-b');
 await devicePage();
 confirmAccept=false;await page.locator('#facWipe').click();
 assert.equal(writes.filter(x=>x.op==='factory_reset').length,0);
 confirmAccept=true;failFactory=true;await page.locator('#facWipe').click();
 await page.waitForFunction(()=>document.querySelector('#maintenanceMessage').textContent.includes('恢复请求未确认'));
 failFactory=false;await page.locator('#facWipe').click();
 await page.waitForFunction(()=>document.querySelector('#maintenanceMessage').textContent.includes('恢复请求已接受'));
 assert.equal(writes.filter(x=>x.op==='factory_reset').length,2);
 assert.equal(await page.locator('#facWipe').isDisabled(),true);
 assert.equal(errors.length,0);
 results.push({viewport:mobile?'mobile':'desktop',draftsAcrossLayouts:true,failedSavePreserved:true,stablePeerIdentityAfterReorder:true,cleanupBusyAndSuccess:true,bootConfirmedOnce:true,factoryCancelBusyAndAccepted:true,onlineUpdateConfirmedOnce:true,writes,errors});await page.close();
 }
 fs.writeFileSync(path.join(out,'maintenance-panel-results.json'),JSON.stringify(results,null,2));console.log(JSON.stringify(results,null,2));
}finally{await browser.close();}})().catch(e=>{console.error(e);process.exitCode=1;});
