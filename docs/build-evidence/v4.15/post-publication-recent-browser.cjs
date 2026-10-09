'use strict';
const assert=require('node:assert/strict');
const fs=require('node:fs');const http=require('node:http');
const {chromium}=require('/opt/codex/runtimes/codex-primary-runtime/dependencies/node/node_modules/playwright');
(async()=>{
 const html=fs.readFileSync('/workspace/time-transmitter/ui/radioclock.html');
 const mock={ssid:'Home',timezone:'Asia/Tokyo',bt_timezone:'Australia/Brisbane',bt_manual_profile:0,bt_manual_protocol:0,
  bt_profiles:[{id:0,name:'Bedroom watch',address:'11:22:33:44:55:66',bound:true,protocol:0},{id:1,name:'Other watch',address:'22:33:44:55:66:77',bound:true,protocol:1}],
  bt_times:[],bt_recent_sync_profile:0,bt_recent_syncs:[
   {utc_epoch:1791500000,date:'2026-10-09 18:30:20',protocol:0,successful:true},
   {utc_epoch:1791400000,date:'2026-10-09 12:30:11',protocol:1,successful:false},
   {utc_epoch:0,date:'',protocol:2,successful:false},
   {utc_epoch:1791300000,date:'2026-10-09 06:30:09',protocol:0,successful:true}]};
 const status={time:'12:00:00',date:'2026-10-09',clock_state:'Synchronized',firmware_version:'V4.15',firmware_build:'R1',uptime_sec:259449,radio_active:false,bt_last_sync_status:'Time delivered',bt_last_sync_date:'2026-10-09 18:30:20'};
 const server=http.createServer((request,response)=>{
  if(!request.url.startsWith('/api/')){response.writeHead(200,{'Content-Type':'text/html'});response.end(html);return}
  let data={};
  if(request.url==='/api/stations')data=[{id:0,name:'JJY East',encoding:'JJY'}];
  else if(request.url==='/api/config')data=mock;
  else if(request.url==='/api/status')data={...mock,...status};
  else if(request.url==='/api/schedules')data=[];
  else if(request.url==='/api/diagnostics')data={...status,firmware:'V4.15',ntp_age_sec:20,heap_free:100000,heap_max_alloc:80000,littlefs_total:917504,littlefs_used:2048};
  response.writeHead(200,{'Content-Type':'application/json'});response.end(JSON.stringify(data));
 });await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
 let browser;
 try{
  browser=await chromium.launch({executablePath:'/usr/bin/chromium',headless:true,args:['--no-sandbox']});
  const page=await browser.newPage({viewport:{width:1440,height:1000}});const errors=[];page.on('pageerror',e=>errors.push(e.message));
  await page.addInitScript(()=>{window.setInterval=()=>0});
  await page.goto('http://127.0.0.1:'+server.address().port);
  await page.waitForFunction(()=>document.getElementById('quickUptime').textContent==='Uptime: 72h 04m 09s');
  await page.evaluate(()=>showView('watch'));
  assert.equal(await page.locator('#btRecentSyncList>div').count(),4);
  assert.deepEqual(await page.locator('#btRecentSyncList .status-pill').allTextContents(),['Successful','Failed','Failed','Successful']);
  assert.deepEqual(await page.locator('#btRecentSyncList b').allTextContents(),['2026-10-09 18:30:20','2026-10-09 12:30:11','Time unavailable','2026-10-09 06:30:09']);
  assert.match(await page.locator('#btRecentSyncWatch').textContent(),/Bedroom watch/);
  await page.screenshot({path:'/tmp/radioclock-v415-recent-watch-desktop.png',fullPage:true,animations:'disabled'});
  await page.setViewportSize({width:390,height:844});
  assert.equal(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth),true);
  assert.equal(await page.locator('#btRecentSyncList>div').count(),4);
  await page.screenshot({path:'/tmp/radioclock-v415-recent-watch-phone.png',fullPage:true,animations:'disabled'});
  await page.evaluate(()=>{config.bt_manual_profile=1;renderSelectedWatch()});
  assert.match(await page.locator('#btRecentSyncList').textContent(),/No BT syncs since restart/);
  assert.match(await page.locator('#btRecentSyncWatch').textContent(),/Other watch/);
  await page.evaluate(()=>{mergeStatusSettings({bt_manual_profile:1,bt_recent_sync_profile:1,bt_recent_syncs:[{utc_epoch:0,date:'',protocol:1,successful:false}]})});
  assert.equal(await page.locator('#btRecentSyncList>div').count(),1);assert.match(await page.locator('#btRecentSyncList').textContent(),/Failed.*Time unavailable/s);
  await page.evaluate(()=>{mergeStatusSettings({bt_recent_sync_profile:1,bt_recent_syncs:[]});config.bt_history_saved_at='saved snapshot';renderRecentSyncs()});
  assert.match(await page.locator('#btRecentSyncList').textContent(),/No BT syncs since restart/);
  await page.evaluate(()=>showView('home'));assert.equal(await page.locator('#quickUptime').textContent(),'Uptime: 72h 04m 09s');
  await page.evaluate(async()=>{showView('advanced');await updateDiagnostics(true)});
  assert.match(await page.locator('#diagnostics').textContent(),/Uptime: 72h 04m 09s/);
  assert.deepEqual(await page.locator('.navbtn').allTextContents(),['Overview','Radio','Watch (BLE)','Schedules','Network','Settings','Diagnostics','About']);
  assert.deepEqual(errors,[]);
  console.log('Published V4.15 R1 recent-history browser PASS: selected-watch latestfour success/failure rows, unknown timestamp, profile isolation, restart-empty, desktop/390px phone layout and screenshots, Overview/Diagnostics72h04m09s, eight sidebar routes, zero page errors.');
 }finally{await browser?.close();server.close()}
})().catch(error=>{console.error(error);process.exitCode=1});
