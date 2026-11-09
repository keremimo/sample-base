#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 MPC Sample Plugins contributors
"""Check the Git index for private files; never print matching content."""
from pathlib import Path
import re
import subprocess
import sys


def git(*args, input=None):
    return subprocess.run(['git', *args], input=input, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, check=True).stdout


PATTERNS = {
    'private key': re.compile(rb'-----BEGIN [A-Z0-9 ]{0,48}PRIVATE KEY-----'),
    'SSH public identity': re.compile(rb'(?:ssh-(?:rsa|dss|ed25519)|ecdsa-sha2-\S+)\s+AAAA[A-Za-z0-9+/]{24,}'),
    'GitHub token': re.compile(rb'\b(?:gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{30,})'),
    'cloud access key': re.compile(rb'\b(?:AKIA|ASIA)[A-Z0-9]{16}\b'),
    'personal home path': re.compile(rb'/(?:Users|home)/[^/\s]+/'),
    'email address': re.compile(rb'\b[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}\b'),
}


def main():
    root = Path(git('rev-parse', '--show-toplevel').decode().strip())
    entries = git('-C', str(root), 'ls-files', '--stage', '-z').split(b'\0')
    indexed = {}
    errors = []
    for entry in filter(None, entries):
        metadata, name = entry.split(b'\t', 1)
        mode, blob, stage = metadata.split()
        if stage != b'0':
            errors.append((name, 'unmerged index entry'))
            continue
        if mode not in (b'100644', b'100755'):
            errors.append((name, 'symlink or submodule is not source text'))
            continue
        indexed[name] = blob
    if not indexed:
        print('No staged source files to check.')
        return 1 if errors else 0
    ignored = subprocess.run(['git', '-C', str(root), 'check-ignore', '--no-index', '-z', '--stdin'],
                             input=b'\0'.join(indexed)+b'\0', stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE)
    if ignored.returncode not in (0, 1):
        raise RuntimeError('Could not verify ignore rules')
    for name in filter(None, ignored.stdout.split(b'\0')):
        errors.append((name, 'ignored/private/generated path is staged'))
    for name, blob in indexed.items():
        size = int(git('-C', str(root), 'cat-file', '-s', blob.decode()))
        if size > 2*1024*1024:
            errors.append((name, 'file exceeds source-only size limit'))
            continue
        data = git('-C', str(root), 'cat-file', 'blob', blob.decode())
        if b'\0' in data:
            errors.append((name, 'binary data is not source text'))
        for reason, pattern in PATTERNS.items():
            if pattern.search(data):
                errors.append((name, reason))
    for name, reason in errors:
        print(f'BLOCKED {name.decode(errors="replace")}: {reason}', file=sys.stderr)
    if errors:
        print('Remove or sanitize the listed files before committing.', file=sys.stderr)
        return 1
    print(f'Privacy check passed: {len(indexed)} indexed text files; no ignored files or detected secrets.')
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError):
        print('Privacy check failed to run; commit blocked.', file=sys.stderr)
        sys.exit(1)
