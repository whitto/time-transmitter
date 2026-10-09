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
  firmware_version: 'V4.15', firmware_build: 'R1', radio_active: false, station: -1,
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
let hangNextStatus = false, invalidNextStatus = false;
const timeoutCallbacks = new Map(); let timeoutId = 0;
let confirmAnswer = true;
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
      if (['bt_always_wait', 'activity_led_enabled', 'bt_idle_power_save', 'bt_history_persist', 'wifi_access_enabled', 'crash_dump_enabled'].includes(key)) state[key] = value === '1';
      else if (['wifi_power_mode', 'bt_manual_profile', 'bt_manual_protocol', 'wifi_access_start', 'wifi_access_end'].includes(key)) state[key] = Number(value);
      else if (key === 'bt_time_offset_minutes') state[key] = Number(value);
      else if (key === 'wifi_access_timezone') state[key] = value;
      else if (key === 'bt_timezone') state[key] = value;
    }
    if (fields.wifi_access_enabled === '1') state.wifi_power_mode = 1;
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
    if(hangNextStatus){hangNextStatus=false;await new Promise((resolve,reject)=>options.signal.addEventListener('abort',()=>reject(new Error('Aborted')),{once:true}));}
    if(invalidNextStatus){invalidNextStatus=false;return {ok:true,status:200,json:async()=>{throw new SyntaxError('bad JSON')}}}
    const snapshot = { ...structuredClone(state), ...structuredClone(status) };
    if (deferNextStatus) {
      deferNextStatus = false;
      await new Promise(resolve => { releaseStatus = resolve; });
    }
    return response(snapshot);
  }
  if (url === '/api/storage/reset') return response({status:'success'});
  if (url === '/api/stations') return response([{ id: 0, name: 'JJY', encoding: 'JJY 40 kHz' }]);
  if (url === '/api/schedules') return response([]);
  if (url === '/api/diagnostics') return response({ ...state, ...status, wifi_connected: true, heap_free: 100000, bt_connection_attempts: 12, bt_acked_writes: 30, bt_notifications: 20, bt_response_errors: 2 });
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
  document: { getElementById: element, querySelectorAll: () => [], activeElement: null, hidden: false, addEventListener() {} },
  localStorage: { getItem: () => null, setItem() {} },
  fetch, FormData, AbortController, performance, setTimeout: (callback,ms)=>{const id=++timeoutId;timeoutCallbacks.set(id,{callback,ms});return id}, clearTimeout(id){timeoutCallbacks.delete(id)}, setInterval() {},
  scrollTo() {}, confirm: () => confirmAnswer, console,
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
assert.equal(element('fw').textContent, 'V4.15 R1');
assert.equal(element('activityLedEnabled').checked, true, 'legacy config without an LED preference must default to enabled');
assert.equal(element('activityLedStatus').textContent, 'BT sync indicator');
assert.equal(element('heroWatch').textContent, 'Delivered · 2026-10-06 22:15:42', 'saved delivery timestamp must survive legacy reboot status');
assert.equal(element('watchStatus').textContent, 'Time sync delivered');
// Battery estimates follow the selected watch, retain 0%, and never replace
// the date/time of a successful time delivery with the battery sample time.
assert.equal(element('quickWatchBattery').textContent, 'Battery: unavailable · Watch 1');
assert.equal(element('watchBattery').textContent, 'Unavailable · Watch 1');
assert.equal(element('watchBatteryReadAt').textContent, 'Not read since restart');
assert.match(html,/Shows the last reading from a Bluetooth sync; kept in RAM and cleared on restart/);
const originalProfiles = structuredClone(state.bt_profiles);
const batterySample = '2026-10-06 22:15:38';
state.bt_profiles = [
  { id: 0, bound: true, address: '11:22:33:44:55:66', protocol: 0, battery_percent: 60, battery_read_at: batterySample, battery_status: 'Read during sync' },
  { id: 1, bound: true, address: '22:33:44:55:66:77', protocol: 1, battery_percent: null, battery_read_at: '', battery_status: 'Battery reading supported on GW-BX5600 only' },
];
const writesBeforeBatteryRead = requests.filter(r => r.method === 'POST').length;
await evaluate('tick()');
assert.equal(element('quickWatchBattery').textContent, 'Battery: 60% estimate · Watch 1');
assert.equal(element('watchBattery').textContent, '60% · Watch 1');
assert.equal(element('watchBatteryReadAt').textContent, batterySample);
assert.equal(element('watchBatteryStatus').textContent, 'Read during sync');
assert.equal(element('heroWatch').textContent, 'Delivered · 2026-10-06 22:15:42');
assert.equal(element('watchDate').textContent, '2026-10-06 22:15:42');
assert.equal(requests.filter(r => r.method === 'POST').length, writesBeforeBatteryRead, 'showing battery status must not save settings');
state.bt_profiles[0].battery_percent = 0;
await evaluate('tick()');
assert.equal(element('watchBattery').textContent, '0% · Watch 1', 'a valid empty-battery estimate must not be treated as unavailable');
state.bt_profiles[0].battery_percent = 100;
await evaluate('tick()');
assert.equal(element('quickWatchBattery').textContent, 'Battery: 100% estimate · Watch 1');
state.bt_profiles[0].battery_status = 'Battery read timed out; last reading retained';
await evaluate('tick()');
assert.equal(element('watchBattery').textContent, '100% · Watch 1');
assert.equal(element('watchBatteryReadAt').textContent, batterySample);
assert.match(element('watchBatteryStatus').textContent, /timed out; last reading retained/);
await evaluate("setBtManualProfile('1')");
assert.equal(element('quickWatchBattery').textContent, 'Battery: unavailable · Watch 2');
assert.equal(element('watchBattery').textContent, 'Unavailable · Watch 2', 'switching to another profile must not inherit the prior watch reading');
assert.match(element('watchBatteryStatus').textContent, /supported on GW-BX5600 only/);
await evaluate("setBtManualProfile('0')");
assert.equal(element('watchBattery').textContent, '100% · Watch 1');
state.bt_profiles = originalProfiles; // Simulate restart clearing RAM-only samples.
await evaluate('tick()');
assert.equal(element('watchBattery').textContent, 'Unavailable · Watch 1');
assert.equal(element('watchBatteryReadAt').textContent, 'Not read since restart');
console.log('Battery UI checks passed: 0/null/60/100%, per-watch selection, read date/status, retained failure and restart.');
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
assert.equal(element('activityLedStatus').textContent, 'BT sync indicator');
await evaluate('loadConfig()');
assert.equal(element('activityLedEnabled').checked, true, 'saved LED On must survive configuration reload');

