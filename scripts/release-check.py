#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Check the source-only publication surface; execute no Wine or game binaries."""
import argparse
import hashlib
import json
import re
import subprocess
from pathlib import Path

project = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--history', action='store_true', help='Scan reachable history for private paths, key patterns and binaries')
parser.add_argument('--public-history', action='store_true', help='Require current branch history to omit local process artifacts')
args = parser.parse_args()
private_path = re.compile(rb'/(?:Users|home)/[A-Za-z0-9_.-]+/|/(?:private/)?var/folders/[A-Za-z0-9]+/')
credentials = re.compile(rb'-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----|\bAKIA[A-Z0-9]{16}\b|\b(?:ghp_|github_pat_)[A-Za-z0-9_]{30,}\b|\bsk-(?:proj-)?[A-Za-z0-9_-]{30,}\b')
media_names = {'test-wmv2.wmv', 'test-mpeg1.mpg', 'test-pcm.wav'}
forbidden = {'.exe', '.dll', '.paz', '.vff', '.iso', '.zip', '.tar', '.gz', '.xz', '.dmg'}

# Standalone helpers keep the license they were published with.
NON_LGPL_SOURCES = {'scripts/patch-eustia-vfs.py': b'SPDX-License-Identifier: MIT'}
PUBLIC_CONFIGS = {'build.env', 'crossover-patch-series.txt', 'crossover-source-lock.json', 'dependency-lock.json',
                  'source-license-inventory.json', 'source-lock.json', 'vulkan-generation-lock.json',
                  'linux-build.json'}
LOCAL_ROOTS = {'work', 'outputs', 'docs', 'plans', 'reports', 'logs'}
project_owned_prefixes = ('scripts/', 'native/', 'tools/probes/', 'tools/tests/', 'configs/', '.github/')
def upstream_tree_lines():
    lock = json.loads((project / 'configs/crossover-source-lock.json').read_text())
    refs = [lock.get('repository_baseline_commit'), 'vendor/crossover']
    for ref in filter(None, refs):
        try:
            return subprocess.check_output(['git', 'ls-tree', '-r', ref], cwd=project,
                                           text=True, stderr=subprocess.DEVNULL).splitlines()
        except subprocess.CalledProcessError:
            pass
    return []


upstream_entries = {}
for line in upstream_tree_lines():
    meta, name = line.split('\t', 1)
    upstream_entries[name] = meta.split()[2]
upstream_names = set(upstream_entries)

def pristine_upstream_file(name, data):
    oid = upstream_entries.get(name)
    if not oid:
        return False
    current = hashlib.sha1(b'blob ' + str(len(data)).encode() + b'\0' + data).hexdigest()
    return current == oid

def check_public_file(name):
    path = Path(name)
    assert path.parts[0] not in LOCAL_ROOTS, ('Local artifact in publication tree', name)
    if path.parts[0] == 'configs':
        assert path.name in PUBLIC_CONFIGS, ('Undeclared public configuration', name)
    assert not re.search(r'(^|/)(?:plan|implementation[-_]plan|test[-_]results|validation[-_]report)[-_ .]', name, re.I), ('Process document in publication tree', name)
    assert name != 'scripts/test-haruoto.sh', ('Game-specific launcher in publication tree', name)

def check_file(name, data):
    path = Path(name)
    pristine = pristine_upstream_file(name, data)
    if path.suffix.lower() in forbidden and not pristine:
        raise SystemExit(f'Binary/archive in publication tree: {name}')
    if (path.suffix.lower() in {'.wmv', '.mpg', '.wav', '.mp4', '.png', '.jpg'}
            and path.name not in media_names and not pristine):
        raise SystemExit(f'Media requiring provenance review: {name}')
    if not pristine and private_path.search(data):
        raise SystemExit(f'Private machine path in publication tree: {name}')
    if credentials.search(data):
        raise SystemExit(f'Credential pattern requires review: {name}')

tracked = subprocess.check_output(['git', 'ls-files', '-z'], cwd=project).decode().split('\0')
count = 0
for name in filter(None, tracked):
    file = project / name
    if not file.exists():
        continue
    data = file.read_bytes()
    check_public_file(name)
    check_file(name, data)
    if file.suffix in {'.py', '.sh', '.c'} and (name.startswith(project_owned_prefixes) or name == 'patch-eustia-vfs.py'):
        expected = NON_LGPL_SOURCES.get(name, b'SPDX-License-Identifier: LGPL-2.1-or-later')
        assert expected in data[:1000], name
    count += 1
md_files = [project / name for name in tracked if name and name.endswith('.md') and (project / name).is_file()]
for file in md_files:
    for link in re.findall(r'\]\(([^)]+)\)', file.read_text()):
        if link and not link.startswith(('http://', 'https://', '#')):
            assert (file.parent / link.split('#')[0]).exists(), (file.name, link)
inventory = json.loads((project / 'configs/source-license-inventory.json').read_text())
declared = {(item['patch'], item['source_file']) for item in inventory['files']}
actual = set()
for patch in (project / 'patches').rglob('*.patch'):
    assert hashlib.sha256(patch.read_bytes()).hexdigest() == inventory['patch_sha256'][str(patch.relative_to(project))], ('Patch changed since license review', patch.name)
    for name in re.findall(r'^(?:--- a/|\+\+\+ b/)(.+)$', patch.read_text(), flags=re.M):
        actual.add((str(patch.relative_to(project)), name))
assert declared == actual, ('License inventory coverage mismatch', actual - declared, declared - actual)
for item in inventory['files']:
    assert (project / item['patch']).exists(), item['patch']
assert (project / 'licenses/GPL-2.0.txt').exists()
assert (project / 'licenses/Linux-syscall-note').exists()
assert (project / 'COPYING.LIB').exists()
for notice_inventory in ('MODIFICATIONS.json', 'LINUX_MODIFICATIONS.json'):
    records = json.loads((project / notice_inventory).read_text())
    for item in records['files']:
        target = project / item['file']
        assert target.is_file(), (notice_inventory, item['file'])
        data = target.read_bytes()
        assert hashlib.sha256(data).hexdigest() == item['sha256'], (notice_inventory, item['file'])
        assert b'modification notice' in data, (notice_inventory, item['file'])
commits = []
if args.history or args.public_history:
    history_ref = 'HEAD' if args.public_history else '--all'
    commits = subprocess.check_output(['git', 'rev-list', history_ref], cwd=project, text=True).splitlines()
    seen = set()
    for commit in commits:
        tree = subprocess.check_output(['git', 'ls-tree', '-rz', commit], cwd=project)
        for entry in tree.split(b'\0'):
            if not entry:
                continue
            meta, raw_name = entry.split(b'\t', 1)
            _, kind, oid = meta.split()
            if kind != b'blob' or (oid, raw_name) in seen:
                continue
            seen.add((oid, raw_name))
            data = subprocess.check_output(['git', 'cat-file', 'blob', oid], cwd=project)
            if args.public_history:
                check_public_file(raw_name.decode())
            check_file(raw_name.decode(), data)
print(json.dumps({'tracked_files_checked': count, 'patch_file_pairs': len(inventory['files']),
                  'history_commits_checked': len(commits), 'result': 'passed',
                  'scope': 'Patterns and declared licenses; upstream ownership, all credential formats and target-device runtime require separate review'}, indent=2))
