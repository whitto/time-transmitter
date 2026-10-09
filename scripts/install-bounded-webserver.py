#!/usr/bin/env python3
"""Install the verified, separately named bounded server source library."""
from pathlib import Path
import os
import shutil
import subprocess
import sys

repo = Path(__file__).resolve().parents[1]
source = repo / 'libraries/RadioBoundedWebServer'
tools_dir = Path(os.environ.get('RADIOCLOCK_TOOLS_DIR', '/workspace/.radioclock-tools'))
destination = tools_dir / 'user/libraries/RadioBoundedWebServer'
validator = repo / 'scripts/validate-bounded-webserver-source.py'

subprocess.run([sys.executable, str(validator), str(source)], check=True)
if source.is_symlink() or any(item.is_symlink() for item in source.rglob('*')):
    raise SystemExit('Refusing a symlink in bounded-server source')
if destination.is_symlink():
    raise SystemExit('Refusing a symlink at bounded-server destination')
if destination.exists():
    shutil.rmtree(destination)
destination.parent.mkdir(parents=True, exist_ok=True)
shutil.copytree(source, destination)
subprocess.run([sys.executable, str(validator), str(destination)], check=True)
print('Installed verified RadioBoundedWebServer 3.3.12-radioclock.1 source')
