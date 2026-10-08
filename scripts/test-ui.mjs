#!/usr/bin/env node
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import vm from 'node:vm';

// Exercise the actual embedded-page script with a small DOM/API boundary.
// No browser, network, or third-party packages are needed for this check.
const html = await readFile(new URL('../ui/radioclock.html', import.meta.url), 'utf8');
const source = html.match(/<script>([\s\S]*?)<\/script>/)?.[1];
assert.ok(source, 'UI must have a JavaScript block');
const elements = new Map();
class Element {
  constructor(id) {
    this.id = id;
    this.value = '';
    this.checked = false;
    this.disabled = false;
    this.hidden = false;
    this.className = '';
    this._text = '';
    this._html = '';
    this.htmlWrites = [];
    this.classList = {
      toggle: (name, enabled) => {
        const names = new Set(this.className.split(/\s+/).filter(Boolean));
        if (enabled) names.add(name); else names.delete(name);
        this.className = [...names].join(' ');
      },
      contains: name => this.className.split(/\s+/).includes(name),
    };
  }
  set textContent(value) {
    this._text = String(value);
    this._html = this._text.replace(/[&<>]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;' })[c]);
  }
  get textContent() { return this._text; }
  set innerHTML(value) { this.htmlWrites.push(String(value)); this._html = String(value); }
  get innerHTML() { return this._html; }
  contains() { return false; }
}
for (const match of html.matchAll(/\bid="([^"]+)"/g)) {
  assert.ok(!elements.has(match[1]), `duplicate ID: ${match[1]}`);
  elements.set(match[1], new Element(match[1]));
}
const element = id => {
  assert.ok(elements.has(id), `UI referenced missing DOM element: ${id}`);
  return elements.get(id);
};
const state = {
  ssid: 'test-network', timezone: 'Australia/Brisbane', full_time_tx: false,
  full_time_station: 0, transmission_offset_minutes: 0,
  bt_timezone: 'Australia/Brisbane', bt_time_offset_minutes: 0, wifi_power_mode: 1,
  bt_manual_profile: 0, bt_manual_protocol: 0, bt_always_wait: false,
  bt_profiles: [{ id: 0, bound: false, name: '', address: '' }], bt_times: [],
  bt_state: 'Waiting for watch', bt_pairing: false,
};
const status = {
  time: '12:00:00', date: '2026-10-06', clock_state: 'Synchronized',
  firmware_version: 'V4.9', radio_active: false, station: -1,
  bt_last_sync_date: '2026-10-06 22:15:42',
  bt_time: '2026-10-06 22:00:00',
  bt_last_sync_status: 'Never synced', bt_day_complete: false,
};
const requests = [];
let failNextSettings = false;
let releaseStatus;
let deferNextStatus = false;
let releaseConfig;
let deferNextConfig = false;
let releaseSetting;
let deferNextSetting = false;
class FormData {
  constructor() { this.fields = []; }
  append(key, value) { this.fields.push([key, value]); }
  *entries() { yield* this.fields; }
}
const response = (data, code = 200) => ({ ok: code < 400, status: code, json: async () => structuredClone(data) });
const fetch = async (url, options = {}) => {
  const method = options.method ?? 'GET';
  const fields = options.body instanceof FormData ? Object.fromEntries(options.body.entries()) : {};
  requests.push({ url, method, fields });
  if (method === 'POST' && ['/api/config', '/api/settings'].includes(url)) {
    if (deferNextSetting) {
      deferNextSetting = false;
      await new Promise(resolve => { releaseSetting = resolve; });
    }
    if (failNextSettings) {
      failNextSettings = false;
      const led = Object.hasOwn(fields, 'activity_led_enabled');
      return response({ status: 'error', message: led ? 'LED setting could not be saved' : 'Rejected setting' }, led ? 500 : 409);
    }
    for (const [key, value] of Object.entries(fields)) {
      if (['bt_always_wait', 'activity_led_enabled', 'bt_idle_power_save'].includes(key)) state[key] = value === '1';
      else if (['wifi_power_mode', 'bt_manual_profile', 'bt_manual_protocol'].includes(key)) state[key] = Number(value);
      else if (key === 'bt_time_offset_minutes') state[key] = Number(value);
      else if (key === 'bt_timezone') state[key] = value;
    }
    if (Object.hasOwn(fields, 'bt_font_profile')) state.bt_profiles.find(p=>Number(p.id)===Number(fields.bt_font_profile)).font_mode=Number(fields.bt_font_mode);
    if (Object.hasOwn(fields, 'bt_slot')) {
      const slot = state.bt_times[Number(fields.bt_slot)];
      if (Object.hasOwn(fields, 'bt_slot_enabled')) slot.enabled = fields.bt_slot_enabled === '1';
      if (Object.hasOwn(fields, 'bt_slot_time')) slot.minute = Number(fields.bt_slot_time);
      if (Object.hasOwn(fields, 'bt_slot_profile')) slot.profile = Number(fields.bt_slot_profile);
      if (Object.hasOwn(fields, 'bt_slot_protocol')) slot.protocol = Number(fields.bt_slot_protocol);
    }
    return response({ status: 'success' });
  }
  if (url === '/api/config') {
    const snapshot = structuredClone(state);
    if (deferNextConfig) {
      deferNextConfig = false;
      await new Promise(resolve => { releaseConfig = resolve; });
    }
    return response(snapshot);
  }
  if (url === '/api/status') {
    const snapshot = { ...structuredClone(state), ...structuredClone(status) };
    if (deferNextStatus) {
      deferNextStatus = false;
      await new Promise(resolve => { releaseStatus = resolve; });
    }
    return response(snapshot);
  }
  if (url === '/api/stations') return response([{ id: 0, name: 'JJY', encoding: 'JJY 40 kHz' }]);
  if (url === '/api/schedules') return response([]);
  if (url === '/api/diagnostics') return response({ wifi_connected: true, radio_active: false, heap_free: 100000 });
  if (url === '/api/bluetooth-sync' && method === 'POST') {
    state.bt_state = 'Waiting for watch';
    return response({ message: 'Manual window restarted' });
  }
  if (url === '/api/bluetooth-pair' && method === 'POST') {
    state.bt_state = 'Pairing'; state.bt_pairing = true;
    return response({ message: 'Pairing window opened' });
  }
  throw new Error(`Unexpected request: ${method} ${url}`);
};
const context = vm.createContext({
  document: { getElementById: element, querySelectorAll: () => [], activeElement: null },
  localStorage: { getItem: () => null, setItem() {} },
  fetch, FormData, setTimeout: () => 1, clearTimeout() {}, setInterval() {},
  scrollTo() {}, confirm: () => true, console,
});
const evaluate = code => vm.runInContext(code, context);
evaluate(source);
// Allow the page's real startup request chain to finish before interacting.
for (let i = 0; i < 12; i++) await new Promise(resolve => setImmediate(resolve));
assert.equal(element('pairedWatchInfo').textContent, 'No watch paired');
assert.equal(element('pairedWatchDetails').hidden, true);
assert.equal(element('wifiKeepOn').checked, false);
assert.equal(element('wifiScheduled').classList.contains('active'), true);
assert.equal(element('btTime').textContent, 'BT watch time: 2026-10-06 22:00:00');
assert.equal(element('btTimezone').value, 'Australia/Brisbane');
assert.equal(element('fw').textContent, 'V4.9');
assert.equal(element('activityLedEnabled').checked, true, 'legacy config without an LED preference must default to enabled');
assert.equal(element('activityLedStatus').textContent, 'Flash on activity');
assert.equal(element('heroWatch').textContent, 'Delivered · 2026-10-06 22:15:42', 'saved delivery timestamp must survive legacy reboot status');
assert.equal(element('watchStatus').textContent, 'Time sync delivered');
status.bt_last_sync_status = 'Last time write delivered (saved; watch unverified)';
await evaluate('tick()');
assert.equal(element('heroWatch').textContent, 'Delivered · 2026-10-06 22:15:42');
assert.equal(element('watchStatus').textContent, 'Time sync delivered');
assert.equal(element('homeWatchStatus').textContent, 'Time sync delivered');
status.bt_last_sync_status = 'Watch 1: GW-BX5600 MIP - time write delivered (watch unverified)';
status.bt_day_complete = true;
await evaluate('tick()');
assert.equal(element('heroWatch').textContent, 'Delivered · 2026-10-06 22:15:42');
assert.equal(element('watchStatus').textContent, 'Time sync delivered');
assert.equal(element('homeWatchStatus').textContent, 'Time sync delivered');
assert.equal(element('homeWatchDate').textContent, '2026-10-06 22:15:42');
assert.equal(element('quickWatchSub').textContent, '2026-10-06 22:15:42');
status.bt_last_sync_status = 'GW-BX5600 MIP - sync attempt failed';
await evaluate('tick()');
assert.equal(element('heroWatch').textContent, 'Failed', 'a later failed attempt must not display the earlier delivery as its result');
assert.equal(element('homeWatchStatus').textContent, status.bt_last_sync_status);
assert.equal(element('watchDate').textContent, '2026-10-06 22:15:42', 'retain the last successful delivery time alongside the latest failure');
assert.equal(element('quickWatch').textContent, 'Failed');
status.bt_last_sync_status = 'Never synced';
status.bt_last_sync_date = '';
status.bt_day_complete = false;
await evaluate('tick()');
assert.equal(element('heroWatch').textContent, 'Not synced');
assert.equal(element('watchStatus').textContent, 'Never synced');
assert.equal(element('homeWatchDate').textContent, 'No successful write recorded');
status.bt_last_sync_date = '2026-10-06 22:15:42';
status.bt_last_sync_status = 'Watch 1: GW-BX5600 MIP - time write delivered (watch unverified)';
status.bt_day_complete = true;
const timezoneRequestStart = requests.length;
await evaluate("setBtTimezone('Asia/Tokyo')");
assert.deepEqual(requests.slice(timezoneRequestStart).find(r => r.method === 'POST'), { url: '/api/config', method: 'POST', fields: { bt_timezone: 'Asia/Tokyo' } });
assert.equal(element('btTimezone').value, 'Asia/Tokyo');
const offsetRequestStart = requests.length;
await evaluate("setBtTimeOffset('60')");
assert.deepEqual(requests.slice(offsetRequestStart).find(r => r.method === 'POST'), { url: '/api/config', method: 'POST', fields: { bt_time_offset_minutes: '60' } });
assert.equal(element('btTimeOffset').value, '60');

