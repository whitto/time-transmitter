#!/usr/bin/env python3
"""Verify the unique source-only ESP32 WebServer pin before install/package."""
import argparse
import hashlib
import json
from pathlib import Path

UPSTREAM_COMMIT='94afccf35fb1e401facddbcf9e13bcf7c76a31d8'

def validate(library):
    manifest=json.loads((library/'RADIOCLOCK_SOURCE_MANIFEST.json').read_text())
    assert manifest['upstream_version']=='3.3.12'
    assert manifest['upstream_commit']==UPSTREAM_COMMIT
    assert manifest['version']=='3.3.12-radioclock.1'
    originals=json.loads((library/'RADIOCLOCK_UPSTREAM_SHA256.json').read_text())
    assert originals['ref']=='3.3.12'
    assert originals['upstream']=='https://github.com/espressif/arduino-esp32'
    assert len(originals['upstream_paths'])==19
    files=manifest['files_sha256']
    inventory={p.relative_to(library).as_posix() for p in library.rglob('*') if p.is_file()}
    expected=set(files)|{'RADIOCLOCK_SOURCE_MANIFEST.json'}
    assert inventory==expected,f'Unreviewed/missing HTTP dependency files: {sorted(inventory^expected)}'
    for name,digest in files.items():
        assert not name.startswith('/') and '..' not in Path(name).parts
        assert hashlib.sha256((library/name).read_bytes()).hexdigest()==digest,f'Pinned HTTP source differs: {name}'
    for path in library.rglob('*'):
        assert path.suffix.lower() not in {'.a','.o','.so','.elf','.bin'},f'Binary HTTP library file: {path}'
    header=(library/'src/RadioBoundedWebServer.h').read_text()
    assert 'class RadioBoundedWebServer {' in header
    assert '#include "RadioHttpBounds.h"' in header
    assert '#define RADIO_HTTP_BOUNDS_VERSION 1' in (library/'src/RadioHttpBounds.h').read_text()
    assert not (library/'src/WebServer.h').exists()
    parser=(library/'src/Parsing.cpp').read_text()
    assert 'client.clear();' not in parser
    assert '::_parseForm(' not in parser
    assert 'radioHttpParseMultipart(' in parser and 'RADIO_HTTP_REQUEST_BUDGET_MS' in parser
    print(f'PASS: RadioBoundedWebServer 3.3.12-radioclock.1, {len(files)+1} verified source/metadata files')

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('library',type=Path,nargs='?',default=Path(__file__).resolve().parents[1]/'libraries/RadioBoundedWebServer')
    validate(parser.parse_args().library)
