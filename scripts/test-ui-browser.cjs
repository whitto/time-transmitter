#!/usr/bin/env node
'use strict';
// Optional real-browser regression check. Requires Playwright and Chromium.
// RADIOCLOCK_CHROMIUM can select another installed Chromium executable.
const assert = require('node:assert/strict');
const { readFile } = require('node:fs/promises');
const http = require('node:http');
const path = require('node:path');
const { chromium } = require('playwright');

(async () => {
  const html = await readFile(path.join(__dirname, '../ui/radioclock.html'));
  const server = http.createServer((req, res) => {
    res.writeHead(200, { 'content-type': 'text/html' });
    res.end(html);
  });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  let browser;
  const requests = [];
  const binding = { id: 0, bound: true, name: 'Original watch', address: '11:22:33:44:55:66', protocol: 0 };
  const config = {
    ssid: 'Home', timezone: 'Asia/Tokyo', full_time_tx: false,
    full_time_station: 0, transmission_offset_minutes: 0,
    bt_timezone: 'Australia/Brisbane', bt_time_offset_minutes: 0,
    wifi_power_mode: 0, bt_manual_profile: 0, bt_manual_protocol: 0,
    bt_always_wait: false, bt_profiles: [binding,
      { id: 1, bound: true, name: 'Other watch', address: '22:33:44:55:66:77', protocol: 1,
        battery_percent: null, battery_read_at: '', battery_status: 'Battery reading supported on GW-BX5600 only' }],
    bt_times: [{ minute: 30, enabled: false, protocol: 0, profile: 0, done_today: true }],
  };
  let schedules = [{ station: 0, start: 0, end: 1440 }];
  const status = {
    time: '12:00:00', date: '2026-10-07', clock_state: 'Synchronized',
    firmware_version: 'V4.13', station: -1, radio_active: false,
    bt_last_sync_status: 'Never synced', bt_last_sync_date: '2026-10-07 11:30:42',
    wifi_connected:true,ap_mode:false,wifi_ip:'10.0.1.137',
    bt_day_complete: true, bt_pairing: false,
  };
  let holdNextStatus = false, statusHeld, releaseStatus;
  let holdNextSlotWrite = false, slotWriteHeld, releaseSlotWrite;
  let rejectNextSlotWrite = false;
  let holdNextLedWrite = false, ledWriteHeld, releaseLedWrite;
  let rejectNextLedWrite = false;
  let rejectNextWatchOption = false;
  let holdNextHistoryWrite=false,historyWriteHeld,releaseHistoryWrite,rejectNextHistoryWrite=false;
  let holdNextAccessWrite=false,accessWriteHeld,releaseAccessWrite,rejectNextAccessWrite=false;
  let holdNextConfig = false, configHeld, releaseConfig;
  try {
    browser = await chromium.launch({
      executablePath: process.env.RADIOCLOCK_CHROMIUM || '/usr/bin/chromium',
      headless: true, args: ['--no-sandbox'],
    });
    const page = await browser.newPage({viewport:{width:1440,height:1000}});
    // Poll explicitly so delayed-response race checks are deterministic.
    await page.addInitScript(() => { window.setInterval = () => 0; });
    const pageErrors = [];
    page.on('pageerror', error => pageErrors.push(error.message));
    await page.route('**/api/**', async route => {
      const req = route.request(), url = new URL(req.url()).pathname;
      requests.push({ url, method: req.method(), body: req.postData() });
      let data = {}, code = 200;
      if (url === '/api/stations') data = [{ id: 0, name: 'JJY 40 kHz', encoding: 'JJY' }, { id: 1, name: 'JJY 60 kHz', encoding: 'JJY' }];
      else if (url === '/api/config') {
        if (req.method() === 'POST') {
          const fields = Object.fromEntries([...(req.postData() || '').matchAll(/name="([^"]+)"\r\n\r\n([^\r\n]*)/g)].map(x => [x[1], x[2]]));
          if (Object.hasOwn(fields, 'bt_history_persist')) {
            assert.deepEqual(Object.keys(fields),['bt_history_persist']);
            if(holdNextHistoryWrite){holdNextHistoryWrite=false;historyWriteHeld();await new Promise(resolve=>{releaseHistoryWrite=resolve})}
            if(rejectNextHistoryWrite){rejectNextHistoryWrite=false;code=500;data={status:'error',message:'History preference could not be saved'}}
            else{config.bt_history_persist=fields.bt_history_persist==='1';data={status:'ok'}}
          } else if (Object.hasOwn(fields, 'wifi_access_enabled')) {
            assert.deepEqual(Object.keys(fields),['wifi_access_enabled','wifi_access_start','wifi_access_end','wifi_access_timezone']);
            if(holdNextAccessWrite){holdNextAccessWrite=false;accessWriteHeld();await new Promise(resolve=>{releaseAccessWrite=resolve})}
            if(rejectNextAccessWrite){rejectNextAccessWrite=false;code=500;data={status:'error',message:'Wi-Fi access schedule could not be saved'}}
            else{Object.assign(config,{wifi_access_enabled:fields.wifi_access_enabled==='1',wifi_access_start:Number(fields.wifi_access_start),wifi_access_end:Number(fields.wifi_access_end),wifi_access_timezone:fields.wifi_access_timezone});if(config.wifi_access_enabled)config.wifi_power_mode=1;data={status:'ok'}}
          } else if(Object.hasOwn(fields,'wifi_power_mode')) {
            config.wifi_power_mode=Number(fields.wifi_power_mode);data={status:'ok'};
          } else if (Object.hasOwn(fields, 'bt_font_profile') || Object.hasOwn(fields, 'bt_idle_power_save')) {
            if (rejectNextWatchOption) {
              rejectNextWatchOption = false; code = 500;
              data = { status: 'error', message: 'Watch option could not be saved' };
            } else {
              if (Object.hasOwn(fields, 'bt_font_profile')) config.bt_profiles.find(p=>Number(p.id)===Number(fields.bt_font_profile)).font_mode=Number(fields.bt_font_mode);
              else config.bt_idle_power_save=fields.bt_idle_power_save==='1';
              data={status:'ok'};
            }
          } else if (Object.hasOwn(fields, 'activity_led_enabled')) {
            assert.deepEqual(Object.keys(fields), ['activity_led_enabled']);
            if (holdNextLedWrite) {
              holdNextLedWrite = false;
              ledWriteHeld();
              await new Promise(resolve => { releaseLedWrite = resolve; });
            }
            if (rejectNextLedWrite) {
              rejectNextLedWrite = false; code = 500;
              data = { status: 'error', message: 'LED setting could not be saved' };
            } else {
              config.activity_led_enabled = fields.activity_led_enabled === '1';
              data = { status: 'ok' };
            }
          } else if (Object.hasOwn(fields, 'bt_manual_profile')) {
            assert.deepEqual(Object.keys(fields), ['bt_manual_profile']);
            config.bt_manual_profile = Number(fields.bt_manual_profile);
            data = { status: 'ok' };
          } else {
            assert.ok(Object.hasOwn(fields, 'bt_slot'), 'browser check expects slot or LED config writes');
            if (holdNextSlotWrite) {
              holdNextSlotWrite = false;
              slotWriteHeld();
              await new Promise(resolve => { releaseSlotWrite = resolve; });
            }
            if (rejectNextSlotWrite) {
              rejectNextSlotWrite = false; code = 409;
              data = { status: 'error', message: 'Slot rejected by device' };
            } else {
              const slot = config.bt_times[Number(fields.bt_slot)];
              if (Object.hasOwn(fields, 'bt_slot_enabled')) slot.enabled = fields.bt_slot_enabled === '1';
              data = { status: 'ok' };
            }
          }
        } else {
          data = structuredClone(config);
          if (holdNextConfig) {
            holdNextConfig = false;
            configHeld();
            await new Promise(resolve => { releaseConfig = resolve; });
          }
        }
      }
      else if (url === '/api/schedules') {
        if (req.method() === 'POST') {
          schedules = JSON.parse(req.postData());
          assert.ok(schedules.every(x => Number.isInteger(x.start) && Number.isInteger(x.end) && x.start >= 0 && x.end <= 1440 && x.end > x.start));
          data = { status: 'ok' };
        } else data = schedules;
      } else if (url === '/api/status') {
        data = structuredClone({ ...config, ...status });
        if (holdNextStatus) {
          holdNextStatus = false;
          statusHeld();
          await new Promise(resolve => { releaseStatus = resolve; });
        }
      }
      else if (url === '/api/settings' && req.method() === 'POST') {
        const fields = Object.fromEntries([...(req.postData() || '').matchAll(/name="([^"]+)"\r\n\r\n([^\r\n]*)/g)].map(x => [x[1], x[2]]));
        assert.deepEqual(Object.keys(fields), ['bt_always_wait']);
        config.bt_always_wait = fields.bt_always_wait === '1';
        data = { status: 'ok' };
      }
      else if (url === '/api/diagnostics') data = {
        firmware:'V4.13',uptime_sec:7200,reset_reason:9,heap_max_alloc:90000,loop_stack_min_free:3000,
        heap_free:120000,heap_min_free:110000,clock_state:'Synchronized',ntp_age_sec:60,
        ntp_sync_count:12,clock_error_est_sec:0.1,ntp_interval_sec:3600,wifi_connected:status.wifi_connected,wifi_ip:status.wifi_ip,
        radio_active:status.radio_active,radio_paused:status.radio_paused||false,carrier_hz:status.carrier_hz||0,boundary_delay_us:80,boundary_delay_worst_us:100,
        missed_second_boundaries:0,littlefs_used:4096,littlefs_total:983040,bt_window_active:false,
        bt_connection_attempts:3,bt_acked_writes:2,bt_notifications:4,bt_response_errors:0,bt_delivery_evidence:'ATT write acknowledged; watch display unverified',
        bt_last_sync_date:status.bt_last_sync_date,bt_last_sync_status:status.bt_last_sync_status,bt_last_sync_epoch:1791491417,bt_last_outcome_successful:true,bt_history_persist:config.bt_history_persist,bt_history_saved_at:config.bt_history_saved_at,bt_history_save_status:config.bt_history_save_status,ap_mode:status.ap_mode,ap_ip:status.ap_ip
      };
      else if (url === '/api/bluetooth-pair') data = { message: 'Pairing window opened' };
      await route.fulfill({ status: code, contentType: 'application/json', body: JSON.stringify(data) });
    });
    await page.goto('http://127.0.0.1:' + server.address().port);
    await page.waitForFunction(() => document.querySelectorAll('#schedulesList .schedule').length === 1 && document.getElementById('fw').textContent === 'V4.13');
    // Browser clicks return before asynchronous onchange/onclick work finishes.
    // Observe the real handler promises instead of assuming HTTP/render timing.
    await page.evaluate(() => {
      const save = window.saveSchedules, pair = window.pairWatch, led = window.setActivityLed;
      window.saveSchedules = (...args) => (window.lastScheduleSave = save(...args));
      window.pairWatch = (...args) => (window.lastPairWatch = pair(...args));
      window.setActivityLed = (...args) => (window.lastLedSave = led(...args));
    });
    const clickSaveSchedules = async () => {
      await page.getByRole('button', { name: 'Save Schedules', exact: true }).click();
      await page.evaluate(async () => await window.lastScheduleSave);
    };
    assert.deepEqual(await page.locator('.navbtn').allTextContents(), ['Overview', 'Radio', 'Watch (BLE)', 'Schedules', 'Network', 'Settings', 'Diagnostics', 'About']);
    assert.equal(await page.locator('.schEnd').inputValue(), '00:00', 'all-day end must be a valid browser time input');
    await page.evaluate(() => saveSchedules());
    assert.deepEqual(schedules, [{ station: 0, start: 0, end: 1440 }]);
    assert.equal(await page.locator('#toast').textContent(), 'Transmission schedules saved.');

    await page.locator('.schStation').selectOption('1');
    await page.locator('.schStart').fill('09:15');
    await page.locator('.schEnd').fill('10:45');
    await page.evaluate(() => addSchedule());
    assert.equal(await page.locator('.schStation').first().inputValue(), '1');
    assert.equal(await page.locator('.schStart').first().inputValue(), '09:15');
    assert.equal(await page.locator('.schEnd').first().inputValue(), '10:45');
    await page.locator('.schStart').first().fill('09:45');
    await page.evaluate(() => removeSchedule(1));
    assert.equal(await page.locator('.schStart').inputValue(), '09:45');
    await page.evaluate(() => saveSchedules());
    assert.deepEqual(schedules, [{ station: 1, start: 585, end: 645 }], 'save must use retained drafts');

    await page.locator('.schStart').fill('');
    await page.locator('.schEnd').fill('');
    await page.evaluate(() => addSchedule());
    assert.equal(await page.locator('.schStart').first().inputValue(), '', 'empty drafts must remain empty when adding');
    assert.equal(await page.locator('.schEnd').first().inputValue(), '');
    await page.evaluate(() => removeSchedule(1));
    assert.equal(await page.locator('.schStart').inputValue(), '', 'empty drafts must remain empty when removing another row');
    const posts = () => requests.filter(x => x.url === '/api/schedules' && x.method === 'POST').length;
    const beforeInvalid = posts();
    await page.evaluate(() => saveSchedules());
    assert.equal(posts(), beforeInvalid, 'blank times must be rejected before an API request');
    assert.match(await page.locator('#toast').textContent(), /valid start and end time/);
    await page.locator('.schStart').fill('21:00');
    await page.locator('.schEnd').fill('01:00');
    await page.evaluate(() => saveSchedules());
    assert.equal(posts(), beforeInvalid, 'an overnight range must not be silently accepted by the daytime-only API');
    await page.locator('.schEnd').fill('00:00');
    await page.evaluate(() => saveSchedules());
    assert.deepEqual(schedules, [{ station: 1, start: 1260, end: 1440 }]);

    // The automatic toggle saves immediately. RF Save must not undo it,
    // including when a status poll captured before the toggle arrives late.
    await page.evaluate(() => showView('schedules'));
    assert.equal(await page.locator('#btScheduleList .slot-note').first().textContent(), 'Disabled');
    assert.match(await page.locator('#view-schedules').textContent(), /Enabled slots repeat until switched off/);
    assert.match(await page.locator('#view-schedules').textContent(), /save automatically and survive power restarts/);
    holdNextStatus = true;
    const oldStatusCaptured = new Promise(resolve => { statusHeld = resolve; });
    await page.evaluate(() => { window.oldSlotStatus = tick(); });
    await oldStatusCaptured;
    holdNextSlotWrite = true;
    const writeStarted = new Promise(resolve => { slotWriteHeld = resolve; });
    const automaticToggle = page.locator('#btScheduleList input[type="checkbox"]').first();
    await automaticToggle.check();
    await writeStarted;
    assert.equal(await automaticToggle.isChecked(), true);
    assert.equal(await automaticToggle.isDisabled(), true, 'pending writes must not accept another toggle');
    assert.equal(await page.locator('#btScheduleList .slot-note').first().textContent(), 'Saving…');
    await page.evaluate(async () => renderStatus(await api('/api/status')));
    assert.equal(await automaticToggle.isChecked(), true, 'polling during a pending write must preserve the visible choice');
    releaseSlotWrite();
    await page.waitForFunction(() => btSlotSaving < 0);
    assert.equal(config.bt_times[0].enabled, true, 'toggle must save without clicking RF Save');
    assert.equal(await page.locator('#btScheduleList .slot-note').first().textContent(), 'Enabled daily · delivered today');
    assert.equal(await page.locator('#toast').textContent(), 'Daily automatic watch sync enabled and saved.');
    await clickSaveSchedules();
    releaseStatus();
    await page.evaluate(async () => await window.oldSlotStatus);
    assert.equal(await automaticToggle.isChecked(), true, 'RF Save and a late poll must not undo a saved automatic toggle');
    assert.equal(await page.evaluate(() => config.bt_times[0].enabled), true);
    await page.reload();
    await page.waitForFunction(() => document.querySelector('#btScheduleList input[type="checkbox"]')?.checked === true);
    await page.evaluate(() => {
      const save = window.saveSchedules, pair = window.pairWatch, led = window.setActivityLed;
      window.saveSchedules = (...args) => (window.lastScheduleSave = save(...args));
      window.pairWatch = (...args) => (window.lastPairWatch = pair(...args));
      window.setActivityLed = (...args) => (window.lastLedSave = led(...args));
    });
    assert.equal(await automaticToggle.isChecked(), true, 'saved automatic toggle must survive reloading');

    await automaticToggle.uncheck();
    await page.waitForFunction(() => btSlotSaving < 0);
    rejectNextSlotWrite = true;
    await automaticToggle.focus();
    await automaticToggle.click();
    await page.waitForFunction(() => btSlotSaving < 0);
    assert.equal(await automaticToggle.isChecked(), false, 'rejected focused checkbox must revert immediately');
    assert.equal(await automaticToggle.isDisabled(), false);
    assert.equal(config.bt_times[0].enabled, false);
    assert.equal(await page.locator('#btScheduleList .slot-note').first().textContent(), 'Disabled');
    assert.match(await page.locator('#toast').textContent(), /Slot rejected by device/);
    await clickSaveSchedules();
    assert.equal(await automaticToggle.isChecked(), false, 'RF Save must not be needed to reveal a failed toggle');

    assert.equal(await page.locator('#heroWatch').textContent(), 'Delivered · 2026-10-07 11:30:42', 'saved delivery must survive reboot status');
    assert.equal(await page.locator('#watchStatus').textContent(),'Time sync delivered');
    // Both battery displays use the selected profile and keep the delivery
    // timestamp separate from the last battery reading in Bluetooth time.
    assert.equal(await page.locator('#quickWatchBattery').textContent(), 'Battery: unavailable · Watch 1');
    assert.equal(await page.locator('#watchBattery').textContent(), 'Unavailable · Watch 1');
    const batterySample = '2026-10-07 11:30:38';
    Object.assign(binding, { battery_percent: 60, battery_read_at: batterySample, battery_status: 'Read during sync' });
    const writesBeforeBatteryRead = requests.filter(r => r.method === 'POST').length;
    await page.evaluate(() => tick());
    assert.equal(await page.locator('#quickWatchBattery').textContent(), 'Battery: 60% estimate · Watch 1');
    assert.equal(await page.locator('#watchBattery').textContent(), '60% · Watch 1');
    assert.equal(await page.locator('#watchBatteryReadAt').textContent(), batterySample);
    assert.equal(await page.locator('#watchBatteryStatus').textContent(), 'Read during sync');
    assert.equal(await page.locator('#heroWatch').textContent(), 'Delivered · 2026-10-07 11:30:42');
    assert.equal(await page.locator('#watchDate').textContent(), '2026-10-07 11:30:42');
    assert.equal(requests.filter(r => r.method === 'POST').length, writesBeforeBatteryRead);
    await page.evaluate(() => showView('home'));
    await page.evaluate(() => { window.scrollTo({ top: 0, left: 0, behavior: 'instant' }); document.getElementById('toast').classList.remove('show'); });
    await page.screenshot({ path: '/tmp/radioclock-v412-battery-overview.png', fullPage: true, animations: 'disabled' });
    await page.evaluate(() => showView('watch'));
    await page.evaluate(() => window.scrollTo({ top: 0, left: 0, behavior: 'instant' }));
    await page.screenshot({ path: '/tmp/radioclock-v412-battery-watch.png', fullPage: true, animations: 'disabled' });
    binding.battery_percent = 0;
    await page.evaluate(() => tick());
    assert.equal(await page.locator('#watchBattery').textContent(), '0% · Watch 1');
    binding.battery_percent = 100;
    await page.evaluate(() => tick());
    assert.equal(await page.locator('#quickWatchBattery').textContent(), 'Battery: 100% estimate · Watch 1');
    binding.battery_status = 'Battery read timed out; last reading retained';
    await page.evaluate(() => tick());
    assert.equal(await page.locator('#watchBattery').textContent(), '100% · Watch 1');
    assert.equal(await page.locator('#watchBatteryReadAt').textContent(), batterySample);
    assert.match(await page.locator('#watchBatteryStatus').textContent(), /timed out; last reading retained/);
    await page.locator('#btManualProfile').selectOption('1');
    await page.waitForFunction(() => config.bt_manual_profile === 1 && !pendingSettings.has('bt_manual_profile'));
    assert.equal(await page.locator('#quickWatchBattery').textContent(), 'Battery: unavailable · Watch 2');
    assert.equal(await page.locator('#watchBattery').textContent(), 'Unavailable · Watch 2');
    assert.match(await page.locator('#watchBatteryStatus').textContent(), /supported on GW-BX5600 only/);
    await page.locator('#btManualProfile').selectOption('0');
    await page.waitForFunction(() => config.bt_manual_profile === 0 && !pendingSettings.has('bt_manual_profile'));
    assert.equal(await page.locator('#watchBattery').textContent(), '100% · Watch 1');
    Object.assign(binding, { battery_percent: null, battery_read_at: '', battery_status: 'Not read since restart' });
    await page.evaluate(() => tick());
    assert.equal(await page.locator('#watchBattery').textContent(), 'Unavailable · Watch 1');
    assert.equal(await page.locator('#watchBatteryReadAt').textContent(), 'Not read since restart');
    status.bt_last_sync_status = 'GW-BX5600 MIP - sync attempt failed';
    await page.evaluate(() => tick());
    assert.equal(await page.locator('#heroWatch').textContent(), 'Failed', 'a newer failure must remain visible after a saved delivery');
    assert.equal(await page.locator('#homeWatchStatus').textContent(), status.bt_last_sync_status);
    assert.equal(await page.locator('#watchDate').textContent(), '2026-10-07 11:30:42');

    await page.evaluate(() => showView('watch'));
    const beforePair = requests.length;
    await page.locator('#btReplaceWatchButton').click();
    await page.evaluate(async () => await window.lastPairWatch);
    const replacementRequests = requests.slice(beforePair).filter(x => x.method === 'POST');
    assert.deepEqual(replacementRequests.map(x => x.url), ['/api/bluetooth-pair']);
    assert.ok(!requests.some(x => (x.body || '').includes('bt_rebind_profile')));
    assert.equal(await page.locator('#pairedWatchName').textContent(), binding.name);
    assert.equal(await page.locator('#pairedWatchAddress').textContent(), binding.address);

    // The physical activity-LED preference saves without a separate button.
    await page.evaluate(() => showView('settings'));
    const activityLed = page.getByRole('checkbox', { name: 'Enable blue ESP32 BT sync indicator', exact: true });
    assert.equal(await activityLed.isChecked(), true, 'legacy configuration defaults to the BT sync indicator');
    assert.match(await page.locator('#activity-led-settings').textContent(), /survives power restarts/);
    assert.match(await page.locator('#activity-led-settings').textContent(), /JJY \/ LF transmission, idle listening and Wi-Fi never make it flash/);
    holdNextStatus = true;
    const oldLedStatusCaptured = new Promise(resolve => { statusHeld = resolve; });
    await page.evaluate(() => { window.oldLedStatus = tick(); });
    await oldLedStatusCaptured;
    holdNextLedWrite = true;
    const ledWriteStarted = new Promise(resolve => { ledWriteHeld = resolve; });
    await activityLed.uncheck();
    await ledWriteStarted;
    assert.equal(await activityLed.isChecked(), false);
    assert.equal(await activityLed.isDisabled(), true, 'pending LED storage write disables repeat clicks');
    assert.equal(await page.locator('#activityLedStatus').textContent(), 'Saving…');
    await page.evaluate(async () => renderStatus(await api('/api/status')));
    await page.evaluate(async () => loadConfig());
    assert.equal(await activityLed.isChecked(), false, 'status and config reads during save keep the visible LED choice');
    assert.equal(await page.locator('#activityLedStatus').textContent(), 'Saving…');
    assert.equal(config.activity_led_enabled, undefined, 'pending write must not yet appear persisted');
    releaseLedWrite();
    await page.evaluate(async () => await window.lastLedSave);
    assert.equal(config.activity_led_enabled, false);
    assert.equal(await activityLed.isDisabled(), false);
    assert.equal(await page.locator('#activityLedStatus').textContent(), 'Off');
    assert.equal(await page.locator('#toast').textContent(), 'Blue ESP32 LED turned off and saved.');
    releaseStatus();
    await page.evaluate(async () => await window.oldLedStatus);
    assert.equal(await activityLed.isChecked(), false, 'late pre-save status cannot undo a saved LED preference');
    await page.reload();
    await page.waitForFunction(() => document.getElementById('activityLedStatus').textContent === 'Off');
    assert.equal(await activityLed.isChecked(), false, 'LED Off survives a page reload from saved config');
    await page.evaluate(() => {
      const led = window.setActivityLed;
      window.setActivityLed = (...args) => (window.lastLedSave = led(...args));
    });
    holdNextConfig = true;
    const oldLedConfigCaptured = new Promise(resolve => { configHeld = resolve; });
    await page.evaluate(() => { window.oldLedConfig = loadConfig(); });
    await oldLedConfigCaptured;
    await activityLed.check();
    await page.evaluate(async () => await window.lastLedSave);
    releaseConfig();
    await page.evaluate(async () => await window.oldLedConfig);
    assert.equal(config.activity_led_enabled, true);
    assert.equal(await activityLed.isChecked(), true, 'late pre-save config cannot undo a saved LED preference');
    await page.reload();
    await page.waitForFunction(() => document.getElementById('activityLedStatus').textContent === 'BT sync indicator');
    assert.equal(await activityLed.isChecked(), true, 'LED On survives a page reload from saved config');
    await page.evaluate(() => {
      const led = window.setActivityLed;
      window.setActivityLed = (...args) => (window.lastLedSave = led(...args));
    });
    rejectNextLedWrite = true;
    await activityLed.focus();
    await activityLed.click();
    await page.evaluate(async () => await window.lastLedSave);
    assert.equal(await activityLed.isChecked(), true, 'failed storage write restores a focused LED checkbox immediately');
    assert.equal(await activityLed.isDisabled(), false);
    assert.equal(config.activity_led_enabled, true);
    assert.equal(await page.locator('#activityLedStatus').textContent(), 'BT sync indicator');
    assert.equal(await page.locator('#toast').textContent(), 'LED setting could not be saved');

    // Daily history saves one preference immediately and shields the focused
    // checkbox from rejected writes and late status/config responses.
    await page.evaluate(()=>showView('watch'));
    await page.evaluate(()=>{const h=window.setBtHistoryPersist,a=window.saveWifiAccess;window.setBtHistoryPersist=(...args)=>(window.lastHistorySave=h(...args));window.saveWifiAccess=(...args)=>(window.lastAccessSave=a(...args))});
    const historyToggle=page.locator('#btHistoryPersist');
    assert.equal(await historyToggle.isChecked(),true);
    assert.match(await page.locator('#bt-history-settings').textContent(),/first successful sync each Brisbane calendar day/);
    assert.match(await page.locator('#view-watch').textContent(),/Shows the last reading from a Bluetooth sync; kept in RAM and cleared on restart/);
    assert.match(await page.locator('#bt-history-settings').textContent(),/later failure may be forgotten/);
    holdNextStatus=true;const historyOldStatus=new Promise(resolve=>{statusHeld=resolve});
    await page.evaluate(()=>{window.oldHistoryStatus=tick()});await historyOldStatus;
    holdNextHistoryWrite=true;const historyPending=new Promise(resolve=>{historyWriteHeld=resolve});
    await historyToggle.uncheck();await historyPending;
    assert.equal(await historyToggle.isChecked(),false);assert.equal(await historyToggle.isDisabled(),true);
    await page.evaluate(async()=>{renderStatus(await api('/api/status'));await loadConfig()});
    assert.equal(await historyToggle.isChecked(),false);assert.equal(await page.locator('#btHistoryPersistStatus').textContent(),'Saving…');
    releaseHistoryWrite();await page.evaluate(async()=>await window.lastHistorySave);releaseStatus();await page.evaluate(async()=>await window.oldHistoryStatus);
    assert.equal(config.bt_history_persist,false);assert.equal(await historyToggle.isChecked(),false);
    await page.reload();await page.waitForFunction(()=>document.getElementById('btHistoryPersistStatus').textContent==='RAM only');
    assert.equal(await historyToggle.isChecked(),false);
    await page.evaluate(()=>{const h=window.setBtHistoryPersist;window.setBtHistoryPersist=(...args)=>(window.lastHistorySave=h(...args));showView('watch')});
    rejectNextHistoryWrite=true;await historyToggle.click();await page.evaluate(async()=>await window.lastHistorySave);
    assert.equal(await historyToggle.isChecked(),false);assert.equal(await historyToggle.isDisabled(),false);
    holdNextConfig=true;const historyOldConfig=new Promise(resolve=>{configHeld=resolve});
    await page.evaluate(()=>{window.oldHistoryConfig=loadConfig()});await historyOldConfig;
    await historyToggle.check();await page.evaluate(async()=>await window.lastHistorySave);releaseConfig();await page.evaluate(async()=>await window.oldHistoryConfig);
    assert.equal(await historyToggle.isChecked(),true);assert.equal(config.bt_history_persist,true);
    config.bt_history_saved_at='2026-10-09 06:30:17';config.bt_history_save_status='Saved first success today';
    await page.evaluate(()=>tick());assert.equal(await page.locator('#btHistorySavedAt').textContent(),'2026-10-09 06:30:17');
    await page.evaluate(()=>{window.scrollTo({top:0,left:0,behavior:'instant'});document.getElementById('toast').classList.remove('show')});
    await page.screenshot({path:'/tmp/radioclock-v413-watch-history.png',fullPage:true,animations:'disabled'});

    // Daily access is independent of watch/LF zones and accepts midnight-crossing
    // windows. Enabling picks Power-save; disabling preserves that mode.
    await page.evaluate(()=>showView('settings'));
    await page.evaluate(()=>{const a=window.saveWifiAccess;window.saveWifiAccess=(...args)=>(window.lastAccessSave=a(...args))});
    const accessToggle=page.locator('#wifiAccessEnabled');
    assert.equal(await accessToggle.isChecked(),false);
    assert.equal(await page.locator('#wifiAccessStart').inputValue(),'18:00');
    await page.locator('#wifiAccessStart').fill('22:30');await page.locator('#wifiAccessEnd').fill('01:15');
    await page.locator('#wifiAccessTimezone').selectOption('Asia/Tokyo');await page.evaluate(()=>tick());
    assert.equal(await page.locator('#wifiAccessStart').inputValue(),'22:30','polling retains drafts');
    holdNextStatus=true;const accessOldStatus=new Promise(resolve=>{statusHeld=resolve});
    await page.evaluate(()=>{window.oldAccessStatus=tick()});await accessOldStatus;
    holdNextAccessWrite=true;const accessPending=new Promise(resolve=>{accessWriteHeld=resolve});
    await accessToggle.check();await accessPending;
    assert.equal(await accessToggle.isDisabled(),true);assert.equal(await page.locator('#wifiAccessSave').isDisabled(),true);
    assert.equal(await page.locator('#wifiKeepOn').isDisabled(),true);
    await page.evaluate(async()=>{renderStatus(await api('/api/status'));await loadConfig()});
    assert.equal(await accessToggle.isChecked(),true);assert.equal(await page.locator('#wifiAccessStart').inputValue(),'22:30');
    releaseAccessWrite();await page.evaluate(async()=>await window.lastAccessSave);releaseStatus();await page.evaluate(async()=>await window.oldAccessStatus);
    assert.equal(config.wifi_access_start,1350);assert.equal(config.wifi_access_end,75);assert.equal(config.wifi_access_timezone,'Asia/Tokyo');
    assert.equal(config.wifi_power_mode,1);assert.equal(await page.locator('#wifiKeepOn').isChecked(),false);
    assert.equal(await page.locator('#wifiAccessStatus').textContent(),'Scheduled daily');
    assert.equal(await page.locator('#btTimezone').inputValue(),'Australia/Brisbane');
    assert.match(await page.locator('#wifiAccessHelp').textContent(),/Startup access, the setup AP, recovery and brief NTP wakeups/);
    await page.evaluate(()=>setWifiPowerMode(0));assert.equal(await page.locator('#wifiAccessStatus').textContent(),'Overridden by Always on');
    await page.locator('#wifiAccessStart').fill('23:00');await page.locator('#wifiAccessEnd').fill('23:00');
    const invalidAccessPosts=requests.filter(r=>r.method==='POST').length;
    await page.getByRole('button',{name:'Save Wi-Fi access schedule',exact:true}).click();await page.evaluate(async()=>await window.lastAccessSave);
    assert.equal(requests.filter(r=>r.method==='POST').length,invalidAccessPosts);
    await page.locator('#wifiAccessEnd').fill('02:00');rejectNextAccessWrite=true;
    await page.getByRole('button',{name:'Save Wi-Fi access schedule',exact:true}).click();await page.evaluate(async()=>await window.lastAccessSave);
    assert.equal(await page.locator('#wifiAccessStart').inputValue(),'22:30');assert.equal(await page.locator('#wifiAccessEnd').inputValue(),'01:15');
    assert.equal(await page.locator('#wifiKeepOn').isChecked(),true,'failed grouped write preserves old mode');
    await page.locator('#wifiAccessStart').fill('21:00');await page.locator('#wifiAccessEnd').fill('03:00');
    await page.getByRole('button',{name:'Save Wi-Fi access schedule',exact:true}).click();await page.evaluate(async()=>await window.lastAccessSave);
    assert.equal(config.wifi_power_mode,1);
    holdNextConfig=true;const accessOldConfig=new Promise(resolve=>{configHeld=resolve});
    await page.evaluate(()=>{window.oldAccessConfig=loadConfig()});await accessOldConfig;
    await accessToggle.uncheck();await page.evaluate(async()=>await window.lastAccessSave);releaseConfig();await page.evaluate(async()=>await window.oldAccessConfig);
    assert.equal(await accessToggle.isChecked(),false);assert.equal(await page.locator('#wifiKeepOn').isChecked(),false);
    await page.reload();await page.waitForFunction(()=>document.getElementById('wifiAccessStart').value==='21:00');
    await page.evaluate(()=>showView('settings'));
    assert.equal(await accessToggle.isChecked(),false);assert.equal(await page.locator('#wifiAccessEnd').inputValue(),'03:00');
    await page.evaluate(()=>{window.scrollTo({top:0,left:0,behavior:'instant'});document.getElementById('toast').classList.remove('show')});
    await page.screenshot({path:'/tmp/radioclock-v413-settings.png',fullPage:true,animations:'disabled'});

    // State colors are derived from active RF sessions and AP precedence, in
    // both saved UI themes. An RF zero-carrier envelope remains green.
    await page.evaluate(()=>showView('home'));
    const tileColors=()=>page.evaluate(()=>['radioStatusTile','wifiStatusTile'].map(id=>{const c=getComputedStyle(document.getElementById(id));return {color:c.color,bg:c.backgroundImage,border:c.borderTopColor}}));
    for(const theme of ['dark','light']){
      await page.evaluate(t=>document.documentElement.dataset.theme=t,theme);
      Object.assign(status,{radio_active:true,radio_paused:true,carrier_hz:0,wifi_connected:true,ap_mode:false,wifi_ip:'10.0.1.137'});await page.evaluate(()=>tick());
      assert.equal(await page.locator('#radioStatusTile').evaluate(e=>e.classList.contains('state-good')),true);
      assert.equal(await page.locator('#wifiStatusTile').evaluate(e=>e.classList.contains('state-good')),true);
      const activeColors=await tileColors();assert.equal(activeColors[0].color,activeColors[1].color);
      Object.assign(status,{radio_active:false,ap_mode:true,ap_ip:'192.168.4.1'});await page.evaluate(()=>tick());
      assert.equal(await page.locator('#wifiStatusTile').evaluate(e=>e.classList.contains('state-bad')),true);
      assert.equal(await page.locator('#heroWifi').textContent(),'Setup AP');assert.equal(await page.locator('#quickWifiIp').textContent(),'192.168.4.1');
      const idleColors=await tileColors();assert.equal(idleColors[0].color,idleColors[1].color);assert.notEqual(idleColors[0].color,activeColors[0].color);
      assert.notEqual(idleColors[0].border,activeColors[0].border);
    }
    await page.evaluate(()=>document.documentElement.dataset.theme='dark');
    Object.assign(status,{radio_active:true,station:0,tx_time:'2026-10-07 12:00:00',bt_time:'2026-10-07 13:00:00',carrier_hz:40000,ap_mode:false,wifi_connected:true,radio_paused:false,bt_last_sync_status:'Watch 1: time write delivered'});await page.evaluate(async()=>{await tick();await updateDiagnostics(true)});
    await page.evaluate(()=>{window.scrollTo({top:0,left:0,behavior:'instant'});document.getElementById('toast').classList.remove('show')});
    await page.screenshot({path:'/tmp/radioclock-v413-overview-active.png',fullPage:true,animations:'disabled'});
    await page.evaluate(()=>document.documentElement.dataset.theme='light');
    await page.screenshot({path:'/tmp/radioclock-v413-overview-light.png',fullPage:true,animations:'disabled'});
    await page.setViewportSize({width:390,height:844});
    await page.evaluate(()=>{document.documentElement.dataset.theme='dark';showView('settings');window.scrollTo({top:0,left:0,behavior:'instant'})});
    await page.screenshot({path:'/tmp/radioclock-v413-settings-phone.png',fullPage:true,animations:'disabled'});
    assert.equal(await page.evaluate(()=>document.documentElement.scrollWidth<=window.innerWidth),true,'new controls must not overflow phone width');
    assert.deepEqual(await page.locator('.navbtn').allTextContents(),['Overview','Radio','Watch (BLE)','Schedules','Network','Settings','Diagnostics','About']);
    await page.evaluate(()=>{showView('watch');window.scrollTo({top:0,left:0,behavior:'instant'})});
    await page.screenshot({path:'/tmp/radioclock-v413-watch-phone.png',fullPage:true,animations:'disabled'});
    assert.equal(await page.evaluate(()=>document.documentElement.scrollWidth<=window.innerWidth),true);
    await page.setViewportSize({width:1440,height:1000});
    Object.assign(status,{radio_active:false,station:-1,ap_mode:false,carrier_hz:0,bt_last_sync_status:'GW-BX5600 MIP - sync attempt failed'});await page.evaluate(()=>tick());

    // Font is a per-watch opt-in; saved selections survive reload and errors.
    await page.evaluate(() => showView('watch'));
    const fontToggle=page.locator('#btFontEnabled'),fontChoice=page.locator('#btFontChoice');
    assert.equal(await fontToggle.isChecked(),false);
    assert.equal(await fontChoice.isDisabled(),true);
    await fontToggle.check();
    await page.waitForFunction(()=>!pendingSettings.has('bt_font_mode')&&!document.getElementById('btManualProfile').disabled);
    assert.equal(config.bt_profiles[0].font_mode,1);
    assert.equal(await fontChoice.inputValue(),'1');
    await fontChoice.selectOption('2');
    await page.waitForFunction(()=>!pendingSettings.has('bt_font_mode')&&!document.getElementById('btManualProfile').disabled);
    assert.equal(config.bt_profiles[0].font_mode,2);
    await page.reload();
    await page.waitForFunction(()=>document.getElementById('btFontChoice').value==='2');
    await page.evaluate(()=>showView('watch'));
    assert.equal(await fontToggle.isChecked(),true);
    rejectNextWatchOption=true;
    await fontToggle.click();
    await page.waitForFunction(()=>!pendingSettings.has('bt_font_mode')&&!document.getElementById('btManualProfile').disabled);
    assert.equal(await fontToggle.isChecked(),true,'failed font save restores the focused switch');
    assert.equal(await fontChoice.inputValue(),'2');
    await fontToggle.uncheck();
    await page.waitForFunction(()=>!pendingSettings.has('bt_font_mode')&&!document.getElementById('btManualProfile').disabled);
    assert.equal(config.bt_profiles[0].font_mode,0,'Off keeps the watch font unchanged on sync');
    await fontToggle.check();
    await page.waitForFunction(()=>!pendingSettings.has('bt_font_mode')&&!document.getElementById('btManualProfile').disabled);
    assert.equal(config.bt_profiles[0].font_mode,1);
    await page.screenshot({path:'/tmp/radioclock-v49-watch-options.png',fullPage:true});
    await page.evaluate(()=>showView('settings'));
    assert.equal(await page.locator('#view-settings #bluetooth-power-settings').count(),1);
    assert.equal(await page.locator('#view-network #bluetooth-power-settings').count(),0);
    const powerToggle=page.locator('#btIdlePowerSave');
    await powerToggle.check();
    await page.waitForFunction(()=>!pendingSettings.has('bt_idle_power_save')&&!document.getElementById('btIdlePowerSave').disabled);
    assert.equal(config.bt_idle_power_save,true);
    assert.equal(await page.locator('#btPowerStatus').textContent(),'On');
    rejectNextWatchOption=true;
    await powerToggle.click();
    await page.waitForFunction(()=>!pendingSettings.has('bt_idle_power_save')&&!document.getElementById('btIdlePowerSave').disabled);
    assert.equal(await powerToggle.isChecked(),true,'failed power save restores saved choice');
    await page.reload();
    await page.waitForFunction(()=>document.getElementById('btIdlePowerSave').checked);
    await page.evaluate(()=>showView('settings'));
    await page.screenshot({path:'/tmp/radioclock-v49-power-options.png',fullPage:true});

    // Device defaults turn all four automatic slots and unset Always Wait On.
    // Explicit user-saved Off must remain Off after loading those defaults.
    delete config.bt_always_wait;
    config.bt_times = [390, 750, 1110, 1380].map(minute => ({ minute, enabled: true, protocol: 0, profile: 0 }));
    await page.reload();
    await page.waitForFunction(() => document.querySelectorAll('#btScheduleList input[type="checkbox"]:checked').length === 4 && document.getElementById('btAlwaysWait').checked);
    assert.equal(await page.locator('#btAlwaysWait').isChecked(), true, 'unset Always Wait defaults to On');
    assert.equal(await page.locator('#btPowerStatus').textContent(),'Paused by Always Wait');
    await page.evaluate(() => {
      const wait = window.setBtAlwaysWait;
      window.setBtAlwaysWait = (...args) => (window.lastAlwaysWaitSave = wait(...args));
    });
    await page.evaluate(() => showView('settings'));
    await page.locator('#btAlwaysWait').uncheck();
    await page.evaluate(async () => await window.lastAlwaysWaitSave);
    await page.evaluate(() => showView('schedules'));
    await page.locator('#btScheduleList input[type="checkbox"]').nth(1).uncheck();
    await page.waitForFunction(() => btSlotSaving < 0);
    await page.reload();
    await page.waitForFunction(() => document.querySelectorAll('#btScheduleList input[type="checkbox"]').length === 4 && document.querySelectorAll('#btScheduleList input[type="checkbox"]:checked').length === 3);
    assert.equal(await page.locator('#btAlwaysWait').isChecked(), false, 'saved Always Wait Off overrides the On default');
    assert.equal(await page.locator('#btScheduleList input[type="checkbox"]').nth(1).isChecked(), false, 'saved automatic-slot Off overrides the On default');
    assert.equal(await page.locator('#btScheduleList input[type="checkbox"]:checked').count(), 3);
    await page.evaluate(() => showView('settings'));
    assert.equal(await activityLed.isChecked(), true, 'watch settings do not alter the saved LED choice');
    await page.evaluate(() => showView('settings'));
    await page.evaluate(() => document.activeElement?.blur());
    await page.screenshot({ path: '/tmp/radioclock-v49-led-settings.png', fullPage: true });
    assert.deepEqual(await page.locator('.navbtn').allTextContents(), ['Overview', 'Radio', 'Watch (BLE)', 'Schedules', 'Network', 'Settings', 'Diagnostics', 'About']);
    const diagBefore=requests.filter(r=>r.url==='/api/diagnostics').length;
    await page.evaluate(async()=>{showView('advanced');await Promise.all(Array.from({length:20},()=>updateDiagnostics(true)))});
    assert.equal(requests.filter(r=>r.url==='/api/diagnostics').length-diagBefore,1,'rapid refresh/navigation shares one diagnostics request');
    await page.waitForFunction(()=>document.getElementById('diagnostics').textContent.includes('Next JJY transmission:'));
    assert.match(await page.locator('#diagnostics').textContent(),/Full-time BT listen \(Always Wait\): Off/);
    assert.match(await page.locator('#diagnostics').textContent(),/Next JJY transmission: 2026-10-07 21:00.*Asia\/Tokyo.*JJY 60 kHz/);
    assert.match(await page.locator('#diagnostics').textContent(),/Last reset reason: Brownout/);
    assert.match(await page.locator('#diagnostics').textContent(),/BT saved snapshot.*2026-10-09 06:30:17/);
    assert.match(await page.locator('#diagnostics').textContent(),/First success each Brisbane day; later updates RAM only/);
    assert.match(await page.locator('#diagnostics').textContent(),/BLE connects.*3 \/ 2 \/ 4 \/ 0/);
    assert.match(await page.locator('#diagnostics').textContent(),/Largest free heap block: 90000 bytes/);
    await page.evaluate(()=>window.scrollTo(0,0));
    await page.screenshot({path:'/tmp/radioclock-v49-diagnostics.png',fullPage:true});
    assert.deepEqual(pageErrors, []);
    console.log('V4.13 browser regressions passed: daily history autosave/persistence/rollback/pending and stale reads, overnight Wi-Fi group/drafts/power override, green/red RF/AP states in dark/light, phone/sidebar screenshots; existing: battery estimates/date/retained failures/profile switching/restart, font choices/opt-out/reload/storage failures, Bluetooth power preference/Always Wait, LED autosave/stale reads/rollback, default-on schedules, LF editor and existing sync/pair workflows, eight sidebar routes.');
  } finally {
    if (browser) await browser.close();
    await new Promise(resolve => server.close(resolve));
  }
})().catch(error => { console.error(error); process.exitCode = 1; });