// Unknown watch text must be assigned as text, never interpreted as HTML.
state.bt_profiles = [{ id: 0, bound: true, address: '<script>bad()</script>', name: '<img src=x onerror=bad()>', protocol: 0 }];
await evaluate('tick()');
assert.equal(element('pairedWatchInfo').textContent, 'Currently paired watch');
assert.equal(element('pairedWatchDetails').hidden, false);
assert.equal(element('pairedWatchName').textContent, state.bt_profiles[0].name);
assert.equal(element('pairedWatchAddress').textContent, state.bt_profiles[0].address);
assert.equal(element('pairedWatchProfile').textContent, 'Watch 1');
assert.equal(element('pairedWatchProtocol').textContent, 'GW-BX5600 MIP');
assert.ok(!element('pairedWatchName').innerHTML.includes('<img'));
assert.ok(!element('pairedWatchAddress').innerHTML.includes('<script'));
assert.deepEqual(element('pairedWatchName').htmlWrites, []);
assert.deepEqual(element('pairedWatchAddress').htmlWrites, []);

await evaluate('setBtAlwaysWait(true)');
assert.deepEqual(requests.at(-1), { url: '/api/settings', method: 'POST', fields: { bt_always_wait: '1' } });
assert.equal(element('btAlwaysWait').checked, true);
assert.equal(element('btAlwaysWaitStatus').textContent, 'Always Wait: enabled');
await evaluate('setBtAlwaysWait(false)');
assert.equal(requests.at(-1).fields.bt_always_wait, '0');
assert.equal(element('btAlwaysWait').checked, false);
failNextSettings = true;
await evaluate('setBtAlwaysWait(true)');
assert.equal(element('btAlwaysWait').checked, false, 'rejected Always Wait change must restore saved state');
assert.equal(element('btAlwaysWait').disabled, false);
delete state.bt_always_wait;
state.bt_times = [390, 750, 1110, 1380].map(minute => ({ minute, enabled: true, protocol: 0, profile: 0 }));
await evaluate('loadConfig()');
assert.equal(element('btAlwaysWait').checked, true, 'missing Always Wait preference defaults to On');
assert.equal((element('btScheduleList').innerHTML.match(/type="checkbox"\s+checked/g) || []).length, 4, 'all four default-enabled slots must reflect the device configuration');
await evaluate('setBtAlwaysWait(false)');
await evaluate('setBtSlot(1,false)');
await evaluate('loadConfig()');
assert.equal(element('btAlwaysWait').checked, false, 'explicit saved Always Wait Off must be preserved');
assert.equal((element('btScheduleList').innerHTML.match(/type="checkbox"\s+checked/g) || []).length, 3, 'explicit saved slot Off must be preserved');

