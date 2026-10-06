#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Build the locked kernel with the ordered local patch series, offline."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys


def git(source, *args):
    return subprocess.check_output(['git', '-C', str(source), *args], text=True).strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel-src', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    lock = dict(line.split('=', 1) for line in
                (here / 'mainline.lock').read_text().splitlines() if line and not line.startswith('#'))
    source, out = args.kernel_src.resolve(), args.out.resolve()
    commit = git(source, 'rev-parse', '--verify', lock['commit'] + '^{commit}')
    patches = [here / 'patches' / line.strip() for line in
               (here / 'patches/series').read_text().splitlines()
               if line.strip() and not line.lstrip().startswith('#')]
    signature = [hashlib.sha256(p.read_bytes()).hexdigest() for p in patches]
    out.mkdir(parents=True, exist_ok=True)
    tree, state_path = out / 'src', out / 'source-state.json'
    state = json.loads(state_path.read_text()) if state_path.exists() else {}
    if tree.exists():
        head = git(tree, 'rev-parse', 'HEAD')
        relation = subprocess.run(['git', '-C', str(tree), 'merge-base', commit, head],
                                  text=True, stdout=subprocess.PIPE, check=False)
        base = relation.stdout.strip()
        count = int(git(tree, 'rev-list', '--count', commit + '..' + head))
        clean = not git(tree, 'status', '--porcelain', '--untracked-files=no')
        valid = (clean and base == commit and
                 ((head == commit and not patches) or
                  (state.get('commit') == commit and state.get('patches') == signature and
                   state.get('head') == head and count == len(patches))))
        if not valid:
            subprocess.run(['git', '-C', str(source), 'worktree', 'remove', '--force', str(tree)], check=True)
    if not tree.exists():
        subprocess.run(['git', '-C', str(source), 'worktree', 'add', '--detach', str(tree), commit], check=True)
        if patches:
            subprocess.run(['git', '-C', str(tree), '-c', 'user.name=codex',
                            '-c', 'user.email=codex@localhost', 'am', *map(str, patches)], check=True)
    state_path.write_text(json.dumps({'commit': commit, 'patches': signature,
                                     'head': git(tree, 'rev-parse', 'HEAD')}, indent=2) + '\n')
    print('Retaining detached source worktree: ' + str(tree), flush=True)
    subprocess.run([sys.executable, str(tree / 'project/build.py'), '--source', str(tree),
                    '--out', str(out / 'kernel')], check=True)


if __name__ == '__main__':
    main()
