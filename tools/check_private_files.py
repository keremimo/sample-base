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

# Images a page of the docs shows (screenshots, diagrams) are binary by nature:
# allowed under a docs/images/ folder when they really are one of these formats,
# still under the size limit and scanned like any other file.
DOC_IMAGE = re.compile(rb'(?:^|/)docs/images/.+\.(?:png|jpe?g|gif|webp)$', re.IGNORECASE)
IMAGE_HEADS = (b'\x89PNG\r\n\x1a\n', b'\xff\xd8\xff', b'GIF87a', b'GIF89a')


# A kernel module is binary too: allowed as an ELF object under kernel/<name>/
# beside the C source it is built from (tools/build_kernel_modules.sh): its
# own .c, or for a module of several files the folder's Kbuild and C sources.
# Still under the size limit and scanned like any other file.
KERNEL_MODULE = re.compile(rb'(?:^|/)kernel/[^/]+/[^/]+\.ko$')
# A module's C may be kernel code copied with its notices (kernel/uvc/), whose
# authors' e-mail addresses stay as the kernel has them: the e-mail rule
# skips C sources under kernel/<name>/, every other rule still applies.
KERNEL_SOURCE = re.compile(rb'(?:^|/)kernel/[^/]+/[^/]+\.[ch]$')


def kernel_module(name, data, indexed):
    if not KERNEL_MODULE.search(name) or not data.startswith(b'\x7fELF'):
        return False
    if name[:-3] + b'.c' in indexed:
        return True
    folder = name[:name.rfind(b'/') + 1]
    return folder + b'Kbuild' in indexed and any(i.startswith(folder) and i.endswith(b'.c') for i in indexed)


def doc_image(name, data):
    if not DOC_IMAGE.search(name):
        return False
    return data.startswith(IMAGE_HEADS) or (data[:4] == b'RIFF' and data[8:12] == b'WEBP')


def main():
    root = Path(git('rev-parse', '--show-toplevel').decode().strip())
    entries = git('-C', str(root), 'ls-files', '--stage', '-z').split(b'\0')
    indexed = {}
    errors = []
    # A submodule named in .gitmodules is a commit of another repository,
    # checked by that repository's own hook (an app keeps sample-base so).
    submodules = set()
    if (root / '.gitmodules').is_file():
        listed = subprocess.run(['git', '-C', str(root), 'config', '-f', '.gitmodules', '--get-regexp', r'\.path$'],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        submodules = {line.split(b' ', 1)[1] for line in listed.stdout.splitlines() if b' ' in line}
    for entry in filter(None, entries):
        metadata, name = entry.split(b'\t', 1)
        mode, blob, stage = metadata.split()
        if stage != b'0':
            errors.append((name, 'unmerged index entry'))
            continue
        if mode == b'160000' and name in submodules:
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
        if b'\0' in data and not doc_image(name, data) and not kernel_module(name, data, indexed):
            errors.append((name, 'binary data is not source text'))
        for reason, pattern in PATTERNS.items():
            if reason == 'email address' and KERNEL_SOURCE.search(name):
                continue # kernel authors' public addresses in the notices copied with their code
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