await evaluate('setWifiPowerMode(0)');
assert.deepEqual(requests.at(-1), { url: '/api/config', method: 'POST', fields: { wifi_power_mode: '0' } });
assert.equal(element('wifiKeepOn').checked, true);
assert.equal(element('wifiAlways').classList.contains('active'), true);
assert.equal(element('wifiScheduled').classList.contains('active'), false);
await evaluate('setWifiPowerMode(1)');
assert.equal(requests.at(-1).fields.wifi_power_mode, '1');
assert.equal(element('wifiKeepOn').checked, false);
assert.equal(element('wifiScheduled').classList.contains('active'), true);
failNextSettings = true;
await evaluate('setWifiPowerMode(0)');
assert.equal(element('wifiKeepOn').checked, false, 'rejected Wi-Fi change must restore saved state');
assert.equal(element('wifiScheduled').classList.contains('active'), true);
assert.equal(element('wifiKeepOn').disabled, false);

// A status response captured before a saved setting must not undo that setting.
deferNextStatus = true;
const staleTick = evaluate('tick()');
await evaluate('setBtAlwaysWait(true)');
releaseStatus(); await staleTick;
assert.equal(element('btAlwaysWait').checked, true, 'stale polling response must not revert a successful save');

// The LED switch persists immediately, ignores stale reads, and rolls back on flash errors.
deferNextStatus = true;
const staleLedTick = evaluate('tick()');
deferNextSetting = true;
const saveLedOff = evaluate('setActivityLed(false)');
assert.equal(element('activityLedEnabled').checked, false);
assert.equal(element('activityLedEnabled').disabled, true);
assert.equal(element('activityLedStatus').textContent, 'Saving…');
evaluate('renderStatus({...lastStatus, activity_led_enabled:true})');
assert.equal(element('activityLedEnabled').checked, false, 'polling during an LED write must preserve the user choice');
assert.equal(element('activityLedStatus').textContent, 'Saving…');
const beforeDuplicateLed = requests.length;
await evaluate('setActivityLed(true)');
assert.equal(requests.length, beforeDuplicateLed, 'pending LED writes must not accept another toggle');
releaseSetting(); await saveLedOff;
assert.deepEqual(requests.findLast(r => r.method === 'POST'), { url: '/api/config', method: 'POST', fields: { activity_led_enabled: '0' } });
assert.equal(state.activity_led_enabled, false);
assert.equal(element('activityLedEnabled').disabled, false);
assert.equal(element('activityLedStatus').textContent, 'Off');
releaseStatus(); await staleLedTick;
assert.equal(element('activityLedEnabled').checked, false, 'a stale status read must not revert a saved LED preference');
await evaluate('loadConfig()');
assert.equal(element('activityLedEnabled').checked, false, 'saved LED Off must survive configuration reload');