failNextSettings = true;
await evaluate('setActivityLed(false)');
assert.equal(state.activity_led_enabled, true);
assert.equal(element('activityLedEnabled').checked, true, 'failed LED storage write must restore the saved preference');
assert.equal(element('activityLedEnabled').disabled, false);
assert.equal(element('activityLedStatus').textContent, 'BT sync indicator');
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
// Mode writes and grouped daily-window saves cannot conflict in the UI.
deferNextSetting=true;const pendingWifiMode=evaluate('setWifiPowerMode(0)');
assert.equal(element('wifiAccessEnabled').disabled,true);
assert.equal(element('wifiAccessSave').disabled,true);
releaseSetting();await pendingWifiMode;
assert.equal(element('wifiAccessEnabled').disabled,false);
assert.equal(element('wifiAccessSave').disabled,false);

// Crash dumping is opt-in and separate from the daily BT snapshot preference.
assert.equal(element('crashDumpEnabled').checked,false);
assert.equal(element('crashDumpStatus').textContent,'Off');
assert.equal(element('crashDumpEnabled').disabled,false,'legacy mock/config without capability field supports toggle');
const crashRequestStart=requests.length;
await evaluate('setCrashDumpEnabled(true)');
assert.deepEqual(requests.slice(crashRequestStart).find(r=>r.method==='POST'),{url:'/api/config',method:'POST',fields:{crash_dump_enabled:'1'}});
assert.equal(element('crashDumpEnabled').checked,true);assert.equal(element('crashDumpStatus').textContent,'On');
assert.equal(element('crashDumpEnabled').disabled,false);
await evaluate('loadConfig()');assert.equal(element('crashDumpEnabled').checked,true);
failNextSettings=true;await evaluate('setCrashDumpEnabled(false)');
assert.equal(element('crashDumpEnabled').checked,true,'rejected crash preference restores saved choice');
assert.equal(element('crashDumpEnabled').disabled,false);
deferNextStatus=true;const staleCrashStatus=evaluate('tick()');
await evaluate('setCrashDumpEnabled(false)');releaseStatus();await staleCrashStatus;
assert.equal(element('crashDumpEnabled').checked,false,'late status cannot undo saved crash preference');
deferNextConfig=true;const staleCrashConfig=evaluate('loadConfig()');
await evaluate('setCrashDumpEnabled(true)');releaseConfig();await staleCrashConfig;
assert.equal(element('crashDumpEnabled').checked,true,'late config cannot undo saved crash preference');
deferNextSetting=true;const pendingCrashSave=evaluate('setCrashDumpEnabled(false)');
assert.equal(element('crashDumpEnabled').disabled,true);assert.equal(element('crashDumpStatus').textContent,'Saving…');
await evaluate('loadConfig()');await evaluate('renderStatus({...lastStatus,crash_dump_enabled:true})');
assert.equal(element('crashDumpEnabled').checked,false,'reads during save keep visible choice');
assert.equal(element('crashDumpStatus').textContent,'Saving…');
releaseSetting();await pendingCrashSave;assert.equal(element('crashDumpEnabled').disabled,false);
assert.equal(state.crash_dump_enabled,false);
assert.match(html,/Off blocks new dumps and keeps any existing dump/);
assert.match(html,/No continuous logs are written/);
assert.match(element('crashDumpHelp').textContent,/separate from daily Bluetooth status snapshots.*no once-per-day limit/);
state.crash_dump_available=false;await evaluate('tick()');
assert.equal(element('crashDumpEnabled').disabled,true);assert.equal(element('crashDumpStatus').textContent,'Unavailable');
const unavailableCrashPosts=requests.filter(r=>r.method==='POST').length;
await evaluate('setCrashDumpEnabled(true)');
assert.equal(requests.filter(r=>r.method==='POST').length,unavailableCrashPosts,'unsupported core must not send enable request');
assert.match(element('crashDumpHelp').textContent,/unavailable with this ESP32 core\/build/);
state.crash_dump_available=true;await evaluate('tick()');assert.equal(element('crashDumpEnabled').disabled,false);
assert.equal(state.bt_history_persist,undefined,'crash preference does not change daily BT retention');
console.log('V4.15 crash toggle checks passed: default Off, persistence, rollback, pending/late reads, unsupported core and independent daily BT history.');

