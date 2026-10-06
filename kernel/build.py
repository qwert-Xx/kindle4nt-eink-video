#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Build Linux with the locked public Kindle patches and local WB series, offline."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys


def patches(directory):
    return [directory / line.strip() for line in
            (directory / 'series').read_text().splitlines()
            if line.strip() and not line.lstrip().startswith('#')]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--linux', type=Path, required=True)
    parser.add_argument('--kindle4nt', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    lock = dict(line.split('=', 1) for line in
                (here / 'upstream.lock').read_text().splitlines()
                if line and not line.startswith('#'))
    linux, kindle4nt, out = args.linux.resolve(), args.kindle4nt.resolve(), args.out.resolve()
    head = subprocess.check_output(['git', '-C', str(kindle4nt), 'rev-parse', 'HEAD'],
                                   text=True).strip()
    if head != lock['kindle4nt_commit']:
        raise ValueError('kindle4nt HEAD does not match upstream.lock')
    version = dict(line.split('=', 1) for line in (linux / 'Makefile').read_text().splitlines()
                   if '=' in line and line.split('=', 1)[0].strip() in
                   ('VERSION', 'PATCHLEVEL', 'SUBLEVEL', 'EXTRAVERSION'))
    version = {key.strip(): value.strip() for key, value in version.items()}
    release = 'v' + '.'.join(version[key] for key in ('VERSION', 'PATCHLEVEL', 'SUBLEVEL'))
    if release != lock['linux_version'] or version['EXTRAVERSION']:
        raise ValueError('Linux Makefile version does not match upstream.lock')
    series = patches(kindle4nt / 'kernel/patches') + patches(here / 'patches')
    signature = {'lock': lock, 'patches': [
        {'name': p.name, 'sha256': hashlib.sha256(p.read_bytes()).hexdigest()} for p in series]}
    out.mkdir(parents=True, exist_ok=True)
    tree, state_path = out / 'src', out / 'source-state.json'
    state = json.loads(state_path.read_text()) if state_path.exists() else {}
    if tree.exists() and state == signature:
        print('Reusing source tree: ' + str(tree), flush=True)
    else:
        state_path.unlink(missing_ok=True)
        if tree.exists():
            shutil.rmtree(tree)
        print('Copying Linux source to: ' + str(tree), flush=True)
        shutil.copytree(linux, tree, symlinks=True, ignore=shutil.ignore_patterns('.git'))
        # Apply in this copy without discovering a surrounding Git repository.
        for patch in series:
            print('Applying ' + patch.name, flush=True)
            subprocess.run(['git', 'apply', str(patch)], cwd=tree,
                           env=dict(os.environ, GIT_CEILING_DIRECTORIES=str(out)), check=True)
        state_path.write_text(json.dumps(signature, indent=2) + '\n')
    subprocess.run([sys.executable, '-B', str(kindle4nt / 'project/build.py'), '--source', str(tree),
                    '--out', str(out / 'kernel')], check=True)


if __name__ == '__main__':
    main()
