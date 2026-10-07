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
    bt_always_wait: false, bt_profiles: [binding],
    bt_times: [{ minute: 30, enabled: false, protocol: 0, profile: 0, done_today: true }],
  };
  let schedules = [{ station: 0, start: 0, end: 1440 }];
  const status = {
    time: '12:00:00', date: '2026-10-07', clock_state: 'Synchronized',
    firmware_version: 'V4.6', station: -1, radio_active: false,
    bt_last_sync_status: 'Never synced', bt_last_sync_date: '2026-10-07 11:30:42',
    bt_day_complete: true, bt_pairing: false,
  };
  let holdNextStatus = false, statusHeld, releaseStatus;
  let holdNextSlotWrite = false, slotWriteHeld, releaseSlotWrite;
  let rejectNextSlotWrite = false;
  let holdNextLedWrite = false, ledWriteHeld, releaseLedWrite;
  let rejectNextLedWrite = false;
  let rejectNextWatchOption = false;
  let holdNextConfig = false, configHeld, releaseConfig;
  try {
    browser = await chromium.launch({
      executablePath: process.env.RADIOCLOCK_CHROMIUM || '/usr/bin/chromium',
      headless: true, args: ['--no-sandbox'],
    });
    const page = await browser.newPage();
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
          if (Object.hasOwn(fields, 'bt_font_profile') || Object.hasOwn(fields, 'bt_idle_power_save')) {
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
      else if (url === '/api/bluetooth-pair') data = { message: 'Pairing window opened' };
      await route.fulfill({ status: code, contentType: 'application/json', body: JSON.stringify(data) });
    });
    await page.goto('http://127.0.0.1:' + server.address().port);
    await page.waitForFunction(() => document.querySelectorAll('#schedulesList .schedule').length === 1 && document.getElementById('fw').textContent === 'V4.6');
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
    const activityLed = page.getByRole('checkbox', { name: 'Flash blue ESP32 LED on activity', exact: true });
    assert.equal(await activityLed.isChecked(), true, 'legacy configuration defaults to activity flashing');
    assert.match(await page.locator('#activity-led-settings').textContent(), /survives power restarts/);
    assert.match(await page.locator('#activity-led-settings').textContent(), /Idle listening and Wi-Fi alone leave it off/);
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
    await page.waitForFunction(() => document.getElementById('activityLedStatus').textContent === 'Flash on activity');
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
    assert.equal(await page.locator('#activityLedStatus').textContent(), 'Flash on activity');
    assert.equal(await page.locator('#toast').textContent(), 'LED setting could not be saved');

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
    await page.screenshot({path:'/tmp/radioclock-v46-watch-options.png',fullPage:true});
    await page.evaluate(()=>showView('network'));
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
    await page.evaluate(()=>showView('network'));
    await page.screenshot({path:'/tmp/radioclock-v46-power-options.png',fullPage:true});

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
    await page.screenshot({ path: '/tmp/radioclock-v46-led-settings.png', fullPage: true });
    assert.deepEqual(await page.locator('.navbtn').allTextContents(), ['Overview', 'Radio', 'Watch (BLE)', 'Schedules', 'Network', 'Settings', 'Diagnostics', 'About']);
    assert.deepEqual(pageErrors, []);
    console.log('Browser regressions passed: font choices/opt-out/reload/storage failures, Bluetooth power preference/Always Wait, LED autosave/stale reads/rollback, default-on schedules, LF editor and existing sync/pair workflows, eight sidebar routes.');
  } finally {
    if (browser) await browser.close();
    await new Promise(resolve => server.close(resolve));
  }
})().catch(error => { console.error(error); process.exitCode = 1; });
