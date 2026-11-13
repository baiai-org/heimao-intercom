const {chromium}=require(process.env.PLAYWRIGHT_MODULE||'playwright');
const fs=require('node:fs'),path=require('node:path'),assert=require('node:assert/strict');
const main=path.resolve(__dirname,'../firmware/main');
const output=process.env.PANEL_TEST_OUTPUT||path.resolve(__dirname,'../.tmp/resource-panel');fs.mkdirSync(output,{recursive:true});
const identity={device_id:'orl-example',api_version:2,custom_name:'桥接测试',name_revision:1};
const status={...identity,build:{version:'r11-93'},wifi:{connected:true,ip:'192.0.2.1',ssid:'测试网络',state:'connected',rssi:-45,host:'heimao',ip6:''},
 mode:'MONITOR',ptt:'IDLE',rssi:80,peak:200,mod_sq:true,sql_open:true,eq_spk:[0,0,0,0,0,0],eq_mic:[0,0,0,0,0,0],
 clip:0,ovf:0,underruns:0,gain:96,txgain:36,ntxgain:254,bgtx:16,cvol:80,spk:5,rxnetgain:16,txpre:1,tx_count:0,uptime:120,heap:50000,audio_rate:16000,
 freq:{rx:433.5,tx:433.5,bw:0,sq:3,vol:5,ctcss_tx:'0000',ctcss_rx:'0000',power:0},
 intercom:{mode:1,room:1000,state:'idle',peers:1,tx:0,rx:0,broker:'mqtt.example.test',diag:'',user:'',pass:0},
 bridge:{en:1,chan:1,chan_name:'测试信道',active:1,keyed:0},tail:{id:0,custom_s:0,modes:7},
 rx_guard:{version:1,p2p:{en:1,ms:1234,sources:7,outputs:7},relay:{en:0,ms:2000,sources:7,outputs:7},blocked:7,remaining_ms:1234}};

