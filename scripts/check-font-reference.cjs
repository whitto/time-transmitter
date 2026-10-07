#!/usr/bin/env node
// Validate the packet fixtures against the actual pinned GitHub web app encoder.
// Usage: node scripts/check-font-reference.cjs /path/to/gshock-smart-sync-webapp
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const upstream = process.argv[2];
assert.ok(upstream, 'Pass the pinned GitHub web app checkout path.');
const source = fs.readFileSync(path.join(upstream, 'src/api/io/SettingsIO.ts'), 'utf8');
let code = source.slice(source.indexOf('const MASK_24_HOURS'), source.indexOf('let resolver:'));
code = code.replace('export const SettingsIOFunctional', 'const SettingsIOFunctional')
  .replace('encode(settings: Settings): number[]', 'encode(settings)')
  .replace('decode(data: number[]): Settings', 'decode(data)')
  .replace('const settings: Settings', 'const settings')
  .replace('const langMap: Record<number, Settings["language"]>', 'const langMap');
const context = { watchInfo: { longLightDuration: '3s', shortLightDuration: '1.5s' },
  CasioConstants: { CHARACTERISTICS: { CASIO_SETTING_FOR_BASIC: 0x13 } } };
vm.createContext(context);
vm.runInContext(code + '\nglobalThis.codec = SettingsIOFunctional;', context);
const inputs = { timeFormat: '24h', buttonTone: true, autoLight: true,
  powerSavingMode: true, DnD: true, lightDuration: '3s', dateFormat: 'DD:MM',
  language: 'French', keyVibration: true, hourlyChime: true };
const packets = {};
for (const font of ['Classic', 'Standard']) {
  const encoded = context.codec.encode({ ...inputs, font });
  assert.equal(encoded.length, 17);
  assert.equal(context.codec.decode(encoded).font, font);
  packets[font] = Buffer.from(encoded).toString('hex');
}
const fixture = JSON.parse(fs.readFileSync(path.join(__dirname, '../tests/fixtures/watch-font.json')));
assert.deepEqual(packets, fixture.packets);
const constants = fs.readFileSync(path.join(upstream, 'src/api/CasioConstants.ts'), 'utf8');
assert.ok(constants.includes("CASIO_READ_REQUEST_FOR_ALL_FEATURES_CHARACTERISTIC_UUID: '26eb002c-b012-49a8-b1f8-394fb2032b0f'"));
assert.ok(constants.includes("CASIO_ALL_FEATURES_CHARACTERISTIC_UUID: '26eb002d-b012-49a8-b1f8-394fb2032b0f'"));
console.log('PASS: actual GitHub web app encoder/decoder and characteristic mapping match Classic/Standard fixtures');