// Daily history is a durable preference, while polling never writes snapshots.
assert.equal(element('btHistoryPersist').checked,true);
const historyRequestStart=requests.length;
await evaluate('setBtHistoryPersist(false)');
assert.deepEqual(requests.slice(historyRequestStart).find(r=>r.method==='POST'),{url:'/api/config',method:'POST',fields:{bt_history_persist:'0'}});
assert.equal(element('btHistoryPersist').checked,false);
assert.equal(element('btHistoryPersistStatus').textContent,'RAM only');
await evaluate('loadConfig()');assert.equal(element('btHistoryPersist').checked,false);
failNextSettings=true;await evaluate('setBtHistoryPersist(true)');
assert.equal(element('btHistoryPersist').checked,false,'rejected persistence preference rolls back immediately');
deferNextStatus=true;const staleHistory=evaluate('tick()');
await evaluate('setBtHistoryPersist(true)');releaseStatus();await staleHistory;
assert.equal(element('btHistoryPersist').checked,true,'late pre-save status cannot undo a history preference');
deferNextConfig=true;const staleHistoryConfig=evaluate('loadConfig()');
await evaluate('setBtHistoryPersist(false)');releaseConfig();await staleHistoryConfig;
assert.equal(element('btHistoryPersist').checked,false,'late config cannot undo a history preference');
await evaluate('setBtHistoryPersist(true)');
deferNextSetting=true;const pendingHistory=evaluate('setBtHistoryPersist(false)');
assert.equal(element('btHistoryPersist').disabled,true);
assert.equal(element('btHistoryPersistStatus').textContent,'Saving…');
await evaluate('loadConfig()');await evaluate('renderStatus({...lastStatus,bt_history_persist:true})');
assert.equal(element('btHistoryPersist').checked,false,'reads during a pending write keep the visible choice');
releaseSetting();await pendingHistory;
assert.equal(element('btHistoryPersist').disabled,false);
state.bt_history_saved_at='2026-10-09 06:30:17';state.bt_history_save_status='Saved first success today';
await evaluate('tick()');
assert.equal(element('btHistorySavedAt').textContent,state.bt_history_saved_at);
assert.equal(element('btHistorySaveStatus').textContent,state.bt_history_save_status);

