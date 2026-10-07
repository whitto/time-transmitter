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
    firmware_version: 'V4.5', station: -1, radio_active: false,
    bt_last_sync_status: 'Never synced', bt_last_sync_date: '2026-10-07 11:30:42',
    bt_day_complete: true, bt_pairing: false,
  };
  let holdNextStatus = false, statusHeld, releaseStatus;
  let holdNextSlotWrite = false, slotWriteHeld, releaseSlotWrite;
  let rejectNextSlotWrite = false;
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
          assert.ok(Object.hasOwn(fields, 'bt_slot'), 'browser check only expects slot config writes');
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
        } else data = structuredClone(config);
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
      else if (url === '/api/bluetooth-pair') data = { message: 'Pairing window opened' };
      await route.fulfill({ status: code, contentType: 'application/json', body: JSON.stringify(data) });
    });
    await page.goto('http://127.0.0.1:' + server.address().port);
    await page.waitForFunction(() => document.querySelectorAll('#schedulesList .schedule').length === 1 && document.getElementById('fw').textContent === 'V4.5');
    // Browser clicks return before asynchronous onchange/onclick work finishes.
    // Observe the real handler promises instead of assuming HTTP/render timing.
    await page.evaluate(() => {
      const save = window.saveSchedules, pair = window.pairWatch;
      window.saveSchedules = (...args) => (window.lastScheduleSave = save(...args));
      window.pairWatch = (...args) => (window.lastPairWatch = pair(...args));
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
      const save = window.saveSchedules, pair = window.pairWatch;
      window.saveSchedules = (...args) => (window.lastScheduleSave = save(...args));
      window.pairWatch = (...args) => (window.lastPairWatch = pair(...args));
    });
    assert.equal(await automaticToggle.isChecked(), true, 'saved automatic toggle must survive reloading');

    await automaticToggle.uncheck();
    await page.waitForFunction(() => btSlotSaving < 0);
    rejectNextSlotWrite = true;
    await automaticToggle.focus();
    await automaticToggle.check();
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
    assert.deepEqual(pageErrors, []);
    console.log('Browser regressions passed: LF drafts, empty inputs, midnight/all-day ranges, automatic toggle autosave/RF Save/stale polls/rejection, safe replacement pairing, saved delivery and later failures, eight sidebar routes.');
  } finally {
    if (browser) await browser.close();
    await new Promise(resolve => server.close(resolve));
  }
})().catch(error => { console.error(error); process.exitCode = 1; });