deferNextConfig = true;
const staleLedConfig = evaluate('loadConfig()');
await evaluate('setActivityLed(true)');
releaseConfig(); await staleLedConfig;
assert.equal(state.activity_led_enabled, true);
assert.equal(element('activityLedEnabled').checked, true, 'a stale config read must not revert a later LED save');
assert.equal(element('activityLedStatus').textContent, 'Flash on activity');
await evaluate('loadConfig()');
assert.equal(element('activityLedEnabled').checked, true, 'saved LED On must survive configuration reload');

failNextSettings = true;
await evaluate('setActivityLed(false)');
assert.equal(state.activity_led_enabled, true);
assert.equal(element('activityLedEnabled').checked, true, 'failed LED storage write must restore the saved preference');
assert.equal(element('activityLedEnabled').disabled, false);
assert.equal(element('activityLedStatus').textContent, 'Flash on activity');
assert.equal(element('toast').textContent, 'LED setting could not be saved');
state.activity_led_enabled = false;
await evaluate('tick()');
assert.equal(element('activityLedEnabled').checked, false, 'authoritative current status must reflect LED settings made by another browser');

// Bluetooth slots need the same stale-response protection as other settings.
state.bt_times = [{ minute: 30, enabled: false, protocol: 0, profile: 0, done_today: true }];
await evaluate('loadConfig()');
assert.match(element('btScheduleList').innerHTML, />Disabled<\/div>/, 'a disabled slot stays visibly disabled even after a delivery today');
deferNextStatus = true;
const staleSlotTick = evaluate('tick()');
await evaluate('setBtSlot(0,true)');
assert.equal(requests.findLast(r => r.method === 'POST').fields.bt_slot_enabled, '1');
releaseStatus(); await staleSlotTick;
assert.equal(evaluate('config.bt_times[0].enabled'), true, 'stale polling must not undo an automatic sync toggle');
assert.match(element('btScheduleList').innerHTML, /type="checkbox"\s+checked/);
assert.match(element('btScheduleList').innerHTML, /Enabled daily · delivered today/, 'delivery today must not imply that daily listening is disabled');
assert.doesNotMatch(element('btScheduleList').innerHTML, /disabled/);