// Independent daily Wi-Fi windows save one validated set, accept overnight,
// switch to Power-save, retain drafts on polling and roll back failed saves.
assert.equal(element('wifiAccessEnabled').checked,false);
assert.equal(element('wifiAccessStart').value,'18:00');
assert.equal(element('wifiAccessTimezone').value,'Australia/Brisbane');
await evaluate('setWifiPowerMode(0)');
element('wifiAccessEnabled').checked=true;element('wifiAccessStart').value='22:30';element('wifiAccessEnd').value='01:15';element('wifiAccessTimezone').value='Asia/Tokyo';
const accessRequestStart=requests.length;await evaluate('saveWifiAccess()');
assert.deepEqual(requests.slice(accessRequestStart).find(r=>r.method==='POST'),{url:'/api/config',method:'POST',fields:{wifi_access_enabled:'1',wifi_access_start:'1350',wifi_access_end:'75',wifi_access_timezone:'Asia/Tokyo'}});
assert.equal(state.wifi_power_mode,1);
assert.equal(element('wifiKeepOn').checked,false);
assert.equal(element('wifiAccessStatus').textContent,'Scheduled daily');
await evaluate('loadConfig()');assert.equal(element('wifiAccessStart').value,'22:30');
await evaluate('setWifiPowerMode(0)');assert.equal(element('wifiAccessStatus').textContent,'Overridden by Always on');
element('wifiAccessStart').value='23:00';await evaluate('markWifiAccessDraft()');await evaluate('tick()');
assert.equal(element('wifiAccessStart').value,'23:00','polling must preserve a Wi-Fi schedule draft');
assert.equal(element('wifiAccessStatus').textContent,'Unsaved changes');
element('wifiAccessEnd').value='23:00';const invalidAccessPosts=requests.filter(r=>r.method==='POST').length;
await evaluate('saveWifiAccess()');assert.equal(requests.filter(r=>r.method==='POST').length,invalidAccessPosts,'equal start/end rejected before API');
element('wifiAccessEnd').value='02:00';failNextSettings=true;await evaluate('saveWifiAccess()');
assert.equal(element('wifiAccessStart').value,'22:30','rejected grouped save restores the saved window');
assert.equal(element('wifiAccessEnd').value,'01:15');assert.equal(element('wifiKeepOn').checked,true);
deferNextStatus=true;const staleAccess=evaluate('tick()');
element('wifiAccessStart').value='21:00';element('wifiAccessEnd').value='03:00';await evaluate('saveWifiAccess()');
releaseStatus();await staleAccess;assert.equal(element('wifiAccessStart').value,'21:00');assert.equal(element('wifiKeepOn').checked,false);
deferNextConfig=true;const staleAccessConfig=evaluate('loadConfig()');
element('wifiAccessEnabled').checked=false;await evaluate('saveWifiAccess()');releaseConfig();await staleAccessConfig;
assert.equal(element('wifiAccessEnabled').checked,false);assert.equal(element('wifiKeepOn').checked,false,'disabling window leaves Power-save unchanged');
element('wifiAccessEnabled').checked=true;deferNextSetting=true;const pendingAccess=evaluate('saveWifiAccess()');
assert.equal(element('wifiAccessSave').disabled,true);assert.equal(element('wifiKeepOn').disabled,true);
await evaluate('loadConfig()');assert.equal(element('wifiAccessEnabled').checked,true);
releaseSetting();await pendingAccess;assert.equal(element('wifiAccessSave').disabled,false);
assert.equal(element('wifiKeepOn').disabled,false);

