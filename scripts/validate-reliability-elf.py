#!/usr/bin/env python3
"""Verify SDK Xtensa CALL8 dispatch to the strong SNTP pre-commit hook.

Some pinned SDK objects mark their text as data in Xtensa mapping metadata,
so objdump omits their decoded instructions. Read ELF LOAD bytes and decode
CALL8's signed18-bit word displacement to verify the actual linked call.
"""
from pathlib import Path
import os
import re
import struct
import subprocess
import sys

elf = Path(sys.argv[1])
base = Path(os.environ.get('RADIOCLOCK_TOOLS_DIR', '/workspace/.radioclock-tools')) / 'data/packages/esp32/tools/esp-x32/2601/bin'
nm = subprocess.check_output([str(base / 'xtensa-esp32-elf-nm'), '-S', str(elf)], text=True)
def symbol(name, kind):
    match = re.search(r'^([0-9a-f]+)\s+([0-9a-f]+)\s+' + kind + r'\s+' + name + r'$', nm, re.M)
    if not match: raise SystemExit('FAIL: missing expected ' + kind + ' symbol ' + name)
    return int(match[1], 16), int(match[2], 16)
hook, _ = symbol('sntp_sync_time', 'T')
caller, size = symbol('sntp_set_system_time', 'T')
data = elf.read_bytes()
if data[:6] != b'\x7fELF\x01\x01' or struct.unpack_from('<H', data, 18)[0] != 94:
    raise SystemExit('FAIL: expected little-endian ELF32 Xtensa release image')
phoff = struct.unpack_from('<I', data, 28)[0]
entry_size, entries = struct.unpack_from('<HH', data, 42)
raw = None
for index in range(entries):
    kind, offset, address, _, file_size, _, _, _ = struct.unpack_from('<IIIIIIII', data, phoff + index * entry_size)
    if kind == 1 and address <= caller and caller + size <= address + file_size:
        start = offset + caller - address
        raw = data[start:start+size]
        break
if raw is None: raise SystemExit('FAIL: SDK SNTP dispatch bytes missing from LOAD segments')
calls = []
for offset in range(len(raw)-2):
    instruction = int.from_bytes(raw[offset:offset+3], 'little')
    if instruction & 0x3f != 0x25: continue  # Xtensa CALL8 opcode.
    displacement = instruction >> 6
    if displacement & 0x20000: displacement -= 0x40000
    target = ((caller + offset + 4) & ~3) + (displacement << 2)
    if target == hook: calls.append(caller + offset)
if len(calls) != 1:
    raise SystemExit('FAIL: SDK SNTP dispatcher must contain exactly one CALL8 to the strong hook')
print(f'PASS: strong sntp_sync_time at 0x{hook:08x}; SDK sntp_set_system_time CALL8 at 0x{calls[0]:08x} dispatches to the pre-commit gate')