// A configuration read started before the toggle must not overwrite it later.
deferNextConfig = true;
const staleConfig = evaluate('loadConfig()');
await evaluate('setBtSlot(0,false)');
releaseConfig(); await staleConfig;
assert.equal(evaluate('config.bt_times[0].enabled'), false, 'stale config reads must preserve a later saved toggle');
assert.doesNotMatch(element('btScheduleList').innerHTML, /type="checkbox"\s+checked/);
assert.match(element('btScheduleList').innerHTML, />Disabled<\/div>/);
assert.doesNotMatch(element('btScheduleList').innerHTML, /Enabled daily/);

failNextSettings = true;
await evaluate('setBtSlot(0,true)');
assert.equal(evaluate('config.bt_times[0].enabled'), false, 'a rejected toggle must retain the saved value');
assert.doesNotMatch(element('btScheduleList').innerHTML, /type="checkbox"\s+checked/);
assert.doesNotMatch(element('btScheduleList').innerHTML, /disabled/);
assert.equal(evaluate('btSlotSaving'), -1);

const beforeSync = requests.length;
await evaluate("syncBluetooth($('btSyncButton'))");
await evaluate("syncBluetooth($('btSyncButton'))");
assert.deepEqual(requests.slice(beforeSync).map(r => [r.url, r.method]), [['/api/bluetooth-sync', 'POST'], ['/api/bluetooth-sync', 'POST']]);
assert.equal(element('btSyncButton').disabled, false);
await evaluate("pairWatch($('btPairSettingsButton'))");
assert.equal(requests.at(-1).url, '/api/bluetooth-pair');
const beforeReplace = requests.length;
await evaluate("pairWatch($('btReplaceWatchButton'))");
assert.deepEqual(requests.slice(beforeReplace).map(r => [r.url, r.method]), [['/api/bluetooth-pair', 'POST']]);
assert.ok(!requests.some(r => Object.hasOwn(r.fields, 'bt_rebind_profile')), 'replacement pairing must never request destructive rebinding');
assert.equal(element('btReplaceWatchButton').disabled, false);
await evaluate('tick()');
assert.equal(element('btListenPill').textContent, 'Pairing');
assert.equal(element('pairPill').textContent, 'Pairing');
assert.equal(element('pairedWatchName').textContent, state.bt_profiles[0].name, 'pair request must leave displayed binding intact');
state.bt_state = 'Bluetooth off for RF';
status.radio_active = true;
await evaluate('tick()');
assert.equal(element('btListenPill').textContent, 'Bluetooth off for RF');
assert.equal(element('btListenPill').classList.contains('warn'), true);
assert.equal(element('btAlwaysWaitStatus').textContent, 'Always Wait: enabled', 'RF pause must not appear to disable the saved preference');