// Tile state uses the whole RF session, including a reduced/zero envelope.
evaluate('renderStatus({...lastStatus,radio_active:true,radio_paused:true,carrier_hz:0,wifi_connected:true,ap_mode:false,wifi_ip:"10.0.1.137"})');
assert.equal(element('radioStatusTile').classList.contains('state-good'),true);
assert.equal(element('wifiStatusTile').classList.contains('state-good'),true);
assert.equal(element('heroWifi').textContent,'Connected');
evaluate('renderStatus({...lastStatus,radio_active:false,wifi_connected:true,ap_mode:true,ap_ip:"192.168.4.1"})');
assert.equal(element('radioStatusTile').classList.contains('state-bad'),true);
assert.equal(element('wifiStatusTile').classList.contains('state-bad'),true,'AP+STA remains red');
assert.equal(element('heroWifi').textContent,'Setup AP');assert.equal(element('quickWifiIp').textContent,'192.168.4.1');
evaluate('renderNetworkStatus({wifi_connected:false,ap_mode:false})');assert.equal(element('heroWifi').textContent,'Offline');
status.bt_last_sync_epoch=1791491417;status.bt_last_outcome_successful=true;status.bt_last_sync_date='2026-10-09 06:30:17';status.bt_last_sync_status='Time write delivered';
await evaluate('updateDiagnostics(true)');
assert.match(element('diagnostics').textContent,/Last successful BT sync.*2026-10-09 06:30:17/);
assert.match(element('diagnostics').textContent,/BT saved snapshot.*2026-10-09 06:30:17/);
assert.match(element('diagnostics').textContent,/BLE connects.*12 \/ 30 \/ 20 \/ 2/);
assert.match(element('diagnostics').textContent,/Last BT outcome: Successful/);
console.log('V4.15 UI checks passed: history persistence toggle/rollback/races, independent overnight Wi-Fi drafts/save/power mode, live RF/AP states and restored diagnostics.');
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

// Unattended browser sessions use one bounded status poll; full diagnostics
// are requested only on their visible page and share any active status request.
await evaluate("showView('home')");
const idleStart=requests.length;
for(let i=0;i<60;i++)await evaluate('tick()');
assert.equal(requests.length-idleStart,60,'one status request per Overview tick');
assert.ok(requests.slice(idleStart).every(r=>r.url==='/api/status'),'no idle diagnostics/config/schedule bundle');
assert.equal(evaluate('typeof statusPromise'), 'object');
evaluate('document.hidden=true');
const hiddenStart=requests.length;await evaluate('tick()');await evaluate('updateDiagnostics()');
assert.equal(requests.length,hiddenStart,'hidden browser tabs stop periodic requests');
evaluate('document.hidden=false');
const overlapStart=requests.length;deferNextStatus=true;
const overlapping=Array.from({length:20},()=>evaluate('tick()'));
assert.equal(requests.length-overlapStart,1,'twenty overlapping ticks share one status request');
releaseStatus();await Promise.all(overlapping);
const diagnosticsStart=requests.length;
evaluate("showView('advanced')");await evaluate('updateDiagnostics(true)');
assert.equal(requests.length-diagnosticsStart,4,'Diagnostics navigation requests a single full bundle');
const intervalStart=requests.length;await evaluate('tick()');
assert.equal(requests.length-intervalStart,1,'Diagnostics interval prevents frequent full bundles');
evaluate('diagAt=performance.now()-DIAGNOSTICS_INTERVAL_MS');
const dueStart=requests.length;await evaluate('tick()');await evaluate('diagPromise');
assert.equal(requests.length-dueStart,4,'due diagnostics reuses the status already fetched by tick');
await evaluate("showView('settings')");evaluate('diagAt=0');
const awayStart=requests.length;await evaluate('tick()');
assert.equal(requests.length-awayStart,1,'leaving Diagnostics stops its bundles');