const radio={freq_grid_hz:500,freq_decimals:4,band:'UHF',min:400,max:480,model:'SA818S-U',compatible:true,ready:true};
const delay=ms=>new Promise(resolve=>setTimeout(resolve,ms));
(async()=>{const browser=await chromium.launch({executablePath:process.env.CHROME_PATH||'C:/Program Files/Google/Chrome/Application/chrome.exe',headless:true});const results=[];
try{for(const scenario of ['widget-recovery','fault-classification','slow-reads','tone-pending']){
 const page=await browser.newPage({viewport:{width:1280,height:1000}}),current=structuredClone(status);
 let omitGuard=false,failRadio=false,failStatus=false,failLive=false,refuseTone=false,renderError=false,scriptFailures=scenario==='widget-recovery'?2:0;
 const errors=[],writes=[],active=new Map();let maxActive=0,maxLive=0;
 page.on('pageerror',e=>errors.push(e.message));
 page.on('request',r=>{const p=new URL(r.url()).pathname;if(['/api/live','/api/status','/api/radio'].includes(p)){active.set(r,p);maxActive=Math.max(maxActive,active.size);maxLive=Math.max(maxLive,[...active.values()].filter(x=>x==='/api/live').length);}});
 page.on('requestfinished',r=>active.delete(r));page.on('requestfailed',r=>active.delete(r));
 await page.route('**/*',async route=>{const url=new URL(route.request().url()),p=url.pathname;if(url.hostname!=='heimao.test')return route.abort();
  if(p==='/')return route.fulfill({contentType:'text/html; charset=utf-8',body:fs.readFileSync(path.join(main,'panel_index.html'))});
  const scripts={'/app-control.js':'app_control.js','/device-name.js':'device_name.js','/rx-guard.js':'rx_guard.js'};
  if(scripts[p]){if(p==='/rx-guard.js'&&scriptFailures-->0)return route.abort();return route.fulfill({contentType:'application/javascript',body:fs.readFileSync(path.join(main,scripts[p]))});}
  let j;if(p==='/api/device')j=identity;
  else if(p==='/api/status'){if(failStatus)return route.abort();j=structuredClone(current);if(omitGuard)delete j.rx_guard;if(renderError)j.eq_spk=null;}
  else if(p==='/api/radio'){if(failRadio)return route.fulfill({status:503,body:'temporarily unavailable'});j=radio;}
  else if(p==='/api/live'){if(failLive)return route.abort();j={ptt:'IDLE',rf_key:0,ic_mode:1,rssi:80,peak:200,mod_sq:1};}
  else if(p==='/api/ctcss')j=[{code:'0001',label:'67.0 Hz'},{code:'0012',label:'100.0 Hz'}];
  else if(['/api/dcs','/api/channels'].includes(p))j=[];
  else if(p==='/api/np2p/denied')j={ready:true,count:0,limit:64,entries:[]};
  else if(p==='/api/set'){
   const k=url.searchParams.get('k'),v=url.searchParams.get('v');writes.push({k,v});
   assert.equal(scenario,'tone-pending');assert(['ctcss_tx','ctcss_rx'].includes(k));
   await delay(2500);
   if(refuseTone){current.rf_config={version:1,revision:2,applied_revision:0,ready:false,error:-1};j={ok:false,err:'模块拒绝或保存失败',rf_config:current.rf_config};}
   else{current.freq[k]=v;current.rf_config={version:1,revision:writes.length+1,applied_revision:writes.length+1,ready:true,error:0};j={ok:true,rf_config:current.rf_config};}
  }else if(p.startsWith('/api/'))throw Error('Unexpected API '+p);
  else if(p.endsWith('.png'))return route.fulfill({contentType:'image/png',body:fs.readFileSync(path.join(main,'brand_icon.png'))});
  else return route.fulfill({status:404,body:''});
  if(scenario==='slow-reads'){if(p==='/api/live')await delay(3500);if(p==='/api/status'||p==='/api/radio')await delay(1200);}
  try{return await route.fulfill({contentType:'application/json',body:JSON.stringify(j)});}catch(e){if(!page.isClosed())throw e;}
 });
 await page.goto('http://heimao.test/');await page.waitForFunction(()=>window._st);await page.locator('[data-page="bridge"]').click();
 await page.waitForFunction(()=>rxGuardViews.length===2&&!document.querySelector('#rxGuard1 [data-rg="form"]').hidden);
 const snapshot=()=>page.evaluate(()=>({online:panelOnline,badge:document.querySelector('#wifiPill').textContent,
  formHidden:document.querySelector('#rxGuard1 [data-rg="form"]').hidden,saveDisabled:document.querySelector('#rxGuard1 [data-rg="save"]').disabled,
  state:document.querySelector('#rxGuard1 [data-rg="state"]').textContent,notice:document.querySelector('#panelReadState').textContent,views:rxGuardViews.length}));
 if(scenario==='widget-recovery'){
  await page.locator('#rxGuard1 [data-rg="delay"]').fill('0.777');
  omitGuard=true;await page.evaluate(()=>refresh());const missing=await snapshot();
  assert(!missing.formHidden&&missing.saveDisabled);assert.match(missing.state,/状态暂缺/);
  omitGuard=false;await page.evaluate(()=>refresh());assert(!(await snapshot()).saveDisabled);
  assert.equal(await page.locator('#rxGuard1 [data-rg="delay"]').inputValue(),'0.777');
  for(let i=0;i<4;i++){await page.locator('#panelLayoutToggle').click();await page.evaluate(()=>refresh());}
  assert.equal(await page.locator('#rxGuard1').count(),1);assert.equal(await page.locator('#rxGuard1 [data-rg="delay"]').inputValue(),'0.777');
  await page.screenshot({path:path.join(output,'resource-panel-desktop.png'),fullPage:true});
  await page.setViewportSize({width:390,height:844});await page.screenshot({path:path.join(output,'resource-panel-mobile.png'),fullPage:true});
  results.push({scenario,missing,after:await snapshot(),writes,errors});
 }else if(scenario==='fault-classification'){
  failRadio=true;await page.evaluate(()=>{panelRadioAt=0;return refresh();});const radioFailure=await snapshot();assert(radioFailure.online);assert.match(radioFailure.notice,/射频信息/);
  failRadio=false;renderError=true;await page.evaluate(()=>refresh());const renderFailure=await snapshot();assert(renderFailure.online);assert.match(renderFailure.notice,/页面显示异常/);
  renderError=false;failStatus=true;failLive=true;await page.evaluate(()=>refresh());assert((await snapshot()).online);
  // Isolate health expiry from real timers while status fails.
  await page.evaluate(()=>{panelLastSeen=Date.now()-11000;return refresh();});const offline=await snapshot();assert(!offline.online);
  failStatus=false;failLive=false;await page.evaluate(()=>refresh());const recovered=await snapshot();assert(recovered.online);
  results.push({scenario,radioFailure,renderFailure,offline,recovered,writes,errors});
 }else if(scenario==='slow-reads'){
  await page.waitForTimeout(9500);assert(maxLive<=1,'meter request overlaps');assert(maxActive<=2,'read concurrency exceeds budget');
  results.push({scenario,maxUnfinishedReadRequests:maxActive,maxLive,writes,errors});
 }else{
  await page.locator('[data-page="radio"]').click();
  await page.selectOption('#ttx','ctcss');await page.waitForFunction(()=>subWrites>0);
  await page.waitForTimeout(1700);await page.evaluate(()=>refresh());
  assert.equal(await page.locator('#ttx').inputValue(),'ctcss','old status overwrote pending tone');
  await page.waitForFunction(()=>subWrites===0);await page.evaluate(()=>refresh());
  assert.equal(current.freq.ctcss_tx,'0001');assert.equal(writes.length,1);
  assert.match(await page.locator('#toneApplyState').textContent(),/手台已确认并保存/);
  refuseTone=true;await page.selectOption('#atx','0012');await page.waitForFunction(()=>subWrites===0);
  await page.evaluate(()=>refresh());assert.equal(current.freq.ctcss_tx,'0001');
  assert.match(await page.locator('#toneApplyState').textContent(),/未确认/);
  refuseTone=false;await page.selectOption('#trx','ctcss');await page.waitForFunction(()=>subWrites===0);await page.evaluate(()=>refresh());
  assert.equal(current.freq.ctcss_tx,'0001');assert.equal(current.freq.ctcss_rx,'0001');
  results.push({scenario,preservedPast1500ms:true,failureVisibleAndSavedValuePreserved:true,writes,errors});
 }
 assert.equal(errors.length,0);if(scenario!=='tone-pending')assert.equal(writes.length,0);
 await page.close();
 }
 fs.writeFileSync(path.join(output,'resource-panel-results.json'),JSON.stringify(results,null,2));console.log(JSON.stringify(results,null,2));
}finally{await browser.close();}})().catch(e=>{console.error(e);process.exitCode=1;});