// Simulate a completed replacement pairing changing the selected watch record.
state.bt_pairing = false;
state.bt_manual_profile = 1;
state.bt_manual_protocol = 1;
state.bt_profiles.push({ id: 1, bound: true, address: '11:22:33:44:55:66', name: 'Replacement watch', protocol: 1 });
state.bt_state = 'Waiting for watch';
status.radio_active = false;
await evaluate('tick()');
assert.equal(element('pairedWatchName').textContent, 'Replacement watch');
assert.equal(element('pairedWatchProfile').textContent, 'Watch 2');
assert.equal(element('pairedWatchProtocol').textContent, 'Standard digital / hybrid');
assert.equal(element('btManualProfile').value, '1');
assert.equal(element('btPairProfile').value, '1');
assert.equal(element('btListenPill').textContent, 'Waiting for watch');
assert.equal(element('btAlwaysWaitStatus').textContent, 'Always Wait: enabled');
// Font overrides follow the selected profile, preserve opt-out and reject stale reads.
state.bt_manual_profile=0;state.bt_manual_protocol=0;state.bt_profiles[0].font_mode=0;
await evaluate('loadConfig()');
assert.equal(element('btFontEnabled').checked,false);
assert.equal(element('btFontChoice').disabled,true);
element('btFontEnabled').checked=true;element('btFontChoice').value='1';
await evaluate('setWatchFont()');
assert.equal(state.bt_profiles[0].font_mode,1);
assert.equal(state.bt_profiles[1].font_mode??0,0);
deferNextStatus=true;const staleFont=evaluate('tick()');
element('btFontChoice').value='2';await evaluate('setWatchFont()');
releaseStatus();await staleFont;
assert.equal(evaluate('selectedFontMode()'),2);
assert.equal(element('btFontChoice').value,'2');
failNextSettings=true;element('btFontEnabled').checked=false;await evaluate('setWatchFont()');
assert.equal(element('btFontEnabled').checked,true);
assert.equal(evaluate('selectedFontMode()'),2);
await evaluate('loadConfig()');assert.equal(element('btFontChoice').value,'2');
element('btFontEnabled').checked=false;await evaluate('setWatchFont()');
assert.equal(state.bt_profiles[0].font_mode,0);
state.bt_manual_profile=1;state.bt_manual_protocol=1;await evaluate('loadConfig()');
assert.equal(element('btFontEnabled').disabled,true,'unsupported protocols cannot select a font');
// Power preference persists and Always Wait explicitly suspends controller savings.
state.bt_always_wait=false;await evaluate('loadConfig()');
await evaluate('setBtIdlePowerSave(true)');assert.equal(state.bt_idle_power_save,true);
assert.equal(element('btPowerStatus').textContent,'On');
deferNextConfig=true;const stalePower=evaluate('loadConfig()');
await evaluate('setBtIdlePowerSave(false)');releaseConfig();await stalePower;
assert.equal(element('btIdlePowerSave').checked,false);
await evaluate('setBtIdlePowerSave(true)');
failNextSettings=true;await evaluate('setBtIdlePowerSave(false)');
assert.equal(element('btIdlePowerSave').checked,true);
await evaluate('setBtAlwaysWait(true)');assert.equal(element('btPowerStatus').textContent,'Paused by Always Wait');
await evaluate('loadConfig()');assert.equal(element('btIdlePowerSave').checked,true);
console.log('UI handler tests passed: font profile selection/persistence/rollback/stale reads, idle BT power saving, LED, schedules and existing sync/pair behavior.');

// Diagnostics uses saved device schedules and station time, independent of BT zone.
const diagSummary=(s,c,w)=>evaluate(`diagnosticsScheduleSummary(${JSON.stringify(s)},${JSON.stringify(c)},${JSON.stringify(w)})`);
const ds={time:'23:40:00',date:'2026-10-08',timezone:'Asia/Tokyo',clock_state:'Synchronized',bt_always_wait:true,bt_state:'Waiting for watch',radio_active:false};
let summary=diagSummary(ds,{full_time_tx:false},[{station:0,start:390,end:450},{station:2,start:0,end:1440}]);
assert.match(summary[0],/^On/);assert.match(summary[1],/2026-10-09 06:30.*Asia\/Tokyo.*JJY/);
assert.match(diagSummary({...ds,bt_always_wait:false},{},[])[0],/^Off/);
assert.match(diagSummary({...ds,radio_active:true},{},[])[0],/paused during RF/);
assert.match(diagSummary({...ds,time:'06:40:00',station:0,radio_active:true},{},[{station:0,start:390,end:450}])[1],/Transmitting now/);
assert.match(diagSummary(ds,{full_time_tx:true,full_time_station:1},[])[1],/full-time selected/);
assert.match(diagSummary(ds,{full_time_tx:true,full_time_station:2},[{station:0,start:390,end:450}])[1],/overrides schedules/);
assert.match(diagSummary(ds,{},[{station:2,start:0,end:1440}])[1],/No JJY transmission scheduled/);
assert.match(diagSummary({...ds,clock_state:'Unsynchronized'},{},[])[1],/Unknown/);
console.log('Diagnostics summary checks passed: Always Wait, RF pause, next-day JJY, active windows, full-time overrides and unknown clock.');