// An earlier config response cannot replace a later read, even without a write.
deferNextConfig=true;state.ssid='older read';const olderRead=evaluate('loadConfig()');
state.ssid='newer read';await evaluate('loadConfig()');releaseConfig();await olderRead;
assert.equal(element('ssid').value,'newer read','out-of-order configuration response discarded');
state.bt_always_wait=false;await evaluate('loadConfig()');deferNextConfig=true;
const olderAlways=evaluate('loadConfig()');await evaluate('setBtAlwaysWait(true)');releaseConfig();await olderAlways;
assert.equal(element('btAlwaysWait').checked,true,'old configuration cannot undo any saved setting');

// A lost HTTP response must release the in-flight guard so polling recovers.
hangNextStatus=true;const lostPoll=evaluate('tick()');
const requestTimeout=[...timeoutCallbacks.values()].find(x=>x.ms===8000);
assert.ok(requestTimeout,'GET requests have an eight-second deadline');requestTimeout.callback();await lostPoll;
assert.equal(element('liveText').textContent,'Offline');assert.equal(evaluate('statusPromise'),null);
await evaluate('tick()');assert.equal(element('liveText').textContent,'Live','polling recovers after timeout');
invalidNextStatus=true;await evaluate('tick()');assert.equal(element('liveText').textContent,'Offline','malformed JSON cannot masquerade as live status');
await evaluate('tick()');

// Storage recovery is explicit and offered only for a mount failure in setup AP.
evaluate('renderStorageHealth({filesystem_available:true,config_storage_fault:false})');
assert.equal(element('storage-recovery-settings').hidden,true);
evaluate('renderStorageHealth({filesystem_available:true,config_storage_fault:true,ap_mode:true})');
assert.equal(element('storage-recovery-settings').hidden,false);assert.equal(element('storageResetButton').hidden,true,'save failures never offer erase');
evaluate('renderStorageHealth({filesystem_available:false,ap_mode:false})');
assert.equal(element('storageResetButton').hidden,true,'erase unavailable in station mode');
evaluate('renderStorageHealth({filesystem_available:false,ap_mode:true})');
assert.equal(element('storageResetButton').hidden,false);
confirmAnswer=false;const cancelStart=requests.length;await evaluate('resetSavedSettings($("storageResetButton"))');
assert.equal(requests.length,cancelStart,'cancelled destructive confirmation sends no request');
confirmAnswer=true;await evaluate('resetSavedSettings($("storageResetButton"))');
assert.deepEqual(requests.at(-1),{url:'/api/storage/reset',method:'POST',fields:{confirm:'ERASE_SAVED_SETTINGS'}});
assert.match(element('toast').textContent,/Reconnect to the RadioStation/);

evaluate('schedules=Array.from({length:24},()=>({station:0,start:0,end:60}))');
await evaluate('addSchedule()');assert.equal(evaluate('schedules.length'),24,'UI schedule count is bounded');
console.log('V4.15 R1 polling reliability checks passed: page/visibility gating, bounded/coalesced requests, timeout recovery, invalid JSON, out-of-order config, saved settings protection and explicit fault-only storage recovery.');

assert.equal(evaluate("formatUptime(450249)"),"125h 04m 09s");
assert.equal(evaluate("formatUptime(0)"),"0h 00m 00s");
assert.equal(evaluate("formatUptime(null)"),"Unavailable");
