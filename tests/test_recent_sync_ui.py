#!/usr/bin/env python3
"""Execute the actual recent-sync/uptime UI functions without the full UI suite."""
import re
import subprocess
import unittest
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
UI=ROOT/'ui/radioclock.html'

class RecentSyncUiTest(unittest.TestCase):
    def run_js(self,names,driver):
        source=UI.read_text()
        # These shipped handlers are intentionally single-line declarations;
        # extracting complete lines preserves JavaScript regex/template syntax.
        functions='\n'.join(next(line for line in source.splitlines() if line.startswith('function '+name+'(')) for name in names)
        prelude=r'''
const assert=require('node:assert/strict');const elements=new Map();
const $=id=>{if(!elements.has(id))elements.set(id,{textContent:'',innerHTML:'',value:''});return elements.get(id)};
let config={};const watchProfiles=[{id:0,name:'Watch 1'},{id:1,name:'Watch 2'},{id:2,name:'Watch 3'},{id:3,name:'Watch 4'}];
const protocols=[{id:0,name:'GW-BX5600 MIP'},{id:1,name:'Standard digital / hybrid'},{id:2,name:'Analogue time-only'}];
const pendingSettings=new Set(),wifiAccessKeys=[];
function renderSelectedWatch(){renderRecentSyncs()}
function renderBtHistory(){}function renderWifiAccess(){}function renderCrashDump(){}function renderPowerMode(){}
function renderActivityLed(){}function renderBtPowerSave(){}function renderWatchFont(){}
'''
        result=subprocess.run(['node','-e',prelude+'\n'+functions+'\n'+driver],capture_output=True,text=True)
        self.assertEqual(result.returncode,0,result.stderr)
        return result.stdout

    def test_recent_success_failure_and_bounded_safe_rows(self):
        driver=r'''
config={bt_manual_profile:0,bt_profiles:[{id:0,name:'Bedroom watch'}],bt_recent_sync_profile:0,
 bt_recent_syncs:[
 {utc_epoch:1791500000,date:'2026-10-09 18:30:20',protocol:0,successful:true},
 {utc_epoch:1791400000,date:'2026-10-09 12:30:11',protocol:1,successful:false},
 {utc_epoch:0,date:'',protocol:2,successful:false},
 {utc_epoch:1791300000,date:'<img src=x onerror=alert(1)>',protocol:99,successful:true},
 {utc_epoch:1791200000,date:'must not be shown',protocol:0,successful:true}]};
renderRecentSyncs();const rows=$('btRecentSyncList').innerHTML;
assert.equal((rows.match(/<div>/g)||[]).length,4);assert.equal((rows.match(/>Successful</g)||[]).length,2);
assert.equal((rows.match(/>Failed</g)||[]).length,2);assert.ok(rows.indexOf('18:30:20')<rows.indexOf('12:30:11'));
assert.match(rows,/GW-BX5600 MIP/);assert.match(rows,/Standard digital/);assert.match(rows,/Analogue time-only/);
assert.match(rows,/Time unavailable/);assert.match(rows,/Unknown protocol/);assert.match(rows,/&lt;img src=x/);
assert.ok(!rows.includes('<img')&&!rows.includes('must not be shown'));
assert.equal($('btRecentSyncWatch').textContent,'Last four BT syncs · Bedroom watch');
config.bt_recent_syncs=[null,{}, {successful:'true',date:'invalid flag'}];renderRecentSyncs();
assert.match($('btRecentSyncList').innerHTML,/No BT syncs since restart/);
console.log('Recent successful/failed sync rows, ordering, four-entry bound and escaping PASS');
'''
        self.assertIn('four-entry bound and escaping PASS',self.run_js(['esc','selectedProfile','renderRecentSyncs'],driver))

    def test_profile_races_restart_and_snapshot_separation(self):
        driver=r'''
config={bt_manual_profile:1,bt_profiles:[{id:1,name:'Second watch'}],bt_recent_sync_profile:0,
 bt_recent_syncs:[{date:'old-watch-only',protocol:0,successful:true}],bt_history_saved_at:'saved snapshot date'};
renderRecentSyncs();assert.match($('btRecentSyncList').innerHTML,/No BT syncs since restart/);
assert.ok(!$('btRecentSyncList').innerHTML.includes('old-watch-only'));assert.match($('btRecentSyncWatch').textContent,/Second watch/);
pendingSettings.add('bt_manual_profile');mergeStatusSettings({bt_manual_profile:0,bt_recent_sync_profile:0,
 bt_recent_syncs:[{date:'late-old-profile',protocol:0,successful:false}]});
assert.equal(config.bt_manual_profile,1);assert.ok(!$('btRecentSyncList').innerHTML.includes('late-old-profile'));
pendingSettings.delete('bt_manual_profile');mergeStatusSettings({bt_manual_profile:1,bt_recent_sync_profile:1,
 bt_recent_syncs:[{date:'matching selected watch',protocol:0,successful:false}]});
assert.match($('btRecentSyncList').innerHTML,/matching selected watch/);
// The profile and records are merged together; partial payloads cannot relabel another watch's rows.
mergeStatusSettings({bt_recent_sync_profile:0});assert.equal(config.bt_recent_sync_profile,1);
mergeStatusSettings({bt_recent_syncs:[{date:'unpaired array',successful:true}]});assert.ok(!$('btRecentSyncList').innerHTML.includes('unpaired array'));
mergeStatusSettings({bt_recent_sync_profile:1,bt_recent_syncs:[]});
assert.match($('btRecentSyncList').innerHTML,/No BT syncs since restart/);
assert.ok(!$('btRecentSyncList').innerHTML.includes('saved snapshot date'));
console.log('Selected-profile races, atomic history payload and restart-empty snapshot separation PASS');
'''
        self.assertIn('restart-empty snapshot separation PASS',self.run_js(['esc','selectedProfile','renderRecentSyncs','mergeStatusSettings'],driver))

    def test_shared_overview_diagnostics_uptime_and_existing_layout(self):
        driver=r'''
for(const [seconds,expected] of [[0,'0h 00m 00s'],[59,'0h 00m 59s'],[60,'0h 01m 00s'],
 [259449,'72h 04m 09s'],[450249,'125h 04m 09s'],[6307200000,'1752000h 00m 00s']]){
renderHealthSummary({uptime_sec:seconds});assert.equal($('quickUptime').textContent,'Uptime: '+expected);assert.equal(formatUptime(seconds),expected)}
for(const invalid of [null,-1,NaN,Infinity]){renderHealthSummary({uptime_sec:invalid});assert.equal($('quickUptime').textContent,'Uptime: Unavailable')}
console.log('Overview/Diagnostics shared hours-minutes-seconds, long uptime and invalid data PASS');
'''
        self.assertIn('long uptime and invalid data PASS',self.run_js(['formatUptime','renderHealthSummary'],driver))
        source=UI.read_text()
        self.assertEqual(source.count('id="btRecentSyncList"'),1)
        self.assertIn("['Uptime',formatUptime(d.uptime_sec)]",source)
        self.assertRegex(source,r'Last watch result.*id="btRecentSyncWatch".*id="watch-battery-reading"')
        navigation=re.findall(r'<button[^>]*class="navbtn[^>]*data-view="([^"]+)"',source)
        self.assertEqual(navigation,['home','radio','watch','schedules','network','settings','advanced','about'])

if __name__=='__main__':unittest.main()
